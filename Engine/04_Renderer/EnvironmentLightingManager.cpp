
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
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Importers/Texture/TextureConvert.h"
#include "04_Renderer/BufferManager.h"
#include "05_Scene/Camera/Camera.h"

#include "SkyboxVS.h"
#include "SkyboxPS.h"
#include "EquirectangularToCubemapCS.h"
#include "IrradianceConvolutionCS.h"
#include "SpecularPrefilterCS.h"
#include "BRDFLUTCS.h"
#include <algorithm>

namespace Renderer::EnvironmentLighting
{
	void EnvironmentLightingManager::Initialize(DXGI_FORMAT scene, DXGI_FORMAT depth)
	{
		m_RootSigAndPSOs = std::make_shared<EnvironmentMapRootSigAndPSOs>();
		// 生成线性采样器
		CreateSamplerDesc();

		InitializeToCubemap();
		CreateEquirectangularToCubemapPipeline();

		CreateSkyboxPipeline(scene, depth);
		CreatePrecomputePipelines(scene, depth);

	}

	void EnvironmentLightingManager::Shutdown()
	{
		m_Textures = {};
		m_RootSigAndPSOs.reset();
	}

	void EnvironmentLightingManager::CreateSkyboxPipeline(DXGI_FORMAT scene, DXGI_FORMAT depth) const
	{
		using namespace Renderer;

		auto& skyboxRootSig = m_RootSigAndPSOs->m_SkyboxRootSig;
		auto& skyboxPSO = m_RootSigAndPSOs->m_SkyboxPSO;

		// 根签名设置并finalize
		skyboxRootSig.Reset(2, 1);
		skyboxRootSig.InitStaticSampler(0, m_LinearSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // s0: Linear SamplerDesc
		skyboxRootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_ALL); // t0: Environment Cubemap.
		skyboxRootSig[1].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL); // b0: Camera
		skyboxRootSig.Finalize(L"skybox Pass skyboxRootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);


		skyboxPSO.SetName(L"DeferredRenderer : skybox PSO");
		skyboxPSO.SetRootSignature(skyboxRootSig);                                           // 根签名
		skyboxPSO.SetRasterizerState(Graphics::RasterizerFullScreen);                                 // 光栅状态
		skyboxPSO.SetDepthStencilState(Graphics::DepthStateReadOnly);
		skyboxPSO.SetBlendState(Graphics::BlendDisable);                                           // 混合模式     默认关闭
		skyboxPSO.SetInputLayout(0, nullptr);                                            // 输入布局
		skyboxPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
		skyboxPSO.SetRenderTargetFormat(scene, depth);
		skyboxPSO.SetVertexShader(SkyboxVS_cso, sizeof(SkyboxVS_cso));
		skyboxPSO.SetPixelShader(SkyboxPS_cso, sizeof(SkyboxPS_cso));
		skyboxPSO.Finalize();
	}

