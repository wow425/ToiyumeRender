// ============================================================================
// 模块：PrimaryRayPass（DXR 1.1 Inline RayQuery 主可见性与表面几何写入 Pass）
// 架构层：Engine/04_Renderer/Passes（契约 4 / 统一 PrimarySurfaceDesc DXR 生产端）
// 核心职责：
//   - 独占 PrimaryRayCS_cso 字节码与 ComputePSO（复用全局共享 BindlessComputeRootSignature）；
//   - 每帧从 AccelerationStructureManager 读取主场景 TLAS / SceneData / InstanceData，
//     在 4 分区单堆 Zone B 瞬态暂存 SRV/UAV 描述符（零切堆、零第二套 SBT），
//     将主射线命中结果写入与光栅 GBuffer 完全同构的 5 张 PrimarySurface 缓冲；
//   - 支持空场景安全清屏（Depth=1.0，其余归零）与开发期 F5 Shader 热重载。
// 硬件与同步前提：
//   - 需要 SM 6.6 Bindless + DXR Tier 1.1（Inline RayTracing）；不支持时安全降级跳过；
//   - 仅单线程主循环录制期执行；旧热重载 PSO 经 DeferredReleaseQueue 延迟回收。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "02_RHI/Pipeline/PipelineState.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"

class ColorBuffer;
class GraphicsContext;
struct ID3D12PipelineState;

namespace Graphics
{
	class ShaderHotReloader;
}

namespace Renderer
{
	class AccelerationStructureManager;
	struct PrimarySurfaceDesc;
	class RenderServices;

	struct PrimaryRayTargets
	{
		ColorBuffer* DepthR32F = nullptr; // DXGI_FORMAT_R32_FLOAT (NDC z ∈ [0,1], 1.0=miss)
		ColorBuffer* AlbedoAO  = nullptr; // DXGI_FORMAT_R8G8B8A8_UNORM (RGB=BaseColor, A=AO)
		ColorBuffer* NormalOct = nullptr; // DXGI_FORMAT_R16G16_FLOAT (RG=OctNormal)
		ColorBuffer* Material  = nullptr; // DXGI_FORMAT_R16G16B16A16_FLOAT (R=M, G=R, B=Spec, A=MatID)
		ColorBuffer* Emission  = nullptr; // DXGI_FORMAT_R8G8B8A8_UNORM (RGB=Emissive)

		bool IsValid() const
		{
			return DepthR32F != nullptr && AlbedoAO != nullptr &&
				NormalOct != nullptr && Material != nullptr && Emission != nullptr;
		}
	};

	class PrimaryRayPass
	{
	public:
		PrimaryRayPass() = default;
		~PrimaryRayPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown();

		bool IsInitialized() const { return m_Initialized; }

		// 注册 PrimaryRayCS.hlsl 到开发期 F5 热重载器（仅当 SM 6.6 + DXR 1.1 就绪时生效）
		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);

		// 执行主射线几何提取：有场景时派发 PrimaryRayCS，空场景时清屏至统一未命中哨兵值。
		void Execute(GraphicsContext& context,
			const RenderFrameDesc& frame,
			const AccelerationStructureManager& asManager,
			const PrimaryRayTargets& targets,
			uint32_t width,
			uint32_t height,
			RenderServices& services) const;

	private:
		bool HotReloadComputeShader(const std::vector<uint8_t>& dxil, RenderServices& services, std::string& outErr);

		ComputePSO                                   m_ComputePSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_HotComputePSO;
		bool                                         m_Initialized = false;
	};
}
