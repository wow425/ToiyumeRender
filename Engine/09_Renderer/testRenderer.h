#pragma once

#include "09_Renderer/BaseRenderer.h"

#include "02_Camera/Camera.h"
#include "03_RHI/Resource/GpuBuffer.h"
#include "03_RHI/CommandSystem/CommandContext.h"
#include "04_GPUInfrastructure/GPUHeap/UploadBuffer.h"
#include "05_ResourceSystem/01_Manager/TextureManager.h"
#include "08_RenderFeatures/Lighting/LightingSystem.h"
#include "13_Math/VectorMath.h"

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
	struct ForwardBuffer
	{
		ColorBuffer SceneColorBuffer;
		DepthBuffer DepthBuffer;
	};

	class testRenderer final : public BaseRenderer
	{
	public:
		~testRenderer() noexcept override;

		std::wstring GetName() const override { return L"testRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void BeginFrame(const RenderFrameDesc& frame) override;
		void Update(const RenderFrameDesc& frame) override;
		void Render(MeshSorter::DrawPass pass, GraphicsContext& context, GlobalConstants& globals, const RenderFrameDesc& frame) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Deferred
				| RendererFeature::Shadow
				| RendererFeature::SSAO
				| RendererFeature::Transparent
				| RendererFeature::PostProcess;
		}

		uint8_t GetPSO(uint16_t psoFlags) override;

	private:
		void BuildRootSignature();
		void BuildDescriptorHeaps();
		void BuildPSOs();
		void DestroyForwardBufferTargets();
		void CreateForwardBufferTargets();

		void UpdateGlobalDescriptors();

	private:
		RootSignature m_RootSig;

		DescriptorHeap m_TextureHeap;  // texture堆。存放CBV/SRV/UAV描述符堆
		DescriptorHeap m_SamplerHeap;  // sampler堆
		DescriptorHandle m_CommonTextures; // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。

		std::vector<GraphicsPSO> m_PSOCache;

		ForwardBuffer m_ForwardBuffer;
	};


}