	// 构建IBL所需的RootSig和PSO
	void EnvironmentLightingManager::CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth) const
	{
		(void)scene;
		(void)depth;
			// irradianceRootSig和PSO
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
			// prefilterRootSig和PSO
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
		// 创建线性采样器。
		// Skybox / IBL 一般都需要一个线性 SamplerDesc。
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
		if (m_Baked || m_HDRLoaded || !m_Textures.HDRTexture.IsValid()) return;

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
		// 1. 加载环境贴图hdr,生成dds
		CompileTextureOnDemand(HDRPath, kDefaultBC | kQualityBC);
		m_Textures.HDRTexture = TextureManager::LoadDDSFromFile(HDRPath);
		if (m_Textures.HDRTexture.IsValid() == false)
		{
			Utility::Printf(L"HDRTexture Load Is Failed");
		}
	}



	void EnvironmentLightingManager::EquirectangularToCubemapPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 这是 HDR equirectangular -> cubemap 的生成步骤。
		//
		// 推荐实现方式：
		// - compute shader 写入 cubemap 6 faces
		// - 或者 6 次 raster pass 渲染到 cube 的 6 个面
		//
		// 核心逻辑：
		// 1) 根据 face index 和 texel 坐标计算方向向量
		// 2) 将方向向量转成 latlong UV
		// 3) 采样 HDR 2D 纹理
		// 4) 输出到 cubemap 对应 face

		/* 跑1趟Pass
		* 行为: 绑定RootSig,PSO, 根实参,视口矩阵,裁剪矩阵,RT,绘制全屏三角形
		* 资源RootSig(自创),PSO(自创), 根实参(t0为HDR Map, s0为静态 point Sampler),视口矩阵和裁剪矩阵(传参),RT(自创),VS,PS
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
		// 漫反射环境光卷积。
		// 对每个输出方向做 cosine-weighted hemisphere integration。
		// 低频，分辨率一般很小。

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
		// 镜面 IBL 的预滤波环境贴图。
		// 每个 mip 对应一个 roughness：
		// roughness = mip / (maxMipCount - 1)
		//
		// 关键：
		// - GGX importance sampling
		// - 输出到不同 mip level
		ComputeContext& context = gfxContext.GetComputeContext();

		context.SetRootSignature(m_RootSigAndPSOs->m_PrefilterRootSig);
		context.SetPipelineState(m_RootSigAndPSOs->m_PrefilterPSO);

		context.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*m_PrefilterCubeMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		for (uint32_t mip = 0; mip < m_PrefilterMipCount; ++mip)
		{
			const uint32_t mipSize = std::max(1u, m_PrefilterCubeSize >> mip);
			const float roughness = m_PrefilterMipCount > 1 ? (float)mip / (float)(m_PrefilterMipCount - 1) : 0.0f;
			struct alignas(16) EnvironmentBakeCB
			{
				uint32_t Size;
				uint32_t MipLevel;
				float Roughness;
				uint32_t SampleCount;
			} constants = { mipSize, mip, roughness, 1024 };

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
		// 生成 DFG BRDF LUT。
		// 供 specular IBL 使用。
		//
		// 常见做法：
		// - fullscreen triangle
		// - 或 compute shader

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


	void EnvironmentLightingManager::SkyboxPass(GraphicsContext& gfxcontext, const Scene::Camera::Camera& camera)
	{
		// TODO:
		// 1) 设置深度状态：DepthFunc = LessEqual，DepthWrite = Off
		// 2) 绑定 Skybox PSO / RootSig
		// 3) 绑定 environment cube 到 t0
		// 4) 绑定 linear SamplerDesc 到 s0
		// 5) 使用去掉 translation 的 view matrix
		// 6) 绘制 cube

		if (!m_Ready || m_EnvironmentCubeMap == nullptr) return;

		struct alignas(16) SkyboxCameraCB
		{
			Math::Matrix4 InvView;
			Math::Matrix4 InvProj;
		} cameraCB = { Math::Invert(camera.GetViewMatrix()), Math::Invert(camera.GetProjMatrix()) };

		gfxcontext.SetRootSignature(m_RootSigAndPSOs->m_SkyboxRootSig); // skybox要点
		gfxcontext.SetPipelineState(m_RootSigAndPSOs->m_SkyboxPSO);		//  skybox要点

		gfxcontext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		gfxcontext.TransitionResource(*m_EnvironmentCubeMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		gfxcontext.SetDynamicDescriptor(0, 0, m_EnvironmentCubeMap->GetSRV());
		gfxcontext.SetDynamicConstantBufferView(1, sizeof(cameraCB), &cameraCB);

		gfxcontext.Draw(3, 0);
	}

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
		uint32_t envMip = floor(log2(m_EnvironmentCubeSize)) + 1;
		
		m_EnvironmentCubeMap = std::make_shared<ColorBuffer>();
		m_EnvironmentCubeMap->CreateCube(L"EnvironmentCubeMap", m_EnvironmentCubeSize, envMip, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_IrradianceCubeMap = std::make_shared<ColorBuffer>();
		m_IrradianceCubeMap->CreateCube(L"IrradianceCubeMap", m_IrradianceCubeSize, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_PrefilterCubeMap = std::make_shared<ColorBuffer>();
		m_PrefilterCubeMap->CreateCube(L"PrefilterCubeMap", m_PrefilterCubeSize, m_PrefilterMipCount, DXGI_FORMAT_R16G16B16A16_FLOAT);

		m_BRDFLUT = std::make_shared<ColorBuffer>();
		m_BRDFLUT->Create(L"BRDF LUT", m_BRDFLUTSize, m_BRDFLUTSize, 1, DXGI_FORMAT_R16G16_FLOAT);
	}

}

