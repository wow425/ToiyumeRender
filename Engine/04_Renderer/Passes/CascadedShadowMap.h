// ============================================================================
// 模块：CascadedShadowMap（2..4 级联阴影图 CSM 实现 · DepthArray + 子资源屏障）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 4 + 契约 3 子资源状态展开）
// 核心职责：
//   - 持有 ArraySize = CascadeCount (2..4) 的 Texture2DArray (D32_FLOAT) 深度缓冲；
//   - 采用 Practical Split Scheme (PSSM) 划分视锥切片，逐级联计算最小包围球 + 纹素对齐；
//   - 逐切片调用 TransitionSubresource(slice, DEPTH_WRITE) 与 ClearDepthSlice，
//     在 PIX 中以 CSM_Cascade_0..N 清晰展示层级，并由 LinearPassList 统一收敛为全资源 SRV。
// ============================================================================
#pragma once

#include <memory>

#include "02_RHI/Resource/DepthBuffer.h"
#include "04_Renderer/Passes/IShadowAlgorithm.h"

namespace Renderer
{
	class CascadedShadowMap final : public IShadowAlgorithm
	{
	public:
		CascadedShadowMap() = default;
		~CascadedShadowMap() override = default;

		const char* GetId() const override { return "CSM"; }
		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, const ShadowParams& params) const override;
		bool Initialize(RenderServices& services, const ShadowParams& params, std::string* outErr) override;
		void Shutdown(Graphics::DeferredReleaseQueue& releaseQueue) override;

		DepthBuffer* GetShadowDepthBuffer() const override { return m_ShadowMap.get(); }
		uint32_t GetResolution() const override { return m_Resolution; }
		uint32_t GetCascadeCount() const override { return m_CascadeCount; }

		ShadowOutput Render(GraphicsContext& ctx,
			const Scene::Camera::Camera& camera,
			const std::vector<Scene::Model::ModelInstance*>& models,
			const ShadowParams& params,
			RenderServices& services,
			const ShadowDrawCascadeFn& drawCascade) override;

	private:
		std::shared_ptr<DepthBuffer> m_ShadowMap;
		uint32_t                     m_Resolution   = 0;
		uint32_t                     m_CascadeCount = 4;
	};
}
