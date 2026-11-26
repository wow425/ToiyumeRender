// ============================================================================
// 模块：GFL2CharacterPipeline（少前2 AK-12 复刻主渲染管线编排器 · M0–M6 全管线）
// 架构层：Engine/04_Renderer/Renderer/GFL2
// 设计基准：
//   - UE5 左手系 Z-Up（+X 前, +Y 右, +Z 上）
//   - UE5 行向量左乘（v * M）与 #pragma pack_matrix(row_major)
//   - UE5 Reverse-Z（Near=1.0, Far=0.0, Clear=0.0f, D3D12_COMPARISON_FUNC_GREATER_EQUAL）
//   - 全程 LinearPassList + UnifiedDescriptorHeap（零切堆）+ 层级 PIX 标记
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"
#include "03_AssetSystem/Loaders/GFL2/EnvSceneLoader.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/LinearPassList.h"
#include "04_Renderer/Passes/Common/GFL2ShadowPass.h"
#include "04_Renderer/Passes/Common/GFL2EnvScenePass.h"
#include "04_Renderer/Passes/Common/GFL2DebugOverlayPass.h"
#include "04_Renderer/Passes/Character/CharacterOpaquePass.h"
#include "04_Renderer/Passes/Character/CharacterFaceHairEyePass.h"
#include "04_Renderer/Passes/Character/CharacterOutlinePass.h"
#include "04_Renderer/Passes/Character/CharacterTransparentPass.h"
#include "04_Renderer/Passes/PostProcess/GFL2PostProcessPass.h"
#include "04_Renderer/Passes/TonemapPass.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"

namespace Renderer::GFL2
{
	struct GFL2PipelineBuffers
	{
		std::shared_ptr<ColorBuffer> LinearHDRBuffer;  // R16G16B16A16_FLOAT
		std::shared_ptr<ColorBuffer> CharacterAuxMask; // R16G16B16A16_FLOAT (N_ws*0.5+0.5, a=0.0 Env / 0.498 Char)
		std::shared_ptr<ColorBuffer> FloorMirrorRT;    // R16G16B16A16_FLOAT (半分辨率平面反射)
		std::shared_ptr<DepthBuffer> FloorMirrorDepth; // D32_FLOAT_S8X24_UINT
		std::shared_ptr<ColorBuffer> SceneColorBuffer; // R8G8B8A8_UNORM (Tonemap 唯一输出)
		std::shared_ptr<DepthBuffer> SceneDepthBuffer; // D32_FLOAT_S8X24_UINT (Reverse-Z Clear=0.0f)
		GFL2PostTargets              PostTargets;
	};

	class GFL2CharacterPipeline final : public BaseRenderer
	{
	public:
		std::wstring GetName() const override { return L"GFL2CharacterPipeline"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext) override;
		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass,
			BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		std::shared_ptr<ColorBuffer> GetOutputColorBuffer() const override
		{
			return m_Buffers ? m_Buffers->SceneColorBuffer : nullptr;
		}

		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const override;
		void RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services) override;
		void ResetHistory() override;

		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Forward | RendererFeature::PostProcess;
		}

		const GFL2PipelineBuffers* GetBuffers() const { return m_Buffers.get(); }
		const DepthBuffer* GetMainShadowMap() const { return m_ShadowMapMain.get(); }
		const ::GFL2::CharacterManifestLoader& GetCharacterAssets() const { return m_Assets; }
		const ::GFL2::EnvSceneLoader& GetEnvAssets() const { return m_EnvAssets; }

	private:
		void CreatePipelineTargets();
		void DestroyPipelineTargets();

	private:
		std::shared_ptr<GFL2PipelineBuffers> m_Buffers;
		std::shared_ptr<DepthBuffer>         m_ShadowMapMain; // 4096x4096 D32_FLOAT
		std::shared_ptr<DepthBuffer>         m_ShadowMapAdd1; // 512x512 D32_FLOAT (Light[1])
		FrameBlackboard                      m_Blackboard;
		LinearPassList                       m_PassList;
		GFL2ShadowPass                       m_ShadowPass;
		GFL2EnvScenePass                     m_EnvPass;
		CharacterOpaquePass                  m_OpaquePass;
		CharacterFaceHairEyePass             m_FaceHairEyePass;
		CharacterOutlinePass                 m_OutlinePass;
		CharacterTransparentPass             m_TransparentPass;
		GFL2PostProcessPass                  m_PostPass;
		GFL2DebugOverlayPass                 m_DebugOverlayPass;
		TonemapPass                          m_TonemapPass;
		::GFL2::CharacterManifestLoader      m_Assets;
		::GFL2::EnvSceneLoader               m_EnvAssets;
		uint64_t                             m_FrameCounter = 0;
	};
}
