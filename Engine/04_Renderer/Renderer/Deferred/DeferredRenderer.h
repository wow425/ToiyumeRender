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
#include "04_Renderer/Renderer/RendererConfig.h"
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
#include <array>
#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
struct GlobalConstants;
struct Mesh; // Model

// GBuffer Pass: Bind GBuffers + Depth
// Lighting Pass： Read GBuffers Write SceneColor
// TAA Pass: Read Velocity + SceneColor




namespace Renderer::Deferred
{


	// Render Pipeline Semantic
	enum class GBufferSlot : uint8_t
	{
		GBuffer_BaseColor, // RGB放Basecolor，A放AO
		GBuffer_Normal,    // 法线编码用Octahedral Encoding
		GBuffer_Material,  // R为Metallic，G为Roughness，B为Specular 或 Anisotropy， A为Shading Model / Material ID
		GBuffer_Emission,  // R为Emissive， G为Subsurface Mask， B为Custom Data， A为ClearCoat   ??

		GBuffer_Count
	};

	struct DeferredBuffer
	{
		std::array<std::shared_ptr<ColorBuffer>, (uint32_t)GBufferSlot::GBuffer_Count> GBuffers;

		std::shared_ptr<ColorBuffer> SceneColorBuffer;
		std::shared_ptr<DepthBuffer> SceneDepthBuffer;
		std::shared_ptr<ColorBuffer> VelocityBuffer;
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


		DeferredBuffer GetDeferredBuffer(void) { return *m_DeferredBuffer; }
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
		void BindMaterial(GraphicsContext& context, const Scene::Material::Material& material) override;

	private:
		void BuildRootSignature();
		void BuildDescriptorHeaps();
		void BuildPSOs();
		void CreateDeferredBufferTargets();
		void DestroyDeferredBufferTargets();


		void UpdateGlobalDescriptors();
		uint8_t GetPSOIndex(const PipelineDesc& desc);

	private:
		std::vector<GraphicsPSO> m_PSOCache;
		RootSignature m_RootSig;

		DescriptorHandle m_CommonTextures; // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。
		std::shared_ptr<DeferredBuffer> m_DeferredBuffer;
	};


} // namespace Renderer::Deferred


