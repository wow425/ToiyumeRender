#pragma once
// ============================================================================
// 模块：DeferredRenderer（延迟渲染管线编排器 · WP-3C 完整特写版）
// 架构层：Engine/04_Renderer/Renderer/Deferred
// 核心职责：
//   - 持有 GBuffer / Depth / SSAO / TAA History / LinearHDR / SceneColorLDR 帧目标；
//   - 通过三阶段 LinearPassList 按序编排并带 PIX 标记与逐 Pass GPU 计时：
//       [ShadowPass] -> GBufferPass -> [SSAO_Compute -> SSAO_Blur] ->
//       LightingPass -> [SkyboxPass] -> [TAAPass] -> TonemapPass；
//   - 在 GetFeatures() 中诚实注册 Deferred | Shadow | SSAO | TAA | PostProcess，
//     由 RenderParams.EnableShadow / EnableSSAO / EnableTAA 实时开关控制。
// ============================================================================

#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/LinearPassList.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "04_Renderer/Passes/GBufferPass.h"
#include "04_Renderer/Passes/IBLBakePass.h"
#include "04_Renderer/Passes/LightingPass.h"
#include "04_Renderer/Passes/ShadowPass.h"
#include "04_Renderer/Passes/SkyboxPass.h"
#include "04_Renderer/Passes/SSAOPass.h"
#include "04_Renderer/Passes/TAAPass.h"
#include "04_Renderer/Passes/TonemapPass.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Command/CommandContext.h"

#include <cstdint>
#include <memory>

namespace Renderer::Deferred
{
	enum class GBufferSlot : uint8_t
	{
		GBuffer_BaseColor, // RGB=BaseColor, A=AO
		GBuffer_Normal,    // RG=OctNormal
		GBuffer_Material,  // R=Metallic, G=Roughness, B=Specular, A=MaterialID
		GBuffer_Emission,  // RGB=Emissive
		GBuffer_Count
	};

	struct DeferredBuffer
	{
		std::shared_ptr<ColorBuffer> GBuffers[(uint32_t)GBufferSlot::GBuffer_Count];
		std::shared_ptr<ColorBuffer> LinearHDRBuffer;  // [WP-2C] R16G16B16A16_FLOAT 场景线性 HDR
		std::shared_ptr<ColorBuffer> SceneColorBuffer; // R8G8B8A8_UNORM Tonemap 后 LDR 上屏目标
		std::shared_ptr<DepthBuffer> SceneDepthBuffer;
		std::shared_ptr<ColorBuffer> VelocityBuffer;
		// [WP-3C] SSAO 原始/双边模糊目标（R8_UNORM）与 TAA 双缓冲历史（R16G16B16A16_FLOAT）
		std::shared_ptr<ColorBuffer> SSAORawBuffer;
		std::shared_ptr<ColorBuffer> SSAOBlurBuffer;
		std::shared_ptr<ColorBuffer> TAAHistoryBuffer[2];
	};

	class DeferredRenderer final : public BaseRenderer
	{
	public:
		std::wstring GetName() const override { return L"DeferredRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext) override;
		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		DeferredBuffer GetDeferredBuffer(void) { return *m_GBuffers; }
		std::shared_ptr<ColorBuffer> GetOutputColorBuffer() const override
		{
			return m_GBuffers ? m_GBuffers->SceneColorBuffer : nullptr;
		}

		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const override;
		void RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services) override;
		void ResetHistory() override;
		const PrimarySurfaceDesc* GetPrimarySurface() const override
		{
			return m_Blackboard.PrimarySurface.IsComplete() ? &m_Blackboard.PrimarySurface : nullptr;
		}

		// [WP-3C] ShadowPass / SSAOPass / TAAPass 真实上线，诚实声明全部已实现特征位
		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Deferred
				| RendererFeature::Shadow
				| RendererFeature::SSAO
				| RendererFeature::TAA
				| RendererFeature::PostProcess;
		}

	private:
		void CreateDeferredBufferTargets();
		void DestroyDeferredBufferTargets();

	private:
		RasterDrawHelper m_DrawHelper{ BatchType::kDefault };
		FrameBlackboard  m_Blackboard;
		LinearPassList   m_PassList;
		ShadowPass       m_ShadowPass;
		GBufferPass      m_GBufferPass;
		SSAOPass         m_SSAOPass;
		IBLBakePass      m_IBLBakePass;
		LightingPass     m_LightingPass;
		SkyboxPass       m_SkyboxPass;
		TAAPass          m_TAAPass;
		TonemapPass      m_TonemapPass;

		std::shared_ptr<DeferredBuffer> m_GBuffers;
		uint32_t                        m_FrameCounter = 0;
	};
} // namespace Renderer::Deferred
