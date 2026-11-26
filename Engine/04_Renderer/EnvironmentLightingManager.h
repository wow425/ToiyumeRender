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
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"


/*
 EnvironmentLightingManager
鈹溾攢鈹€ LoadHDRI() 鍔犺浇HDR
鈹溾攢鈹€ BuildEnvironment() 鐢熸垚cubemap,鐢熸垚 irradiance / prefilter / LUT
鈹溾攢鈹€ GetSkyboxCube()
鈹溾攢鈹€ GetIrradianceCube()
鈹溾攢鈹€ GetPrefilterCube()
鈹溾攢鈹€ GetBRDFLUT()
鈹斺攢鈹€ RenderSkybox() 澶╃┖鐩掔粯鍒?
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
namespace Renderer { struct RenderParams; }
namespace Renderer::EnvironmentLighting
{

	// environmentMap鍏ㄥ绾圭悊
	struct EnvironmentMapTextures
	{
		TextureRef HDRTexture;				// HDR
		TextureRef EnvironmentCubeTexture;	//  Cubemap
		TextureRef IrradianceCubeTexture;	// Diffuse IBL
		TextureRef PrefilterCubeTexture;		// Specular IBL
		TextureRef  BRDFLUTTexture;          // Specular BRDF LUT
	};

	//  environmentMap鍏ㄥRootSig鍜孭SO
	struct EnvironmentMapRootSigAndPSOs
	{
		RootSignature m_EquirectToCubemapRootSig; // hdr杞琧ubemap
		ComputePSO   m_EquirectToCubemapPSO;

		RootSignature m_SkyboxRootSig;		 // skybox缁樺埗
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

		void LoadHDR(const std::wstring& HDRPath);						// 鍔犺浇hdr杞崲涓篸ds
		void BakeEnvironmentTextures(GraphicsContext& gfxContext);		// 灏唄dr绾圭悊鐑樼剻鎴恠kybox,IBL绾圭悊


		// [WP-2A] Skybox 娓叉煋绠＄嚎宸叉娊绂昏嚦鐙珛 Engine/04_Renderer/Passes/SkyboxPass.{h,cpp}銆?


		const EnvironmentMapTextures& GetEnvironmentMapTextureRefs() const { return m_Textures; }


		bool IsReady() const { return m_Ready; }
		// R4锛氳瘖鏂敤銆?鐜鍥惧姞杞芥垚鍔?涓?鐑樼剻瀹屾垚"鏄袱浠朵簨锛?
		// 鍒嗗紑鏆撮湶鎵嶈兘鍦ㄥ惎鍔ㄦ棩蹇楅噷鍖哄垎銆岃祫浜х己澶便€嶄笌銆岀儤鐒欐病璺戙€嶃€?
		bool IsHDRLoaded() const { return m_HDRLoaded; }

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
		// [WP-2A] Skybox RootSig/PSO 宸茶縼鍏ョ嫭绔?SkyboxPass锛屾秷闄?SkyboxVS_cso/SkyboxPS_cso ODR 閲嶅瀹氫箟銆?
		void CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth) const; // TODO
		void CreateSamplerDesc(); // 閰嶇疆绾挎€ampler


		// PreCompute

		void IrradianceConvolutionPass(GraphicsContext& gfxContext); // Diffuse IBL
		void SpecularPrefilterPass(GraphicsContext& gfxContext); // Specular IBL
		void BRDFLUTPass(GraphicsContext& gfxContext);

		void SaveDDS();
	};
} // Renderer



