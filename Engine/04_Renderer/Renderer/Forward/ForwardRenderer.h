#pragma once
// ============================================================================
// 模块：ForwardRenderer（前向渲染管线薄编排器 · WP-2B）
// 架构层：Engine/04_Renderer/Renderer/Forward
// 核心职责：持有 Forward 帧目标并通过 RasterDrawHelper + FrameBlackboard 驱动 ForwardMainPass。
// ============================================================================

#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/LinearPassList.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "04_Renderer/Passes/ForwardMainPass.h"
#include "04_Renderer/Passes/TonemapPass.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Command/CommandContext.h"

#include <cstdint>
#include <memory>

namespace Renderer::Forward
{
	using Config = Renderer::Forward::Config;

	struct ForwardBuffer
	{
		std::shared_ptr<ColorBuffer> LinearHDRBuffer;     // [WP-2C] R16G16B16A16_FLOAT
		std::shared_ptr<ColorBuffer> SceneColorBuffer[8]; // R8G8B8A8_UNORM LDR
		std::shared_ptr<DepthBuffer> DepthBuffer;
	};

	class ForwardRenderer final : public BaseRenderer
	{
	public:
		std::wstring GetName() const override { return L"ForwardRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext) override;
		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		ForwardBuffer GetForwardBuffer(void) { return *m_ForwardBuffer; }
		std::shared_ptr<ColorBuffer> GetOutputColorBuffer() const override
		{
			return m_ForwardBuffer ? m_ForwardBuffer->SceneColorBuffer[0] : nullptr;
		}

		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const override;
		void RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services) override;

		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Forward;
		}

	private:
		void CreateForwardBufferTargets();
		void DestroyForwardBufferTargets();

	private:
		RasterDrawHelper m_DrawHelper{ BatchType::kDefault };
		FrameBlackboard  m_Blackboard;
		LinearPassList   m_PassList;
		ForwardMainPass  m_MainPass;
		TonemapPass      m_TonemapPass;

		std::shared_ptr<ForwardBuffer> m_ForwardBuffer;
	};
} // namespace Renderer::Forward
