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

	// 加载HDR图desc
	struct EnvironmentMapLoadDesc
	{
		std::string HDRPath;
		bool UseExternalHDRTexture = false;
	};

	// 环境map Desc
	struct EnvironmentMapBuildDesc
	{
		uint32_t CubemapSize = 1024;
		uint32_t IrradianceSize = 32;
		uint32_t PrefilterSize = 128;
		uint32_t BRDFLUTSize = 512;

		// 0 表示自动计算
		uint32_t MaxPrefilterMipCount = 0;
	};

	// 环境map资源
	struct EnvironmentMapResources
	{
		std::shared_ptr<Texture>  HDRSource;        // 原始 HDR 2D 图
		std::shared_ptr<TextureCube> EnvironmentCube; // HDR -> Cubemap
		std::shared_ptr<TextureCube> IrradianceCube;   // Diffuse IBL
		std::shared_ptr<TextureCube> PrefilterCube;    // Specular IBL
		std::shared_ptr<Texture>  BRDFLUT;          // Specular BRDF LUT

		bool IsValid() const
		{
			return HDRSource != nullptr
				&& EnvironmentCube != nullptr
				&& IrradianceCube != nullptr
				&& PrefilterCube != nullptr
				&& BRDFLUT != nullptr;
		}
	};

	// 环境map所需根签名和PSO
	struct EnvironmentMapRootSigAndPSO
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

		// 载入 HDR。你可以让它内部读文件，也可以外部先解码再塞进来。
		bool LoadHDRI(const EnvironmentMapLoadDesc& desc);

		// 根据 HDR 构建完整环境光照资源
		bool BuildEnvironment(GraphicsContext& ctx, const EnvironmentMapBuildDesc& desc);

		void RenderSkybox(GraphicsContext& gfxcontext, const Camera& camera);

		const EnvironmentMapResources& GetResources() const { return m_Resources; }
		bool IsReady() const { return m_Ready; }

	private:
		EnvironmentMapResources m_Resources;
		std::shared_ptr<EnvironmentMapRootSigAndPSO> m_RootSigAndPSO;
		SamplerDesc m_LinearSamplerDesc;

		bool m_Ready = false;
	private:
		void CreateSkyboxPipeline(DXGI_FORMAT scene, DXGI_FORMAT depth);
		void CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth);
		void CreateSamplerDesc();

		// 你自己的资产系统 / 纹理系统接在这里
		std::shared_ptr<Texture> LoadHDRTexture2D_Impl(const std::string& path);
		std::shared_ptr<TextureCube> CreateTextureCube_Impl(uint32_t size, uint32_t mipCount);
		std::shared_ptr<Texture> CreateTexture2D_Impl(uint32_t width, uint32_t height);


		void BuildEnvironmentCubemap(GraphicsContext& gfxContext, const Texture& hdrSource, TextureCube& outEnvironmentCube, uint32_t cubemapSize);
		void BuildIrradianceCubemap(GraphicsContext& gfxContext, const TextureCube& environmentCube, TextureCube& outIrradianceCube, uint32_t irradianceSize);
		void BuildPrefilterCubemap(GraphicsContext& gfxContext, const TextureCube& environmentCube, TextureCube& outPrefilterCube, uint32_t prefilterSize, uint32_t maxMipCount);
		void BuildBRDFLUT(GraphicsContext& gfxContext, Texture& outBRDFLUT, uint32_t lutSize);

		static uint32_t ComputePrefilterMipCount(uint32_t size);

		static void RemoveTranslationFromViewMatrix(float outViewNoTranslation[16], const float inView[16]);
	};

} // Renderer



