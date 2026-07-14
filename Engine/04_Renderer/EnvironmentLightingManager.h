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
#include "03_AssetSystem/Importers/Texture/TextureManager.h"


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
class ByteAddressBuffer;

namespace Scene::Camera{ class Camera; }
namespace Renderer::EnvironmentLighting
{

	// environmentMap全套纹理
	struct EnvironmentMapTextures
	{
		TextureRef HDRTexture;				// HDR
		TextureRef EnvironmentCubeTexture;	//  Cubemap
		TextureRef IrradianceCubeTexture;	// Diffuse IBL
		TextureRef PrefilterCubeTexture;		// Specular IBL
		TextureRef  BRDFLUTTexture;          // Specular BRDF LUT
	};

	//  environmentMap全套RootSig和PSO
	struct EnvironmentMapRootSigAndPSOs
	{
		RootSignature m_EquirectToCubemapRootSig; // hdr转cubemap
		ComputePSO   m_EquirectToCubemapPSO;

		RootSignature m_SkyboxRootSig;		 // skybox绘制
		GraphicsPSO     m_SkyboxPSO;

		RootSignature m_IrradianceRootSig;		// Diffuse IBL
		ComputePSO     m_IrradiancePSO;

		RootSignature m_PrefilterRootSig;		// Specular IBL
		ComputePSO     m_PrefilterPSO;

		RootSignature	m_BRDFLUTRootSig;			// Specular BRDF LUT
		ComputePSO     m_BRDFLUTPSO;
	};

	class EnvironmentLightingManager
	{
	public:
		EnvironmentLightingManager() = default;
		~EnvironmentLightingManager() = default;

		void Initialize(DXGI_FORMAT scene, DXGI_FORMAT depth);
		void Shutdown();

		void LoadHDR(const std::wstring& HDRPath);						// 加载hdr转换为dds
		void BakeEnvironmentTextures(GraphicsContext& gfxContext);		// 将hdr纹理烘焙成skybox,IBL纹理


		void SkyboxPass(GraphicsContext& gfxcontext, const  Scene::Camera::Camera& camera); // 绘制skybox


		const EnvironmentMapTextures& GetEnvironmentMapTextureRefs() const { return m_Textures; }


		bool IsReady() const { return m_Ready; }

		const ColorBuffer* GetEnvironmentCube() const { return m_EnvironmentCubeMap.get(); }
		const ColorBuffer* GetIrradianceCube() const { return m_IrradianceCubeMap.get(); }
		const ColorBuffer* GetPrefilterCube() const { return m_PrefilterCubeMap.get(); }
		const ColorBuffer* GetBRDFLUT() const { return m_BRDFLUT.get(); }

		uint32_t GetPrefilterMipCount() const { return m_PrefilterMipCount; }
	private:
		EnvironmentMapTextures m_Textures = {};
		std::shared_ptr<EnvironmentMapRootSigAndPSOs> m_RootSigAndPSOs = nullptr;
		SamplerDesc m_LinearSamplerDesc;

		bool m_HDRLoaded = false;
		bool m_Baked = false;
		bool m_Ready = false;
		uint32_t m_EnvironmentCubeSize = 512;
		uint32_t m_IrradianceCubeSize = 32;
		uint32_t m_PrefilterCubeSize = 128;
		uint32_t m_PrefilterMipCount = 5;
		uint32_t m_BRDFLUTSize = 256;

	private:
		std::shared_ptr<ColorBuffer> m_EnvironmentCubeMap;
		std::shared_ptr<ColorBuffer> m_IrradianceCubeMap;
		std::shared_ptr<ColorBuffer> m_PrefilterCubeMap;
		std::shared_ptr<ColorBuffer> m_BRDFLUT;

		void InitializeToCubemap();
		void CreateEquirectangularToCubemapPipeline();
		void EquirectangularToCubemapPass(GraphicsContext& gfxContext);
	private:
		void CreateSkyboxPipeline(DXGI_FORMAT scene, DXGI_FORMAT depth) const;	// 创建RootSig和PSO
		void CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth) const; // TODO
		void CreateSamplerDesc(); // 配置线性sampler


		// PreCompute

		void IrradianceConvolutionPass(GraphicsContext& gfxContext); // Diffuse IBL
		void SpecularPrefilterPass(GraphicsContext& gfxContext); // Specular IBL
		void BRDFLUTPass(GraphicsContext& gfxContext);

		void SaveDDS();
	};
} // Renderer



