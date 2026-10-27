#pragma once

// ============================================================================
//  RaytracingPSO.h —— R5 / D1：DXR State Object（RTPSO）封装
// ----------------------------------------------------------------------------
//  为什么不能复用 PSO / GraphicsPSO：
//    * PSO 内部持有的是 D3D12_GRAPHICS_PIPELINE_STATE_DESC，
//      DXR 用的是 D3D12_STATE_OBJECT_DESC + ID3D12StateObject，
//      两者结构体、创建接口、查询接口（GetPipelineStackSize 等）都不同。
//    * 光栅 PSO 走 ID3D12Device::CreateGraphicsPipelineState；
//      RTPSO 走 ID3D12Device5::CreateStateObject，且失败时还能拿到
//      ID3D12InfoQueue1 / ID3D12StateObjectProperties 做诊断。
//    * 生命周期也不同：RTPSO 里嵌着对全局根签名的引用，
//      必须先于根签名销毁。
//  因此这里独立一条创建/缓存路径，符合路线图 D1「不复用仅面向 Graphics PSO 的缓存」。
//
//  本类只做三件事：
//    1) 收集 State Object 子对象（DXIL 库 / HitGroup / ShaderConfig /
//       PipelineConfig / GlobalRootSignature）；
//    2) 调用 CreateStateObject；
//    3) 暴露 shader identifier 查询（SBT 需要）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

class RootSignature;

namespace Renderer::RayTracing
{
	// ============================================================
	//  State Object 配置（值语义，便于将来做「按配置哈希缓存」）
	// ============================================================
	struct RaytracingStateObjectDesc
	{
		// DXIL 库字节码（由 compile_raytracing_library 生成，见 ShaderCache/RayTracing.h）
		const void* DXILLibrary = nullptr;
		size_t      DXILLibrarySize = 0;

		// 库内导出名。lib_6_4 编译出的每个 [shader("...")] 都是一个导出。
		std::vector<const wchar_t*> Exports;

		// Hit Group
		const wchar_t* HitGroupName = L"HitGroup";
		const wchar_t* ClosestHitExport = L"ClosestHit";
		const wchar_t* AnyHitExport = nullptr;          // 可选
		const wchar_t* IntersectionExport = nullptr;    // 可选（程序化几何）

		const wchar_t* RayGenExport = L"RayGen";
		const wchar_t* MissExport = L"Miss";

		// 载荷与属性大小（必须与 HLSL 的 RayPayload / HitAttributes 一致）
		uint32_t MaxPayloadSizeInBytes = 8 * sizeof(float);   // float3 + float，按 8 个 float 留余量
		uint32_t MaxAttributeSizeInBytes = 2 * sizeof(float); // float2 Barycentrics

		// 最大递归深度（D2 只发一次主射线，1 足够；D3 加阴影/反射时需要 +1）
		uint32_t MaxTraceRecursionDepth = 1;

		// 全局根签名（必须活得比 State Object 久）
		ID3D12RootSignature* GlobalRootSignature = nullptr;
	};

	// ============================================================
	//  RaytracingPSO
	// ============================================================
	class RaytracingPSO
	{
	public:
		RaytracingPSO() = default;
		explicit RaytracingPSO(const wchar_t* name) : m_Name(name) {}
		~RaytracingPSO() { Destroy(); }

		// 不允许拷贝（持有 COM 引用）
		RaytracingPSO(const RaytracingPSO&) = delete;
		RaytracingPSO& operator=(const RaytracingPSO&) = delete;

		// 允许移动（便于放进 std::vector 做缓存）
		RaytracingPSO(RaytracingPSO&& other) noexcept { Swap(other); }
		RaytracingPSO& operator=(RaytracingPSO&& other) noexcept
		{
			if (this != &other) { Destroy(); Swap(other); }
			return *this;
		}

		// 创建 State Object。失败返回 false，并通过 GetLastError() 取诊断信息。
		bool Finalize(const RaytracingStateObjectDesc& desc);

		void Destroy();

		ID3D12StateObject* GetStateObject() const { return m_StateObject.Get(); }
		ID3D12StateObjectProperties* GetProperties() const { return m_Properties.Get(); }
		bool IsValid() const { return m_StateObject != nullptr && m_Properties != nullptr; }

		const std::wstring& GetLastError() const { return m_LastError; }
		const wchar_t* GetName() const { return m_Name; }

		// ============================================================
		//  Shader Identifier（SBT 用）
		// ------------------------------------------------------------
		// DXR 的 SBT 里存的不是 GPU 地址，而是 State Object 编译期分配的
		// 32 字节 shader identifier。这里做一层封装，避免调用方到处
		// QueryInterface + 判空。
		// ============================================================
		bool GetShaderIdentifier(const wchar_t* exportName, void* outIdentifier32Bytes) const;

	private:
		void Swap(RaytracingPSO& other) noexcept;

		const wchar_t* m_Name = L"Unnamed RTPSO";
		Microsoft::WRL::ComPtr<ID3D12StateObject> m_StateObject;
		Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> m_Properties;
		std::wstring m_LastError;
	};

	// 全局销毁（与 PSO::DestroyAll / RootSignature::DestroyAll 对齐的对称接口，
	// 供 Graphics::Shutdown 里统一收尾；本阶段只登记计数，便于泄漏排查）。
	void DestroyAllRaytracingPSOs();
	void RegisterRaytracingPSO();
	void UnregisterRaytracingPSO();
	uint32_t GetLiveRaytracingPSOCount();
}
