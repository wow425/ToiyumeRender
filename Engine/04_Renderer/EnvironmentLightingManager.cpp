
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

	void EnvironmentLightingManager::CreateSkyboxPipeline(DXGI_FORMAT scene, DXGI_FORMAT depth)
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

	void EnvironmentLightingManager::CreatePrecomputePipelines(DXGI_FORMAT scene, DXGI_FORMAT depth)
	{
		// TODO:
		// 1) Equirectangular -> Cubemap
		// 2) Irradiance convolution
		// 3) Prefilter env map
		// 4) BRDF LUT
		//
		// 你可以选择：
		// - 全部用 compute shader
		// - 或者部分用 fullscreen pass / cubemap face raster pass
		//
		// 个人渲染器里建议：
		// - Equirect -> Cubemap：compute 或 6 face raster
		// - Irradiance：compute
		// - Prefilter：compute
		// - BRDF LUT：fullscreen quad 或 compute
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

		//EquirectangularToCubemapPass(); // CubeMap

		//IrradianceConvolutionPass(); // Diffuse IBL

		//SpecularPrefilterPass(); // Specular IBL

		//BRDFLUTPass();

		//SaveDDS();
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

	}

	void EnvironmentLightingManager::IrradianceConvolutionPass(GraphicsContext& gfxContext)
	{
		// TODO:
		// 漫反射环境光卷积。
		// 对每个输出方向做 cosine-weighted hemisphere integration。
		// 低频，分辨率一般很小。
		(void)gfxContext;
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
		(void)gfxContext;
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
		(void)gfxContext;
	}


	void EnvironmentLightingManager::SkyboxPass(GraphicsContext& gfxcontext, const Camera& camera)
	{
		//if (!m_Ready || m_TextureRefs.EnvironmentCubeTextureRef.IsValid()) return;


		// TODO:
		// 1) 设置深度状态：DepthFunc = LessEqual，DepthWrite = Off
		// 2) 绑定 Skybox PSO / RootSig
		// 3) 绑定 environment cube 到 t0
		// 4) 绑定 linear SamplerDesc 到 s0
		// 5) 使用去掉 translation 的 view matrix
		// 6) 绘制 cube
		//
		// 伪代码示意：
		// ctx.SetGraphicsPSO(m_SkyboxPSO.get());
		// ctx.SetRootSignature(m_SkyboxRootSig.get());
		// ctx.SetShaderResource(0, m_Resources.EnvironmentCube->GetSRV());
		// ctx.SetSamplerDesc(0, m_LinearSamplerDesc.get());
		// ctx.DrawIndexed(36);


	}

	void EnvironmentLightingManager::CreateEquirectangularToCubemapPipeline()
	{

	}

	void EnvironmentLightingManager::InitializeToCubemap()
	{
		m_EnvironmentCubeMap = Graphics::BufferManager::CreateColorBuffer(L"EnvironmentCubeMap RT", )
	}
}

