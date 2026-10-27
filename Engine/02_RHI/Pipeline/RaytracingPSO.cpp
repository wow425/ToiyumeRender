#include "00_Core/PCH.h"
#include "02_RHI/Pipeline/RaytracingPSO.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Pipeline/RootSignature.h"

#include <atomic>
#include <mutex>

using namespace Microsoft::WRL;

namespace Renderer::RayTracing
{
	namespace
	{
		std::atomic<uint32_t> s_LiveRaytracingPSOs{ 0 };
	}

	void RegisterRaytracingPSO() { ++s_LiveRaytracingPSOs; }
	void UnregisterRaytracingPSO() { --s_LiveRaytracingPSOs; }
	uint32_t GetLiveRaytracingPSOCount() { return s_LiveRaytracingPSOs.load(); }

	// 本阶段没有需要跨 renderer 共享的全局 RTPSO 缓存（每个 RayTracingRenderer
	// 自己持有一份），因此 DestroyAll 只做计数复位前的安全检查。
	// 保留这个函数是为了与 PSO::DestroyAll / RootSignature::DestroyAll 的
	// 调用点对称，将来若引入全局缓存可直接在此实现。
	void DestroyAllRaytracingPSOs()
	{
		const uint32_t live = s_LiveRaytracingPSOs.load();
		if (live != 0)
		{
			LOG_INFO(L"[DXR] DestroyAllRaytracingPSOs: 仍有 %u 个 RTPSO 未销毁（由各自 Renderer 负责）\n", live);
		}
	}

	void RaytracingPSO::Swap(RaytracingPSO& other) noexcept
	{
		m_StateObject.Swap(other.m_StateObject);
		m_Properties.Swap(other.m_Properties);
		std::swap(m_Name, other.m_Name);
		m_LastError.swap(other.m_LastError);
	}

	void RaytracingPSO::Destroy()
	{
		if (m_StateObject != nullptr)
		{
			m_Properties.Reset();
			m_StateObject.Reset();
			UnregisterRaytracingPSO();
		}
		m_LastError.clear();
	}

	bool RaytracingPSO::Finalize(const RaytracingStateObjectDesc& desc)
	{
		Destroy();

		if (Graphics::g_DeviceDXR == nullptr)
		{
			m_LastError = L"ID3D12Device5 不可用（设备不支持 DXR）";
			LOG_INFO(L"[DXR] RTPSO 创建失败：%s\n", m_LastError.c_str());
			return false;
		}
		if (desc.DXILLibrary == nullptr || desc.DXILLibrarySize == 0)
		{
			m_LastError = L"DXIL 库字节码为空";
			LOG_INFO(L"[DXR] RTPSO 创建失败：%s\n", m_LastError.c_str());
			return false;
		}
		if (desc.GlobalRootSignature == nullptr)
		{
			m_LastError = L"全局根签名为空";
			LOG_INFO(L"[DXR] RTPSO 创建失败：%s\n", m_LastError.c_str());
			return false;
		}

		// ============================================================
		//  组装 State Object 子对象
		// ------------------------------------------------------------
		//  CD3DX12_STATE_OBJECT_DESC 的子对象是**延迟绑定**的：这里创建的
		//  每个 CD3DX12_XXX_SUBOBJECT 都只是把描述挂到 State Object 的
		//  子对象数组上，真正的 D3D12_STATE_OBJECT_DESC 由 .CreateStateObject
		//  时一次性展开。因此这些局部对象在函数返回前必须保持有效 ——
		//  它们确实都在本函数栈上，直到 Finalize 末尾才析构，安全。
		// ============================================================
		CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

		// 1) DXIL 库 + 导出
		auto* dxilLib = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		D3D12_SHADER_BYTECODE libBytecode = {};
		libBytecode.pShaderBytecode = desc.DXILLibrary;
		libBytecode.BytecodeLength = desc.DXILLibrarySize;
		dxilLib->SetDXILLibrary(&libBytecode);
		for (const wchar_t* exportName : desc.Exports)
		{
			if (exportName != nullptr)
				dxilLib->DefineExport(exportName);
		}

		// 2) Hit Group
		//    DXR 要求 ClosestHit/AnyHit/Intersection 先被 hit group 引用，
		//    才能被 DispatchRays 的 SBT 间接寻址。
		auto* hitGroup = stateObjectDesc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
		hitGroup->SetHitGroupExport(desc.HitGroupName);
		hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
		if (desc.ClosestHitExport)  hitGroup->SetClosestHitShaderImport(desc.ClosestHitExport);
		if (desc.AnyHitExport)      hitGroup->SetAnyHitShaderImport(desc.AnyHitExport);
		if (desc.IntersectionExport) hitGroup->SetIntersectionShaderImport(desc.IntersectionExport);

		// 3) Shader Config（载荷/属性大小，必须与 HLSL 结构体一致）
		auto* shaderConfig = stateObjectDesc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
		shaderConfig->Config(desc.MaxPayloadSizeInBytes, desc.MaxAttributeSizeInBytes);

		// 4) Pipeline Config（最大递归深度）
		auto* pipelineConfig = stateObjectDesc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
		pipelineConfig->Config(desc.MaxTraceRecursionDepth);

		// 5) Global Root Signature
		auto* globalRootSig = stateObjectDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
		globalRootSig->SetRootSignature(desc.GlobalRootSignature);

		// ============================================================
		//  创建
		// ============================================================
		const HRESULT hr = Graphics::g_DeviceDXR->CreateStateObject(
			stateObjectDesc, TY_IID_PPV_ARGS(&m_StateObject));

		if (FAILED(hr))
		{
			wchar_t buf[256];
			swprintf_s(buf, L"CreateStateObject 失败 hr=0x%08X", (unsigned)hr);
			m_LastError = buf;
			LOG_INFO(L"[DXR] RTPSO 创建失败：%s（name=%s）\n", m_LastError.c_str(), m_Name);
			m_StateObject.Reset();
			return false;
		}

		const HRESULT hrProps = m_StateObject->QueryInterface(TY_IID_PPV_ARGS(&m_Properties));
		if (FAILED(hrProps))
		{
			wchar_t buf[256];
			swprintf_s(buf, L"QueryInterface(ID3D12StateObjectProperties) 失败 hr=0x%08X", (unsigned)hrProps);
			m_LastError = buf;
			LOG_INFO(L"[DXR] RTPSO 创建失败：%s（name=%s）\n", m_LastError.c_str(), m_Name);
			m_StateObject.Reset();
			return false;
		}

		RegisterRaytracingPSO();
		LOG_INFO(L"[DXR] RTPSO 创建成功：name=%s exports=%zu payload=%u attr=%u depth=%u\n",
			m_Name, desc.Exports.size(),
			desc.MaxPayloadSizeInBytes, desc.MaxAttributeSizeInBytes, desc.MaxTraceRecursionDepth);
		return true;
	}

	bool RaytracingPSO::GetShaderIdentifier(const wchar_t* exportName, void* outIdentifier32Bytes) const
	{
		if (m_Properties == nullptr || exportName == nullptr || outIdentifier32Bytes == nullptr)
			return false;

		const void* id = m_Properties->GetShaderIdentifier(exportName);
		if (id == nullptr)
		{
			LOG_INFO(L"[DXR] GetShaderIdentifier 失败：export=%s 不存在于 State Object\n", exportName);
			return false;
		}
		// D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES = 32
		std::memcpy(outIdentifier32Bytes, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
		return true;
	}
}
