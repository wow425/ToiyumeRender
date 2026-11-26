
#include "00_Core/PCH.h"
#include "EnvironmentLightingManager.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Resource/Texture.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Command/CommandContext.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureConvert.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Camera/Camera.h"

// 鐫€鑹插櫒瀛楄妭鐮佸ご鎸?HLSL 瀛愮洰褰曢暅鍍忓埌
// out/<Config>/Engine/Shader/06_Environment/锛屽洜姝?include 甯﹀瓙鐩綍鍓嶇紑銆?
#include "06_Environment/EquirectangularToCubemapCS.h"
#include "06_Environment/IrradianceConvolutionCS.h"
#include "06_Environment/SpecularPrefilterCS.h"
#include "06_Environment/BRDFLUTCS.h"
#include <algorithm>

namespace Renderer::EnvironmentLighting
{
	void EnvironmentLightingManager::Initialize(DXGI_FORMAT scene, DXGI_FORMAT depth)
	{
		m_RootSigAndPSOs = std::make_shared<EnvironmentMapRootSigAndPSOs>();
		// 鐢熸垚绾挎€ч噰鏍峰櫒
		CreateSamplerDesc();

		// R3锛氬垏 renderer 蹇呴』閲嶆柊鐑樼剻銆?
		// InitializeToCubemap() 姣忔閮戒細閲嶅缓 4 寮?ColorBuffer锛圗nvironmentCube / Irradiance /
		// Prefilter / BRDFLUT锛夛紝杩欐槸鍏ㄦ柊鐨?GPU 璧勬簮銆佸唴瀹逛负绌恒€備絾 m_Baked / m_Ready /
		// m_HDRLoaded 鑻ヤ笉璺熺潃澶嶄綅锛屽氨浼氬甫鐫€涓婁竴杞?renderer 鐨勭姸鎬佽繘鏉ワ細
		//   - Deferred -> Forward -> Deferred锛氱浜屾 Initialize 鍚?m_Baked 浠嶆槸 true锛?
		//     BakeEnvironmentTextures锛?150锛変細鍥?m_Baked==true 鐩存帴鏃╅€€锛?
		//     浜庢槸 Skybox / IBL 閲囨牱鐨勬案杩滄槸鍒氬缓鍑烘潵銆佷粠鏈啓鍏ュ唴瀹圭殑绌?cubemap锛堢敾闈㈠瀮鍦撅級銆?
		//   - m_HDRLoaded 鍚岀悊锛氬畠鍙備笌 :150 鐨勬棭閫€鍒ゆ柇锛屼笖璇箟鏄€孒DR 宸插姞杞姐€嶏紝
		//     澶嶄綅鍚庢墠鑳芥帴鍙楁柊涓€杞殑 LoadHDR銆?
		// 澶嶄綅蹇呴』鏀惧湪 InitializeToCubemap() 涔嬪墠 鈥斺€?鍏堟湁骞插噣鐨勬爣蹇楋紝鍐嶆湁骞插噣鐨勮祫婧愩€?
		m_Baked = false;
		m_Ready = false;
		m_HDRLoaded = false;

		InitializeToCubemap();
		CreateEquirectangularToCubemapPipeline();

		CreatePrecomputePipelines(scene, depth);

	}

