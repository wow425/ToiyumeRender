// ============================================================================
// 模块：SimpleShadowMap（单级联方向光 Shadow Map 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 4）
// 核心职责：
//   - 持有 1 切片 Texture2DArray (D32_FLOAT) 深度阴影图，紧贴当前场景包围球构造正交投影；
//   - 对正交中心按纹素网格对齐（Texel Snapping），抑制移动时的阴影边缘闪烁；
//   - 关闭时经 DeferredReleaseQueue 延迟归还 GPU 资源与 DSV/SRV 描述符，支持无损热切换。
// ============================================================================
#pragma once

#include <memory>

#include "02_RHI/Resource/DepthBuffer.h"
#include "04_Renderer/Passes/IShadowAlgorithm.h"

namespace Renderer
{
	class SimpleShadowMap final : public IShadowAlgorithm
	{
	public:
		SimpleShadowMap() = default;
		~SimpleShadowMap() override = default;

		const char* GetId() const override { return "SimpleShadowMap"; }
		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, const ShadowParams& params) const override;
		bool Initialize(RenderServices& services, const ShadowParams& params, std::string* outErr) override;
		void Shutdown(Graphics::DeferredReleaseQueue& releaseQueue) override;

		DepthBuffer* GetShadowDepthBuffer() const override { return m_ShadowMap.get(); }
		uint32_t GetResolution() const override { return m_Resolution; }
		uint32_t GetCascadeCount() const override { return 1u; }

		ShadowOutput Render(GraphicsContext& ctx,
			const Scene::Camera::Camera& camera,
			const std::vector<Scene::Model::ModelInstance*>& models,
			const ShadowParams& params,
			RenderServices& services,
			const ShadowDrawCascadeFn& drawCascade) override;

	private:
		std::shared_ptr<DepthBuffer> m_ShadowMap;
		uint32_t                     m_Resolution = 0;
	};
}
