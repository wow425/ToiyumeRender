#include "00_Core/PCH.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "00_Core/Utility/Utility.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace Asset
{
	std::wstring g_AssetsRoot;

	namespace
	{
		// 把路径规范成 "X:\\dir\\sub\\" 形式（统一分隔符 + 保证尾部有反斜杠）
		std::wstring NormalizeRoot(const fs::path& p)
		{
			std::wstring s = p.wstring();
			for (auto& c : s)
				if (c == L'/') c = L'\\';
			if (!s.empty() && s.back() != L'\\')
				s.push_back(L'\\');
			return s;
		}

		// 从 start 开始逐级向上，找到第一个「自身包含 Assets 子目录」的祖先。
		// 这同时覆盖两种布局：
		//   源码树:  <repo>/Assets
		//   构建树:  <repo>/out/build/x64-debug/Demos/DeferredApp/  -> 上溯到 <repo>
		bool FindAssetsByWalkingUp(const fs::path& start, int maxDepth, fs::path& out)
		{
			std::error_code ec;
			fs::path cur = start;
			for (int i = 0; i < maxDepth; ++i)
			{
				if (cur.empty())
					break;

				const fs::path candidate = cur / L"Assets";
				if (fs::is_directory(candidate, ec))
				{
					out = candidate;
					return true;
				}

				if (!cur.has_parent_path() || cur.parent_path() == cur)
					break;
				cur = cur.parent_path();
			}
			return false;
		}

		// 取 exe 所在目录（GetModuleFileNameW 对路径长度有上限，这里循环扩容）
		bool GetExeDirectory(fs::path& out)
		{
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
				if (len == 0)
					return false;
				if (len < buffer.size())
				{
					buffer.resize(len);
					break;
				}
				buffer.resize(buffer.size() * 2);
			}

			std::error_code ec;
			out = fs::path(buffer).parent_path();
			return !out.empty();
		}

		// 该路径是否像资源根：必须有 Assets 子目录
		bool LooksLikeAssets(const fs::path& p)
		{
			std::error_code ec;
			return fs::is_directory(p / L"Assets", ec);
		}
	}

	bool Initialize()
	{
		std::error_code ec;

		// 1) 命令行覆盖
		{
			std::wstring fromArg;
			if (CommandLineArgs::GetString(L"AssetsRoot", fromArg) && !fromArg.empty())
			{
				fs::path p(fromArg);
				// 允许直接给 Assets 目录，也允许给项目根目录
				if (fs::is_directory(p / L"Assets", ec))
				{
					g_AssetsRoot = NormalizeRoot(p / L"Assets");
					return true;
				}
				if (fs::is_directory(p, ec))
				{
					g_AssetsRoot = NormalizeRoot(p);
					return true;
				}
				Utility::Printf(L"[Asset] -AssetsRoot 指定的路径不存在: %ws\n", fromArg.c_str());
			}
		}

		// 2) 环境变量覆盖
		{
			const DWORD needed = ::GetEnvironmentVariableW(L"TOOIYUME_ASSETS_ROOT", nullptr, 0);
			if (needed > 0)
			{
				std::wstring value(needed, L'\0');
				const DWORD written = ::GetEnvironmentVariableW(L"TOOIYUME_ASSETS_ROOT", value.data(), needed);
				if (written > 0 && written < needed)
				{
					value.resize(written);
					fs::path p(value);
					if (fs::is_directory(p / L"Assets", ec))
					{
						g_AssetsRoot = NormalizeRoot(p / L"Assets");
						return true;
					}
					if (fs::is_directory(p, ec))
					{
						g_AssetsRoot = NormalizeRoot(p);
						return true;
					}
					Utility::Printf(L"[Asset] TOOIYUME_ASSETS_ROOT 指向的路径无效: %ws\n", value.c_str());
				}
			}
		}

		// 3) 从 exe 目录向上找
		{
			fs::path exeDir;
			fs::path found;
			if (GetExeDirectory(exeDir) && FindAssetsByWalkingUp(exeDir, 8, found))
			{
				g_AssetsRoot = NormalizeRoot(found);
				return true;
			}
		}

		// 4) 从当前工作目录向上找（覆盖「从 IDE / 任意 shell 启动」的场景）
		{
			const fs::path cwd = fs::current_path(ec);
			if (!ec)
			{
				fs::path found;
				if (FindAssetsByWalkingUp(cwd, 8, found))
				{
					g_AssetsRoot = NormalizeRoot(found);
					return true;
				}
			}
		}

		// 5) 编译期兜底（CMake 注入的源码树 Assets 目录）
#ifdef TOOIYUME_SOURCE_ASSETS_DIR
		{
			fs::path p(TOOIYUME_SOURCE_ASSETS_DIR);
			if (fs::is_directory(p, ec))
			{
				g_AssetsRoot = NormalizeRoot(p);
				return true;
			}
		}
#endif

		g_AssetsRoot.clear();
		return false;
	}

	std::wstring Resolve(const std::wstring& relativePath)
	{
		std::wstring rel = relativePath;
		for (auto& c : rel)
			if (c == L'/') c = L'\\';

		// 已经是绝对路径就不再拼根（保留调用方显式指定的能力）
		if (rel.size() >= 3 && rel[1] == L':' && (rel[2] == L'\\' || rel[2] == L'/'))
			return rel;

		while (!rel.empty() && rel.front() == L'\\')
			rel.erase(rel.begin());

		// 根目录没解析出来时，直接拼接只会得到一个相对路径，之后在文件系统里
		// 静默找不到。这里明确报一次错，避免又变成「黑屏 + 无信息」。
		if (g_AssetsRoot.empty())
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				Utility::Printf(L"[Asset] 错误：AssetsRoot 尚未解析，请求资源 %ws 无法定位。\n", rel.c_str());
				Utility::Printf(L"[Asset]   请先查看启动日志中的 AssetsRoot 解析结果。\n");
			}
			return rel;
		}

		return g_AssetsRoot + rel;
	}

	bool Exists(const std::wstring& relativePath)
	{
		std::error_code ec;
		return fs::exists(Resolve(relativePath), ec);
	}

	void LogResolution()
	{
		if (g_AssetsRoot.empty())
		{
			Utility::Printf(
				L"[Asset] *** 未能定位 Assets 目录 ***\n"
				L"[Asset] 已尝试: -AssetsRoot 参数 / TOOIYUME_ASSETS_ROOT 环境变量 /\n"
				L"[Asset]          exe 目录上溯 / 工作目录上溯 / 编译期默认路径\n"
				L"[Asset] 请设置其中之一；模型与环境贴图将无法加载。\n");
		}
		else
		{
			Utility::Printf(L"[Asset] AssetsRoot = %ws\n", g_AssetsRoot.c_str());
		}
	}
}