	void EnvironmentLightingManager::Shutdown()
	{
		// R3锛氬垏 renderer 蹇呴』閲嶆柊鐑樼剻锛堣 Initialize 寮€澶寸殑璇存槑锛夈€?
		// 鍏堟妸 4 寮犵儤鐒欎骇鐗╋紙cubemap / irradiance / prefilter / BRDF LUT锛夌湡姝ｉ噴鏀炬帀锛?
		// 瀹冧滑鏄?shared_ptr<ColorBuffer>锛屼粎闈?m_Textures = {} 鏄竻涓嶆帀鐨勶紙閭ｆ槸 TextureRef 琛級锛?
		// 涓嬩竴杞?InitializeToCubemap 閲岀殑 make_shared 鍙槸瑕嗙洊鎸囬拡锛屾棫 GPU 璧勬簮浼氭嫋鍒?
		// 寮曠敤璁℃暟褰掗浂鎵嶉噴鏀撅紝鑰屼笖 GetEnvironmentCube()/GetIrradianceCube() 绛夎闂櫒
		// 鍦?Shutdown 涔嬪悗浠嶄細杩斿洖涓婁竴杞殑鎮┖瑙嗗浘锛圖eferredRenderer 鐨?Lighting Pass
		// 涓?SkyboxPass 閮界洿鎺ョ敤杩欎簺璁块棶鍣級銆?
		m_EnvironmentCubeMap.reset();
		m_IrradianceCubeMap.reset();
		m_PrefilterCubeMap.reset();
		m_BRDFLUT.reset();

		// 鍐嶅浣嶄笁涓姸鎬佹爣蹇楋細鍚﹀垯 Forward -> Deferred 鐨勭浜岃疆 Initialize 浼氬洜
		// m_Baked == true 璺宠繃鐑樼剻锛岀暀涓嬫案涓嶅～鍏呯殑绌?cubemap銆?
		m_Baked = false;
		m_Ready = false;
		m_HDRLoaded = false;

		m_Textures = {};
		m_RootSigAndPSOs.reset();
	}

	// [WP-2A] Skybox 绠＄嚎宸叉娊绂昏嚦鐙珛 Engine/04_Renderer/Passes/SkyboxPass.{h,cpp}銆?

