#pragma once

#include "09_Renderer/BaseRenderer.h"

#include "02_Camera/Camera.h"
#include "03_RHI/CommandSystem/CommandContext.h"
#include "03_RHI/Pipeline/RootSignature.h"
#include "03_RHI/Pipeline/PipelineState.h"
#include "03_RHI/Resource/GpuBuffer.h"
#include "03_RHI/Resource/ColorBuffer.h"
#include "03_RHI/Resource/DepthBuffer.h"

#include "04_GPUInfrastructure/GPUHeap/UploadBuffer.h"
#include "05_ResourceSystem/01_Manager/TextureManager.h"
#include "08_RenderFeatures/Lighting/LightingSystem.h"

#include "13_Math/VectorMath.h"
//#include <cstdint>
//#include <vector>
//
//#include <d3d12.h>

class GraphicsPSO;
class GraphicsContext;

namespace Renderer
{
	class DefaultRender final : public BaseRenderer
	{
	public:
		std::wstring GetName() const override { return L"DeferredRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame) override;
		void Render(GraphicsContext& context, const RenderFrameDesc& frame) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		RendererFeature GetFeatures() const override // !!!!!!!!!!!!!!没实现
		{
			return RendererFeature::Deferred
				| RendererFeature::Shadow
				| RendererFeature::SSAO
				| RendererFeature::Transparent
				| RendererFeature::PostProcess;
		}

	private:
		void BuildRootSignature();
		void BuildDescriptorHeaps();
		void BuildPSOs();
		void CreateGBufferTargets();
		void DestroyGBufferTargets();

		void UpdateGlobalDescriptors();
	private:
		RootSignature m_RootSig;

		DescriptorHeap m_TextureHeap;
		DescriptorHeap m_SamplerHeap;
		DescriptorHandle m_CommonTextures;

		std::vector<GraphicsPSO> m_PSOCache;

		DepthBuffer g_DepthBuffer;
		ColorBuffer g_BaseColorBuffer;
		ColorBuffer g_NormalBuffer;
		ColorBuffer g_MaterialBuffer;

		ColorBuffer g_SceneColorBuffer;
	};

}
