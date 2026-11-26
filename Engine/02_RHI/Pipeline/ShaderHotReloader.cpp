// ============================================================================
// 模块：ShaderHotReloader（开发期单线程按键 Shader 热重载器 · 实现）
// 架构层：Engine/02_RHI/Pipeline（契约 7.2 / WP-3B）
// ============================================================================
#include "00_Core/PCH.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"

#include "00_Core/Utility/Utility.h"

#include <directx-dxc/dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace Graphics
{
	namespace
	{
		uint64_t QueryFileWriteTimeU64(const std::wstring& path)
		{
			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
				return 0;
			ULARGE_INTEGER ui = {};
			ui.LowPart  = data.ftLastWriteTime.dwLowDateTime;
			ui.HighPart = data.ftLastWriteTime.dwHighDateTime;
			return ui.QuadPart;
		}

		std::wstring NormalizePathSlashes(std::wstring p)
		{
			std::replace(p.begin(), p.end(), L'/', L'\\');
			return p;
		}

		std::wstring JoinPath(const std::wstring& base, const std::wstring& rel)
		{
			if (base.empty()) return NormalizePathSlashes(rel);
			std::wstring out = NormalizePathSlashes(base);
			if (!out.empty() && out.back() != L'\\')
				out.push_back(L'\\');
			out += NormalizePathSlashes(rel);
			return out;
		}

		std::wstring ParentDirectory(const std::wstring& fullPath)
		{
			const size_t pos = fullPath.find_last_of(L"\\/");
			return (pos == std::wstring::npos) ? std::wstring() : fullPath.substr(0, pos);
		}

		void CollectIncludesRecursive(const std::wstring& filePath,
			const std::wstring& shaderRoot,
			std::set<std::wstring>& visited,
			uint64_t& maxStamp)
		{
			const std::wstring norm = NormalizePathSlashes(filePath);
			if (!visited.insert(norm).second)
				return;

			const uint64_t stamp = QueryFileWriteTimeU64(norm);
			if (stamp > maxStamp)
				maxStamp = stamp;

			std::ifstream ifs(norm, std::ios::in);
			if (!ifs.is_open())
				return;

			const std::wstring curDir = ParentDirectory(norm);
			std::string line;
			while (std::getline(ifs, line))
			{
				const size_t incPos = line.find("#include");
				if (incPos == std::string::npos)
					continue;
				// 忽略单行注释后的 #include
				const size_t cmtPos = line.find("//");
				if (cmtPos != std::string::npos && cmtPos < incPos)
					continue;
				const size_t q1 = line.find('"', incPos);
				if (q1 == std::string::npos)
					continue;
				const size_t q2 = line.find('"', q1 + 1);
				if (q2 == std::string::npos || q2 <= q1 + 1)
					continue;

				const std::string relAscii = line.substr(q1 + 1, q2 - q1 - 1);
				std::wstring relW(relAscii.begin(), relAscii.end());

				std::wstring candidate = JoinPath(curDir, relW);
				if (QueryFileWriteTimeU64(candidate) == 0 && !shaderRoot.empty())
					candidate = JoinPath(shaderRoot, relW);

				if (QueryFileWriteTimeU64(candidate) != 0)
					CollectIncludesRecursive(candidate, shaderRoot, visited, maxStamp);
			}
		}

		bool EndsWithW(const std::wstring& str, const wchar_t* suffix)
		{
			const std::wstring s(suffix);
			return str.size() >= s.size() &&
				str.compare(str.size() - s.size(), s.size(), s) == 0;
		}

		bool StartsWithW(const std::wstring& str, const wchar_t* prefix)
		{
			const std::wstring p(prefix);
			return str.size() >= p.size() && str.compare(0, p.size(), p) == 0;
		}
	}

	bool ShaderHotReloader::Initialize(const std::wstring& repoRootDir, bool enabled)
	{
		Shutdown();

#ifdef TOOIYUME_SHIPPING
		(void)repoRootDir;
		(void)enabled;
		m_Enabled = false;
		LOG_INFO(L"[HotReload] TOOIYUME_SHIPPING 已定义，开发期热重载器永久禁用\n");
		return true;
#else
		m_RepoRootDir = NormalizePathSlashes(repoRootDir);
		m_ShaderIncludeRoot = JoinPath(m_RepoRootDir, L"Engine\\Shader");
		m_Enabled = enabled;

		if (!m_Enabled)
		{
			LOG_INFO(L"[HotReload] 已自动禁用（-Experiment / -Screenshot 批跑模式，保证零磁盘 IO 与计时纯净）\n");
			return true;
		}

		// 动态加载部署在 exe 同目录（由 DeployRuntimeDlls.cmake 部署）的 dxcompiler.dll
		m_DxcModule = ::LoadLibraryW(L"dxcompiler.dll");
		if (m_DxcModule == nullptr)
		{
			LOG_ERROR(L"[HotReload] LoadLibraryW(dxcompiler.dll) 失败，F5 热重载不可用\n");
			m_Enabled = false;
			return false;
		}

		m_DxcCreateInstanceFn = reinterpret_cast<void*>(::GetProcAddress(m_DxcModule, "DxcCreateInstance"));
		if (m_DxcCreateInstanceFn == nullptr)
		{
			LOG_ERROR(L"[HotReload] GetProcAddress(DxcCreateInstance) 失败，F5 热重载不可用\n");
			::FreeLibrary(m_DxcModule);
			m_DxcModule = nullptr;
			m_Enabled = false;
			return false;
		}

		m_DxcReady = true;
		LOG_INFO(L"[HotReload] 已就绪（单线程 F5/Shift+F5，IncludeRoot=%ws）\n", m_ShaderIncludeRoot.c_str());
		return true;
#endif
	}

	void ShaderHotReloader::Shutdown()
	{
		m_Entries.clear();
		m_DxcCreateInstanceFn = nullptr;
		if (m_DxcModule != nullptr)
		{
			::FreeLibrary(m_DxcModule);
			m_DxcModule = nullptr;
		}
		m_DxcReady = false;
		m_Enabled  = false;
	}

	void ShaderHotReloader::ClearRegistrations()
	{
		m_Entries.clear();
	}

	void ShaderHotReloader::Register(ShaderHotReloadDesc desc)
	{
		if (desc.Id.empty() || desc.SourceRelPath.empty() || !desc.RebuildPipeline)
			return;

		// 重复注册同 Id 时覆盖更新
		for (auto& existing : m_Entries)
		{
			if (existing.Desc.Id == desc.Id)
			{
				existing.Desc = std::move(desc);
				existing.FullPath = JoinPath(m_RepoRootDir, existing.Desc.SourceRelPath);
				existing.LastWriteStamp = ComputeRecursiveWriteStamp(existing.FullPath);
				return;
			}
		}

		RegisteredEntry entry;
		entry.FullPath = JoinPath(m_RepoRootDir, desc.SourceRelPath);
		entry.LastWriteStamp = ComputeRecursiveWriteStamp(entry.FullPath);
		entry.Desc = std::move(desc);
		m_Entries.push_back(std::move(entry));
	}

	uint64_t ShaderHotReloader::ComputeRecursiveWriteStamp(const std::wstring& fullFilePath) const
	{
		std::set<std::wstring> visited;
		uint64_t maxStamp = 0;
		CollectIncludesRecursive(fullFilePath, m_ShaderIncludeRoot, visited, maxStamp);
		return maxStamp;
	}

	bool ShaderHotReloader::CompileInMemory(const RegisteredEntry& entry,
		std::vector<uint8_t>& outDxil, std::string& outErr)
	{
		outDxil.clear();
		outErr.clear();

		if (!m_DxcReady || m_DxcCreateInstanceFn == nullptr)
		{
			outErr = "DXC compiler runtime not initialized";
			return false;
		}

		auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(m_DxcCreateInstanceFn);

		ComPtr<IDxcUtils> utils;
		ComPtr<IDxcCompiler3> compiler;
		HRESULT hr = createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
		if (FAILED(hr))
		{
			outErr = "DxcCreateInstance(IDxcUtils) failed";
			return false;
		}
		hr = createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
		if (FAILED(hr))
		{
			outErr = "DxcCreateInstance(IDxcCompiler3) failed";
			return false;
		}

		ComPtr<IDxcIncludeHandler> includeHandler;
		hr = utils->CreateDefaultIncludeHandler(&includeHandler);
		if (FAILED(hr))
		{
			outErr = "IDxcUtils::CreateDefaultIncludeHandler failed";
			return false;
		}

		// 从磁盘读取最新源码到内存 Blob
		ComPtr<IDxcBlobEncoding> sourceBlob;
		hr = utils->LoadFile(entry.FullPath.c_str(), nullptr, &sourceBlob);
		if (FAILED(hr) || sourceBlob == nullptr)
		{
			outErr = "Failed to read HLSL source file for hot reload: " + entry.Desc.Id;
			return false;
		}

		DxcBuffer sourceBuf = {};
		sourceBuf.Ptr      = sourceBlob->GetBufferPointer();
		sourceBuf.Size     = sourceBlob->GetBufferSize();
		sourceBuf.Encoding = DXC_CP_ACP;
		BOOL knownEnc = FALSE;
		UINT32 cp = 0;
		if (SUCCEEDED(sourceBlob->GetEncoding(&knownEnc, &cp)) && knownEnc)
			sourceBuf.Encoding = cp;

		// 编译参数与 Engine/20_cmake/ShaderCompiler.cmake 严格对齐
		std::vector<std::wstring> argStorage;
		argStorage.push_back(entry.FullPath);
		if (!entry.Desc.EntryPoint.empty() && !StartsWithW(entry.Desc.TargetProfile, L"lib_"))
		{
			argStorage.push_back(L"-E");
			argStorage.push_back(entry.Desc.EntryPoint);
		}
		argStorage.push_back(L"-T");
		argStorage.push_back(entry.Desc.TargetProfile);
		argStorage.push_back(L"-I");
		argStorage.push_back(m_ShaderIncludeRoot);

		if (EndsWithW(entry.Desc.TargetProfile, L"_6_6") || EndsWithW(entry.Desc.TargetProfile, L"_6_7"))
		{
			argStorage.push_back(L"-HV");
			argStorage.push_back(L"2021");
			argStorage.push_back(L"-enable-16bit-types");
		}
#if defined(_DEBUG)
		argStorage.push_back(L"-Zi");
		argStorage.push_back(L"-Qembed_debug");
		argStorage.push_back(L"-Od");
#else
		argStorage.push_back(L"-O3");
#endif
		for (const auto& def : entry.Desc.Defines)
		{
			argStorage.push_back(L"-D");
			argStorage.push_back(def);
		}

		std::vector<LPCWSTR> args;
		args.reserve(argStorage.size());
		for (const auto& s : argStorage)
			args.push_back(s.c_str());

		ComPtr<IDxcResult> result;
		hr = compiler->Compile(&sourceBuf, args.data(), static_cast<UINT32>(args.size()),
			includeHandler.Get(), IID_PPV_ARGS(&result));
		if (FAILED(hr) || result == nullptr)
		{
			outErr = "IDxcCompiler3::Compile failed with HRESULT";
			return false;
		}

		ComPtr<IDxcBlobUtf8> errors;
		if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
			errors != nullptr && errors->GetStringLength() > 0)
		{
			outErr.assign(errors->GetStringPointer(), errors->GetStringLength());
		}

		HRESULT compileStatus = S_OK;
		result->GetStatus(&compileStatus);
		if (FAILED(compileStatus))
		{
			if (outErr.empty())
				outErr = "DXC compilation failed (no diagnostic message)";
			return false;
		}

		ComPtr<IDxcBlob> objectBlob;
		hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objectBlob), nullptr);
		if (FAILED(hr) || objectBlob == nullptr || objectBlob->GetBufferSize() == 0)
		{
			outErr = "DXC succeeded but DXIL object blob is empty";
			return false;
		}

		const auto* bytes = static_cast<const uint8_t*>(objectBlob->GetBufferPointer());
		outDxil.assign(bytes, bytes + objectBlob->GetBufferSize());
		return true;
	}

	ShaderReloadReport ShaderHotReloader::ReloadModified(bool forceAll)
	{
		ShaderReloadReport report;
		if (!IsEnabled())
		{
			report.Summary = "Hot reload disabled";
			return report;
		}

		for (auto& entry : m_Entries)
		{
			++report.Checked;
			const uint64_t currentStamp = ComputeRecursiveWriteStamp(entry.FullPath);
			if (!forceAll && currentStamp != 0 && currentStamp == entry.LastWriteStamp)
			{
				++report.Skipped;
				continue;
			}

			std::vector<uint8_t> dxil;
			std::string err;
			if (!CompileInMemory(entry, dxil, err))
			{
				++report.Failed;
				report.LastError = "[" + entry.Desc.Id + "] " + err;
				// 即使失败也更新时间戳？不更新：允许用户再按 F5 或修复后再次触发
				LOG_ERROR(L"[HotReload] DXC 编译失败 (%hs)，原 PSO 保留不变：\n%hs\n",
					entry.Desc.Id.c_str(), err.c_str());
				continue;
			}

			std::string rebuildErr;
			if (!entry.Desc.RebuildPipeline(dxil, rebuildErr))
			{
				++report.Failed;
				report.LastError = "[" + entry.Desc.Id + "] PSO rebuild failed: " + rebuildErr;
				LOG_ERROR(L"[HotReload] 候选 PSO 重建失败 (%hs)，原 PSO 保留不变：%hs\n",
					entry.Desc.Id.c_str(), rebuildErr.c_str());
				continue;
			}

			entry.LastWriteStamp = currentStamp;
			++report.Succeeded;
			LOG_INFO(L"[HotReload] 成功热替换 PSO：%hs (%zu bytes DXIL)\n",
				entry.Desc.Id.c_str(), dxil.size());
		}

		char buf[160] = {};
		std::snprintf(buf, sizeof(buf), "Reloaded %u, failed %u, unchanged %u (total %u)",
			report.Succeeded, report.Failed, report.Skipped, report.Checked);
		report.Summary = buf;
		return report;
	}
}