	// 鏋勫缓IBL鎵€闇€鐨凴ootSig鍜孭SO
	void EnvironmentLightingManager::CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth) const
	{
		(void)scene;
		(void)depth;
			// irradianceRootSig鍜孭SO
		auto& irradianceRootSig = m_RootSigAndPSOs->m_IrradianceRootSig;
		irradianceRootSig.Reset(3, 1);
		irradianceRootSig.InitStaticSampler(0, m_LinearSamplerDesc, D3D12_SHADER_VISIBILITY_ALL);
		irradianceRootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
		irradianceRootSig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		irradianceRootSig[2].InitAsConstantBuffer(0);
		irradianceRootSig.Finalize(L"Irradiance Convolution RootSig");

		auto& irradiancePSO = m_RootSigAndPSOs->m_IrradiancePSO;
		irradiancePSO.SetName(L"EnvironmentLighting : Irradiance Convolution PSO");
		irradiancePSO.SetRootSignature(irradianceRootSig);
		irradiancePSO.SetComputeShader(IrradianceConvolutionCS_cso, sizeof(IrradianceConvolutionCS_cso));
		irradiancePSO.Finalize();
			// prefilterRootSig鍜孭SO
		auto& prefilterRootSig = m_RootSigAndPSOs->m_PrefilterRootSig;
		prefilterRootSig.Reset(3, 1);
		prefilterRootSig.InitStaticSampler(0, m_LinearSamplerDesc, D3D12_SHADER_VISIBILITY_ALL);
		prefilterRootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
		prefilterRootSig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		prefilterRootSig[2].InitAsConstantBuffer(0);
		prefilterRootSig.Finalize(L"Specular Prefilter RootSig");
			auto& prefilterPSO = m_RootSigAndPSOs->m_PrefilterPSO;
			prefilterPSO.SetName(L"EnvironmentLighting : Specular Prefilter PSO");
			prefilterPSO.SetRootSignature(prefilterRootSig);
			prefilterPSO.SetComputeShader(SpecularPrefilterCS_cso, sizeof(SpecularPrefilterCS_cso));
			prefilterPSO.Finalize();

			auto& brdfRootSig = m_RootSigAndPSOs->m_BRDFLUTRootSig;
			brdfRootSig.Reset(2, 0);
			brdfRootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
			brdfRootSig[1].InitAsConstantBuffer(0);
		brdfRootSig.Finalize(L"BRDF LUT RootSig");
			auto& brdfPSO = m_RootSigAndPSOs->m_BRDFLUTPSO;
			brdfPSO.SetName(L"EnvironmentLighting : BRDF LUT PSO");
		brdfPSO.SetRootSignature(brdfRootSig);
		brdfPSO.SetComputeShader(BRDFLUTCS_cso, sizeof(BRDFLUTCS_cso));
			brdfPSO.Finalize();
	}

	void EnvironmentLightingManager::SaveDDS()
	{

	}

	void EnvironmentLightingManager::CreateSamplerDesc()
	{
		// TODO:
		// 鍒涘缓绾挎€ч噰鏍峰櫒銆?
		// Skybox / IBL 涓€鑸兘闇€瑕佷竴涓嚎鎬?SamplerDesc銆?
		m_LinearSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		m_LinearSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		m_LinearSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		m_LinearSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		m_LinearSamplerDesc.MipLODBias = 0.0f;
		m_LinearSamplerDesc.MaxAnisotropy = 1;
		m_LinearSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		m_LinearSamplerDesc.BorderColor[0] = 0.0f;
		m_LinearSamplerDesc.BorderColor[1] = 0.0f;
		m_LinearSamplerDesc.BorderColor[2] = 0.0f;
		m_LinearSamplerDesc.BorderColor[3] = 0.0f;
		m_LinearSamplerDesc.MinLOD = 0.0f;
		m_LinearSamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	}

	void EnvironmentLightingManager::BakeEnvironmentTextures(GraphicsContext& gfxContext)
	{
		(void)gfxContext;
		// R4锛氭棭閫€鏉′欢淇銆?
		// ------------------------------------------------------------
		// 鏃у啓娉曟槸 `if (m_Baked || m_HDRLoaded || !HDRTexture.IsValid()) return;`
		// 鎶?m_HDRLoaded 褰撴垚浜?绂佹鐑樼剻"鐨勭悊鐢?鈥斺€?璇箟姝ｅソ鍙嶄簡锛?
		// 銆孒DR 宸插姞杞姐€嶆伆鎭版槸**鍙互鐑樼剻**鐨勫墠鎻愶紝鑰屼笉鏄笉鑳界儤鐒欑殑鍘熷洜銆?
		// 杩欎釜閿欒涓€鐩存病鏆撮湶锛屽彧鍥犱负 m_HDRLoaded 浠庢潵娌′汉缃?true锛堟鏍囧織锛夛紝
		// 鏁存潯 `|| m_HDRLoaded` 鎭掍负鍋囥€備竴鏃︽湁浜烘寜瀛楅潰鎰忔€濊ˉ涓婅祴鍊硷紝
		// 鐑樼剻灏变細琚案涔呰烦杩囥€丼kybox 涓?IBL 鍏ㄩ儴閫€鍥炵┖ cubemap銆?
		//
		// 鐜板湪鏀逛负锛氬凡缁忕儤鐒欒繃灏辫烦杩囷紱娌℃湁鍙敤鐨?HDR 涔熻烦杩囷紱
		// 銆屽姞杞借繃 HDR銆嶄笉鍐嶅弬涓庣煭璺垽鏂紝鍙綔涓?LoadHDR 鐨勬垚鍔熸爣蹇椼€?
		if (m_Baked || !m_Textures.HDRTexture.IsValid()) return;

		ComputeContext& computeContext = ComputeContext::Begin(L"Environment IBL Bake");

		EquirectangularToCubemapPass(computeContext.GetGraphicsContext());
		IrradianceConvolutionPass(computeContext.GetGraphicsContext());
		SpecularPrefilterPass(computeContext.GetGraphicsContext());
		BRDFLUTPass(computeContext.GetGraphicsContext());

		computeContext.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		computeContext.TransitionResource(*m_IrradianceCubeMap,	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		computeContext.TransitionResource(*m_PrefilterCubeMap,	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		computeContext.TransitionResource(*m_BRDFLUT,			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		computeContext.Finish(true);

		m_Baked = true;
		m_Ready = true;
	}

	void EnvironmentLightingManager::LoadHDR(const std::wstring& HDRPath)
	{
		// 1. 鍔犺浇鐜璐村浘hdr,鐢熸垚dds
		CompileTextureOnDemand(HDRPath, kDefaultBC | kQualityBC);
		m_Textures.HDRTexture = TextureManager::LoadDDSFromFile(HDRPath);
		// R4锛歮_HDRLoaded 鐜板湪鏈夌湡瀹炶涔?鈥斺€?"鏈疆鐨?HDR 鍔犺浇鎴愬姛"銆?
		// 瀹冧笉鍐嶅弬涓庣儤鐒欑殑鐭矾鍒ゆ柇锛堣 BakeEnvironmentTextures锛夛紝鍙敤浜庤瘖鏂笌鐘舵€佹煡璇€?
		m_HDRLoaded = m_Textures.HDRTexture.IsValid();
		if (m_Textures.HDRTexture.IsValid() == false)
		{
			// R0.2锛氬師淇℃伅鏃㈡病鏈夎矾寰勪篃娌℃湁鍚庢灉锛屾帓鏌ユ椂鏃犳硶鍖哄垎銆岃矾寰勯敊銆嶅拰銆岃В鐮佸け璐ャ€嶃€?
			Utility::Printf(L"[Asset] 閿欒锛氱幆澧冨浘鍔犺浇澶辫触 -> %ws\n", HDRPath.c_str());
			Utility::Printf(L"[Asset]   璇风‘璁ゆ枃浠跺瓨鍦ㄤ笖涓哄彲璇荤殑 .dds/.hdr锛汼kybox 涓?IBL 灏嗚烦杩囥€俓n");
		}
	}



	void EnvironmentLightingManager::EquirectangularToCubemapPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 杩欐槸 HDR equirectangular -> cubemap 鐨勭敓鎴愭楠ゃ€?
		//
		// 鎺ㄨ崘瀹炵幇鏂瑰紡锛?
		// - compute shader 鍐欏叆 cubemap 6 faces
		// - 鎴栬€?6 娆?raster pass 娓叉煋鍒?cube 鐨?6 涓潰
		//
		// 鏍稿績閫昏緫锛?
		// 1) 鏍规嵁 face index 鍜?texel 鍧愭爣璁＄畻鏂瑰悜鍚戦噺
		// 2) 灏嗘柟鍚戝悜閲忚浆鎴?latlong UV
		// 3) 閲囨牱 HDR 2D 绾圭悊
		// 4) 杈撳嚭鍒?cubemap 瀵瑰簲 face

		/* 璺?瓒烶ass
		* 琛屼负: 缁戝畾RootSig,PSO, 鏍瑰疄鍙?瑙嗗彛鐭╅樀,瑁佸壀鐭╅樀,RT,缁樺埗鍏ㄥ睆涓夎褰?
		* 璧勬簮RootSig(鑷垱),PSO(鑷垱), 鏍瑰疄鍙?t0涓篐DR Map, s0涓洪潤鎬?point Sampler),瑙嗗彛鐭╅樀鍜岃鍓煩闃?浼犲弬),RT(鑷垱),VS,PS
		*
		*/
		ComputeContext& context = gfxContext.GetComputeContext();

		struct alignas(16) EnvironmentBakeCB
		{
			uint32_t Size;
			uint32_t MipLevel;
			float Roughness;
			uint32_t SampleCount;
		} constants = { m_EnvironmentCubeSize, 0, 0.0f, 1024 };

		context.SetRootSignature(m_RootSigAndPSOs->m_EquirectToCubemapRootSig);
		context.SetPipelineState(m_RootSigAndPSOs->m_EquirectToCubemapPSO);
		context.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		context.SetDynamicDescriptor(0, 0, m_Textures.HDRTexture.GetSRV());
		context.SetDynamicDescriptor(1, 0, m_EnvironmentCubeMap->GetUAV(0));
		context.SetDynamicConstantBufferView(2, sizeof(constants), &constants);

		context.Dispatch3D(m_EnvironmentCubeSize, m_EnvironmentCubeSize, 6, 8, 8, 1);

		context.InsertUAVBarrier(*m_EnvironmentCubeMap);
	}

	void EnvironmentLightingManager::IrradianceConvolutionPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 婕弽灏勭幆澧冨厜鍗风Н銆?
		// 瀵规瘡涓緭鍑烘柟鍚戝仛 cosine-weighted hemisphere integration銆?
		// 浣庨锛屽垎杈ㄧ巼涓€鑸緢灏忋€?

		ComputeContext& context = gfxContext.GetComputeContext();

		struct alignas(16) EnvironmentBakeCB
		{
			uint32_t Size;
			uint32_t MipLevel;
			float Roughness;
			uint32_t SampleCount;
		} constants = { m_IrradianceCubeSize, 0, 0.0f, 1024 };

		context.SetRootSignature(m_RootSigAndPSOs->m_IrradianceRootSig);
		context.SetPipelineState(m_RootSigAndPSOs->m_IrradiancePSO);
		context.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*m_IrradianceCubeMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		context.SetDynamicDescriptor(0, 0, m_EnvironmentCubeMap->GetSRV());
		context.SetDynamicDescriptor(1, 0, m_IrradianceCubeMap->GetUAV(0));
		context.SetDynamicConstantBufferView(2, sizeof(constants), &constants);

		context.Dispatch3D(m_IrradianceCubeSize, m_IrradianceCubeSize, 6, 8, 8, 1);

		context.InsertUAVBarrier(*m_IrradianceCubeMap);
	}

	void EnvironmentLightingManager::SpecularPrefilterPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 闀滈潰 IBL 鐨勯婊ゆ尝鐜璐村浘銆?
		// 姣忎釜 mip 瀵瑰簲涓€涓?roughness锛?
		// roughness = mip / (maxMipCount - 1)
		//
		// 鍏抽敭锛?
		// - GGX importance sampling
		// - 杈撳嚭鍒颁笉鍚?mip level
		ComputeContext& context = gfxContext.GetComputeContext();

		context.SetRootSignature(m_RootSigAndPSOs->m_PrefilterRootSig);
		context.SetPipelineState(m_RootSigAndPSOs->m_PrefilterPSO);

		context.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*m_PrefilterCubeMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		for (uint32_t mip = 0; mip < m_PrefilterMipCount; ++mip)
		{
			const uint32_t mipSize = std::max(1u, m_PrefilterCubeSize >> mip);
			const float roughness = m_PrefilterMipCount > 1 ? (float)mip / (float)(m_PrefilterMipCount - 1) : 0.0f;
			// [TD-06b] 涓?SpecularPrefilterCS.hlsl 鐨?SpecularPrefilterCB 涓ユ牸瀵归綈锛?2 B锛夛細
			// 鍒嗙鐩爣 mip 灏哄锛圖stSize锛変笌婧愮幆澧冪珛鏂逛綋灏哄锛圫rcSize锛夛紝骞舵樉寮忎紶鍏?SrcMaxMip = 0.0f
			// 锛堝洜涓?EquirectangularToCubemapPass 浠呭啓鍏ユ簮 EnvironmentCubeMap 鐨?mip 0锛夈€?
			struct alignas(16) SpecularPrefilterCB
			{
				uint32_t DstSize;      // offset  0
				uint32_t SrcSize;      // offset  4
				float    Roughness;    // offset  8
				uint32_t SampleCount;  // offset 12
				float    SrcMaxMip;    // offset 16
				float    Pad0[3];      // offset 20..31
			};
			static_assert(sizeof(SpecularPrefilterCB) == 32 && sizeof(SpecularPrefilterCB) % 16 == 0,
				"SpecularPrefilterCB must match HLSL layout and 16B alignment");

			SpecularPrefilterCB constants = { mipSize, m_EnvironmentCubeSize, roughness, 1024u, 0.0f, { 0.0f, 0.0f, 0.0f } };

			context.SetDynamicDescriptor(0, 0, m_EnvironmentCubeMap->GetSRV());
			context.SetDynamicDescriptor(1, 0, m_PrefilterCubeMap->GetUAV(mip));
			context.SetDynamicConstantBufferView(2, sizeof(constants), &constants);
			context.Dispatch3D(mipSize, mipSize, 6, 8, 8, 1);
			context.InsertUAVBarrier(*m_PrefilterCubeMap);
		}
	}

	void EnvironmentLightingManager::BRDFLUTPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 鐢熸垚 DFG BRDF LUT銆?
		// 渚?specular IBL 浣跨敤銆?
		//
		// 甯歌鍋氭硶锛?
		// - fullscreen triangle
		// - 鎴?compute shader

		ComputeContext& context = gfxContext.GetComputeContext();

		struct alignas(16) EnvironmentBakeCB
		{
			uint32_t Size;
			uint32_t MipLevel;
			float Roughness;
			uint32_t SampleCount;
		} constants = { m_BRDFLUTSize, 0, 0.0f, 1024 };

		context.SetRootSignature(m_RootSigAndPSOs->m_BRDFLUTRootSig);
		context.SetPipelineState(m_RootSigAndPSOs->m_BRDFLUTPSO);
		context.TransitionResource(*m_BRDFLUT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		context.SetDynamicDescriptor(0, 0, m_BRDFLUT->GetUAV(0));
		context.SetDynamicConstantBufferView(1, sizeof(constants), &constants);
		context.Dispatch2D(m_BRDFLUTSize, m_BRDFLUTSize, 8, 8);
		context.InsertUAVBarrier(*m_BRDFLUT);
	}


	// [WP-2A] SkyboxPass 鎵ц鍏ュ彛宸茶縼鍏ョ嫭绔?Renderer::SkyboxPass::Execute銆?

	void EnvironmentLightingManager::CreateEquirectangularToCubemapPipeline()
	{
		auto& rootSig = m_RootSigAndPSOs->m_EquirectToCubemapRootSig;
		rootSig.Reset(3, 1);
		rootSig.InitStaticSampler(0, m_LinearSamplerDesc, D3D12_SHADER_VISIBILITY_ALL);
		rootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
		rootSig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		rootSig[2].InitAsConstantBuffer(0);
		rootSig.Finalize(L"Equirectangular To Cubemap RootSig");

		auto& pso = m_RootSigAndPSOs->m_EquirectToCubemapPSO;
		pso.SetName(L"EnvironmentLighting : Equirectangular To Cubemap PSO");
		pso.SetRootSignature(rootSig);
		pso.SetComputeShader(EquirectangularToCubemapCS_cso, sizeof(EquirectangularToCubemapCS_cso));
		pso.Finalize();
	}

	void EnvironmentLightingManager::InitializeToCubemap()
	{
		// [TD-06b] EquirectangularToCubemapPass 浠呯儤鐒?mip 0锛涙簮鐜璐村浘鍒嗛厤 1 绾?mip锛?
		// 褰诲簳鏉滅粷楂橀樁鏈啓鍏ラ粦 mip 琚閲囷紝鍚屾椂鑺傜渷 33% 绔嬫柟浣撴樉瀛樸€?
		m_EnvironmentCubeMap = std::make_shared<ColorBuffer>();
		m_EnvironmentCubeMap->CreateCube(L"EnvironmentCubeMap", m_EnvironmentCubeSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_IrradianceCubeMap = std::make_shared<ColorBuffer>();
		m_IrradianceCubeMap->CreateCube(L"IrradianceCubeMap", m_IrradianceCubeSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_PrefilterCubeMap = std::make_shared<ColorBuffer>();
		m_PrefilterCubeMap->CreateCube(L"PrefilterCubeMap", m_PrefilterCubeSize, m_PrefilterMipCount, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_BRDFLUT = std::make_shared<ColorBuffer>();
		m_BRDFLUT->Create(L"BRDF LUT", m_BRDFLUTSize, m_BRDFLUTSize, 1, DXGI_FORMAT_R16G16_FLOAT);
	}

}

