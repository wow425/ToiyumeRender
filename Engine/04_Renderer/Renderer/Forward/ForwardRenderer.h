#pragma once

//Mesh
//↓ 提供 Geometry Feature
//
//Material
//↓ 提供 Render Feature
//
//PipelineStateCache
//↓ 组合生成PSO
//
//Renderer
//↓ 调度



#include "04_Renderer/Renderer/Base/BaseRenderer.h"

#include "05_Scene/Camera/Camera.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/Heap/UploadBuffer.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "04_Renderer/Features/Lighting/LightingSystem.h"
#include "00_Core/Math/VectorMath.h"

#include <cstdint>
#include <vector>

#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
struct GlobalConstants;
struct Mesh; // Model


namespace Renderer
{
	extern DescriptorHeap s_TextureHeap;  // texture堆。存放CBV/SRV/UAV描述符堆
	extern DescriptorHeap s_SamplerHeap;  // sampler堆

	struct ForwardBuffer
	{
		std::shared_ptr<ColorBuffer> SceneColorBuffer[8];
		std::shared_ptr<DepthBuffer> DepthBuffer;
	};


	class ForwardRenderer  final : public BaseRenderer
	{
	public:

		std::wstring GetName() const override { return L"ForwardRenderer "; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame) override;

		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;


		ForwardBuffer GetForwardBuffer(void) { return *m_ForwardBuffer; }
		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Deferred
				| RendererFeature::Shadow
				| RendererFeature::SSAO
				| RendererFeature::Transparent
				| RendererFeature::PostProcess;
		}

		const GraphicsPSO& GetPSO(const PipelineDesc& desc) override;
		void BindRenderState(GraphicsContext& context) override;
		void BindMaterial(GraphicsContext& context, const Material& material) override;


	private:
		void BuildRootSignature();
		void BuildDescriptorHeaps();
		void BuildPSOs();
		void DestroyForwardBufferTargets();
		void CreateForwardBufferTargets();

		void UpdateGlobalDescriptors();
		uint8_t GetPSOIndex(const PipelineDesc& desc);

	private:
		RootSignature m_RootSig;

		DescriptorHandle m_CommonTextures; // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。

		std::vector<GraphicsPSO> m_PSOCache;

		std::shared_ptr<ForwardBuffer> m_ForwardBuffer;
	};


} // namespace Renderer
