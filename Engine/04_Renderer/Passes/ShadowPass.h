// ============================================================================
// 模块：ShadowPass（可插拔方向光阴影 Pass · 托管 SimpleShadowMap / CSM 热切换）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 4）
// 核心职责：
//   1) 独占 ShadowDepthVS / ShadowDepthAlphaTestPS 字节码、Shadow RootSignature 与深度 PSO 缓存；
//   2) 托管当前活跃 IShadowAlgorithm 实例，并在帧边界按「先备后切 + VramBudgetGuard +
//      DeferredReleaseQueue 延迟退场」协议无痛热切换 SimpleShadowMap <-> CSM 或分辨率；
//   3) 通过 RasterDrawHelper 复用场景排序结果，为不透明网格走纯深度快速路径（PS=nullptr），
//      为 glTF MASK 网格走 AlphaTest 镂空深度路径。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <string>
#include <vector>

#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/Passes/IShadowAlgorithm.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"

class GraphicsContext;

namespace Renderer
{
	class FrameBlackboard;
	class RasterDrawHelper;
	class RenderServices;
	struct RenderParams;

	class ShadowPass
	{
	public:
		ShadowPass() = default;
		~ShadowPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown(RenderServices* services);

		// 帧边界确保当前活跃 IShadowAlgorithm 与 params (Algorithm / Resolution / CascadeCount) 匹配；
		// 若参数变更则按「CheckSupport -> VramBudgetGuard -> Candidate Initialize -> 延迟释放旧实例」热切换。
		bool EnsureAlgorithm(RenderServices& services, const RenderParams& params);

		// 返回当前活跃阴影深度缓冲指针（供 LinearPassList 声明 ResourceAccess 屏障）。
		DepthBuffer* GetShadowDepthBuffer() const
		{
			return m_Algorithm ? m_Algorithm->GetShadowDepthBuffer() : nullptr;
		}

		// 执行阴影深度图绘制并返回本帧 ShadowOutput（含 Bindless SRV 下标与各级联矩阵）。
		ShadowOutput Execute(GraphicsContext& context,
			const RenderFrameDesc& frame,
			RasterDrawHelper& drawHelper,
			const RenderParams& params,
			RenderServices& services);

		const char* GetActiveAlgorithmId() const { return m_Algorithm ? m_Algorithm->GetId() : "None"; }
		const std::string& GetLastSwitchError() const { return m_LastSwitchError; }
		bool IsInitialized() const { return m_Initialized; }

	private:
		void BuildRootSignature();
		void BuildBasePSOs();
		const GraphicsPSO& GetOrCreatePSO(const PipelineDesc& desc);
		void BindShadowMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const;

		// 与 ShadowDepthVS.hlsl / ShadowDepthAlphaTestPS.hlsl 严格对应的根参数下标
		enum ShadowRootSlot : uint32_t
		{
			kShadowMeshCBV     = 0, // b0 (VS): 与 BaseRenderer::kMeshConstants == 0 严格一致（RasterDrawHelper 要求）
			kShadowViewCBV     = 1, // b1 (VS): LightViewProj (64B)
			kShadowMaterialCBV = 2, // b0 (PS): MaterialConstants (AlphaTest flags / alphaRef)
			kShadowMaterialSRV = 3, // t0..t4 (PS): 材质 SRV 表（Zone A 5 槽块）
			kShadowMaterialSmp = 4, // s0..s4 (PS): 材质采样器表
			kShadowNumSlots    = 5
		};

		RootSignature                     m_RootSig;
		GraphicsPSO                       m_OpaqueBasePSO;
		GraphicsPSO                       m_AlphaTestBasePSO;
		std::vector<GraphicsPSO>          m_PSOCache;
		std::unique_ptr<IShadowAlgorithm> m_Algorithm;
		uint32_t                          m_ActiveAlgType     = 0xFFFFFFFFu;
		uint32_t                          m_ActiveResolution  = 0;
		uint32_t                          m_ActiveCascades    = 0;
		std::string                       m_LastSwitchError;
		bool                              m_Initialized       = false;
	};
}
