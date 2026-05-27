#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "02_RHI/Pipeline/PipelineState.h"
/*
 EnvironmentLightingManager
├── LoadHDRI() 加载HDR
├── BuildEnvironment() 生成cubemap,生成 irradiance / prefilter / LUT
├── GetSkyboxCube()
├── GetIrradianceCube()
├── GetPrefilterCube()
├── GetBRDFLUT()
└── RenderSkybox() 天空盒绘制
*/

class Texture;
class TextureCube;
class GpuBuffer;
class SamplerDescDesc;
class GraphicsPSO;
class RootSignature;
class GraphicsContext;
class Camera;
class ByteAddressBuffer;


namespace Renderer::Deferred
{

	// environmentMap全套纹理
	struct EnvironmentMapTextureRefs
	{
		TextureRef EnvironmentCubeTextureRef; // HDR -> Cubemap

		TextureRef IrradianceCubeTextureRef;   // Diffuse IBL
		TextureRef PrefilterCubeTextureRef;    // Specular IBL
		TextureRef  BRDFLUTTextureRef;          // Specular BRDF LUT
	};

	//  environmentMap全套RootSig和PSO
	struct EnvironmentMapRootSigAndPSOs
	{
		RootSignature m_SkyboxRootSig;
		GraphicsPSO     m_SkyboxPSO;

		RootSignature m_EquirectToCubemapRootSig;
		GraphicsPSO   m_EquirectToCubemapPSO;

		RootSignature m_IrradianceRootSig;
		GraphicsPSO     m_IrradiancePSO;

		RootSignature m_PrefilterRootSig;
		GraphicsPSO     m_PrefilterPSO;

		RootSignature m_BRDFLUTRootSig;
		GraphicsPSO     m_BRDFLUTPSO;
	};

	class EnvironmentLightingManager
	{
	public:
		EnvironmentLightingManager() = default;
		~EnvironmentLightingManager() = default;

		void Initialize(DXGI_FORMAT scene, DXGI_FORMAT depth);
		void Shutdown();
		// 加载hdr转换为dds, 并创建environmentmap全套textureRef(包含预计算)
		bool LoadHDR(const std::wstring& HDRPath); // TODO
		// 绘制skybox
		void RenderSkyboxPass(GraphicsContext& gfxcontext, const Camera& camera); // TODO

		const EnvironmentMapTextureRefs& GetEnvironmentMapTextureRefs() const { return m_TextureRefs; }
		bool IsReady() const { return m_Ready; }

	private:
		EnvironmentMapTextureRefs m_TextureRefs;
		std::shared_ptr<EnvironmentMapRootSigAndPSOs> m_RootSigAndPSOs;
		SamplerDesc m_LinearSamplerDesc;

		bool m_Ready = false;
	private:
		// 创建RootSig和PSO
		void CreateSkyboxPipeline(DXGI_FORMAT scene, DXGI_FORMAT depth);
		void CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth); // TODO
		void CreateSamplerDesc(); // 配置线性sampler

		// 创建TextureRef(带有SRV的纹理)
		// HDR Equirectangular → Cubemap → IBL预计算
		// HDR图本身作为纹理,可用作cubemap,而 diffuse IBL跟Specular IBL需要进行预处理生成纹理
		void BuildEnvironmentCubemap(GraphicsContext& gfxContext, const Texture& hdrSource, TextureCube& outEnvironmentCube, uint32_t cubemapSize); // TODO
		// Diffuse IBL
		void BuildIrradianceCubemap(GraphicsContext& gfxContext, const TextureCube& environmentCube, TextureCube& outIrradianceCube, uint32_t irradianceSize); // TODO
		// Specular IBL
		void BuildPrefilterCubemap(GraphicsContext& gfxContext, const TextureCube& environmentCube, TextureCube& outPrefilterCube, uint32_t prefilterSize, uint32_t maxMipCount); // TODO
		void BuildBRDFLUT(GraphicsContext& gfxContext, Texture& outBRDFLUT, uint32_t lutSize); // TODO

		static uint32_t ComputePrefilterMipCount(uint32_t size); // TODO

		static void RemoveTranslationFromViewMatrix(float outViewNoTranslation[16], const float inView[16]); // TODO
	};

} // Renderer



