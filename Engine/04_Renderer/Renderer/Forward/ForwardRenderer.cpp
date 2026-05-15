
#include "00_Core/PCH.h"
#include "ForwardRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "03_AssetSystem/Assets/Constants//ConstantBuffers.h"
#include "04_Renderer/BufferManager.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "04_Renderer/Features/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
#include "06_Content/Shaders/01_Default/DefaultVS.h"
#include "06_Content/Shaders/01_Default/DefaultPS.h"

#pragma warning(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer
{
	RendererAutoRegister<ForwardRenderer> s_RegisterForwardRenderer(L"ForwardRenderer");
}


namespace Renderer
{
	bool ForwardRenderer::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)	return true;


		m_CreateDesc = desc;

		BuildRootSignature();
		BuildPSOs();
		BuildDescriptorHeaps();
		TextureManager::Initialize(L"");
		CreateForwardBufferTargets();
		LightingSystem::InitializeResources();
		LightingSystem::CreateLights();

		uint32_t DestCount = 1;
		uint32_t SourceCounts[] = { 1 };

		D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
		{
			LightingSystem::m_LightGPUBuffer.GetSRV(),
		};

		m_Initialized = true;
		return true;
	}

	void ForwardRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		DestroyForwardBufferTargets();

		m_PSOCache.clear();
		m_TextureHeap.Destroy();
		m_SamplerHeap.Destroy();

		TextureManager::Shutdown();

		m_Initialized = false;
	}

	void ForwardRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width = width;
		m_CreateDesc.height = height;

		if (!m_Initialized) return;

		DestroyForwardBufferTargets();
		CreateForwardBufferTargets();
	}

	void ForwardRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;
	}

	void ForwardRenderer::Update(const RenderFrameDesc& frame)
	{
		(void)frame;

		// 这里放每帧更新：
		// 1. 相机常量
		// 2. 灯光常量
		// 3. 材质/对象常量
		// 4. 历史帧信息（TAA / Motion Vector）
	}

	void ForwardRenderer::Render(MeshSorter::DrawPass pass, GraphicsContext& context, GlobalConstants& globals, const RenderFrameDesc& frame)
	{
		ASSERT(m_Initialized);
		(void)frame;
		context.PIXSetEvent(L"ForwardRenderer: Begin");

		// 这里是渲染调度入口。
		// 你后续把具体 Pass 拆出来后，按这个顺序接：
		// 1. ShadowPass
		// 2. GBufferPass
		// 3. SSAOPass
		// 4. DeferredLightingPass
		// 5. TransparentForwardPass
		// 6. SkyPass
		// 7. Tonemap / Bloom / TAA
		//
		// 现在先保留为调度骨架，不把具体功能塞回 Engine。


		context.PIXSetEvent(L"ForwardRenderer: End");
	}

	void ForwardRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		(void)frame;
	}

	void ForwardRenderer::BuildDescriptorHeaps()
	{
		m_TextureHeap.Create(L"Deferred Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		m_SamplerHeap.Create(L"Deferred Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = m_TextureHeap.Alloc(1);
	}

	void ForwardRenderer::BuildRootSignature()
	{
		using namespace Renderer;

		m_RootSig.Reset(kNumRootBindings, 1);

		SamplerDesc DefaultSamplerDesc;
		DefaultSamplerDesc.MaxAnisotropy = 8;

		// 根签名设置并finalize
		m_RootSig.Reset(kNumRootBindings, 1); // 初始化分配根参数内存
		m_RootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 静态采样器
		//m_RootSig.InitStaticSampler(11, SamplerShadowDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		//m_RootSig.InitStaticSampler(12, CubeMapSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量        根描述符绑定
		m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量     根描述符绑定  
		m_RootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
		m_RootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
		m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
		m_RootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV
		m_RootSig.Finalize(L"ForwardRenderer RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void ForwardRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT colorFormat = m_CreateDesc.backBufferFormat;
		DXGI_FORMAT depthFormat = m_CreateDesc.depthBufferFormat;

		GraphicsPSO defaultPSO(L"ForwardRenderer: Default PSO");
		defaultPSO.SetRootSignature(m_RootSig);                                           // 根签名
		defaultPSO.SetRasterizerState(RasterizerDefault);                                 // 光栅状态
		defaultPSO.SetBlendState(BlendDisable);                                           // 混合模式     默认关闭
		defaultPSO.SetDepthStencilState(DepthStateReadWrite);                              // 深度模板状态 DepthStateDisabled  DepthStateReadWrite
		defaultPSO.SetInputLayout(0, nullptr);                                            // 输入布局
		defaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
		defaultPSO.SetRenderTargetFormats(1, &colorFormat, depthFormat);                  // RTV绑定
		defaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));                   // VS绑定
		defaultPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));                    // PS绑定
		m_DefaultPSO = defaultPSO;
		// defaultPSO.Finalize();

		// m_PSOCache.push_back(defaultPSO);
	}

	// 现代renderer是自己持有资源，buffermanager负责管理资源
	void ForwardRenderer::CreateForwardBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0) return;

		const uint32_t width = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_ForwardBuffer->SceneColorBuffer = BufferManager::CreateColorBuffer(L"Scene Color Buffer", width, height, m_CreateDesc.backBufferFormat);
		m_ForwardBuffer->DepthBuffer = BufferManager::CreateDepthBuffer(L"Scene Depth Buffer", width, height, m_CreateDesc.depthBufferFormat);


	}

	void ForwardRenderer::DestroyForwardBufferTargets()
	{
		m_ForwardBuffer->SceneColorBuffer->Destroy();
		m_ForwardBuffer->DepthBuffer->Destroy();
	}

	void ForwardRenderer::UpdateGlobalDescriptors()
	{
		// 这里以后放：
		// - 阴影图 SRV
		// - SSAO 结果 SRV
		// - IBL 贴图 SRV
		// - 环境贴图 SRV
		// - 历史帧颜色 SRV
	}

	uint8_t ForwardRenderer::GetPSO(uint16_t psoFlags)
	{
		using namespace PSOFlags;

		GraphicsPSO ColorPSO = m_DefaultPSO;

		uint16_t Requirements = kHasPosition | kHasNormal;
		ASSERT((psoFlags & Requirements) == Requirements);

		std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
		if (psoFlags & kHasPosition)
			vertexLayout.push_back({ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (psoFlags & kHasNormal)
			vertexLayout.push_back({ "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (psoFlags & kHasTangent)
			vertexLayout.push_back({ "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (psoFlags & kHasUV0)
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		else
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (psoFlags & kHasUV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());

		// TODO:阉割掉动态选择shader，以后添加功能时再加回来
		// TODO: 预编译shader带宏定义脚本
		ColorPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
		ColorPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));




		if (psoFlags & kAlphaBlend)
		{
			ColorPSO.SetBlendState(BlendPreMultiplied);
			ColorPSO.SetDepthStencilState(DepthStateReadOnly);
		}
		ColorPSO.Finalize(); // Mesh PSO绑定处

		// Look for an existing PSO
		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (ColorPSO.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
			{
				return (uint8_t)i;
			}
		}

		// If not found, keep the new one, and return its index
		m_PSOCache.push_back(ColorPSO);

		// Z-prepass阉割掉，延迟渲染时加回来

		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique PSOs");

		return (uint8_t)m_PSOCache.size() - 1;
	}
}
