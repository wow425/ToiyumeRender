
#include "00_Core/PCH.h"
#include "ForwardRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "04_Renderer/Material/Material.h"
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "03_AssetSystem/Assets/Constants//ConstantBuffers.h"
#include "04_Renderer/BufferManager.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "04_Renderer/Features/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
#include "Shader/01_Default/DefaultVS.h"
#include "Shader/01_Default/DefaultPS.h"

#pragma warning(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer::Forward
{
	RendererAutoRegister<ForwardRenderer> s_RegisterForwardRenderer(L"ForwardRenderer ");
}



namespace Renderer::Forward
{
	using Config = Renderer::Forward::Config;

	bool ForwardRenderer::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)	return true;
		//  视口，裁剪矩阵初始化
		{
			m_MainViewport.TopLeftX = 0.0f; // taa用
			m_MainViewport.TopLeftY = 0.0f;

			m_MainViewport.Width = (float)desc.width;
			m_MainViewport.Height = (float)desc.height;
			m_MainViewport.MinDepth = 0.0f;
			m_MainViewport.MaxDepth = 1.0f;

			m_MainScissor.left = 0;
			m_MainScissor.top = 0;
			m_MainScissor.right = (LONG)desc.width;
			m_MainScissor.bottom = (LONG)desc.height;
		}
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
		s_TextureHeap.Destroy();
		s_SamplerHeap.Destroy();

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

	void ForwardRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		(void)frame;

		// 这里放每帧更新：
		// 1. 相机常量 m_CameraController管理分离
		// 2. 灯光常量
		// 3. 材质/对象常量
		// 4. 历史帧信息（TAA / Motion Vector）
	}

	void ForwardRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass,
		BatchType batchType)
	{
		ASSERT(m_Initialized);
		auto DepthBuffer = this->GetForwardBuffer().DepthBuffer;
		auto SceneColorBuffer = this->GetForwardBuffer().SceneColorBuffer[0];
		auto Camera = frame.Camera;
		ASSERT(DepthBuffer != nullptr);
		ASSERT(SceneColorBuffer != nullptr);
		ASSERT(Camera != nullptr);


		// 前置准备
		{
			context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
			context.ClearDepth(*DepthBuffer);
			context.ClearColor(*SceneColorBuffer);

			context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor()); // 设置视口和裁剪矩形
		}
		// 全局绑定
		{
			this->UpdateGlobalDescriptors();
			this->BindRenderState(context);
			context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Set common textures
			context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);
			// Set common shader constants
			GlobalConstants globals;
			globals.ViewProjMatrix = Camera->GetViewProjMatrix();
			globals.CameraPos = Camera->GetPosition();
			context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);
		}

		context.PIXBeginEvent(L"ForwardRenderer ");
		// 1.ShadowPass
		{
			context.PIXSetEvent(L"ShadowPass");
		}
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
		auto CurrentPass = this->DefaultSorter.GetCurrentPass();
		const uint32_t* PassCounts = this->DefaultSorter.GetPassCounts();
		auto CurrentDraw = this->DefaultSorter.GetCurrentDraw();
		auto Sortkeys = this->DefaultSorter.GetSortkeys();
		auto SortObjects = this->DefaultSorter.GetSortObjects();
		for (; CurrentPass <= pass; CurrentPass = (DrawPass)(CurrentPass + 1))
		{
			const uint32_t passCount = PassCounts[CurrentPass];
			if (passCount == 0)
				continue;

			if (batchType == kDefault)
			{
				switch (CurrentPass)
				{
				case kZPass:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.SetDepthStencilTarget(DepthBuffer->GetDSV());
					break;
				case kOpaque:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(SceneColorBuffer->GetRTV(), DepthBuffer->GetDSV());
					break;
				case kTransparent:
					context.TransitionResource(*DepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
					context.TransitionResource(*SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(SceneColorBuffer->GetRTV(), DepthBuffer->GetDSV_DepthReadOnly());
					break;
				}
			}

			context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor());
			context.FlushResourceBarriers();

			const uint32_t lastDraw = CurrentDraw + passCount;

			while (CurrentDraw < lastDraw)
			{
				Renderer::MeshSorter::SortKey key;
				key.value = Sortkeys[CurrentDraw];
				const Renderer::MeshSorter::SortObject& object = SortObjects[key.objectIdx];
				const Scene::Model::Mesh& mesh = *object.mesh;
				const Scene::Material::Material& material = *object.material;
				const PipelineDesc& desc = Renderer::PipelineStateCache::GetPipelineDesc(static_cast<uint16_t>(key.psoIdx));
				// 根实参绑定和PSO绑定
				{
					context.SetConstantBuffer(kMeshConstants, object.meshCBV);
					context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
					this->BindMaterial(context, material);

					context.SetPipelineState(this->GetPSO(desc)); // !?
				}


				if (CurrentPass == kZPass)
				{
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbDepthOffset, mesh.vbDepthSize, mesh.vbDepthStride });
				}
				else
				{
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
				}
				context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, (DXGI_FORMAT)mesh.ibFormat });

				for (uint32_t i = 0; i < mesh.numDraws; ++i)
				{
					context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
				}
				++CurrentDraw;
			}
		}
	}

	void ForwardRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		(void)frame;
		DefaultSorter.Reset();
	}

	void ForwardRenderer::BuildDescriptorHeaps()
	{
		s_TextureHeap.Create(L"Forward Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		s_SamplerHeap.Create(L"Forward Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = s_TextureHeap.Alloc(1);
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
		m_RootSig.Finalize(L"ForwardRenderer  RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void ForwardRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT colorFormat = Config::backBufferFormat;
		DXGI_FORMAT depthFormat = Config::depthBufferFormat;

		GraphicsPSO defaultPSO(L"ForwardRenderer : Default PSO");
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
		//defaultPSO.Finalize();

		//m_PSOCache.push_back(defaultPSO);
	}

	// 现代renderer是自己持有资源，buffermanager负责管理资源
	void ForwardRenderer::CreateForwardBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0) return;

		const uint32_t width = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_ForwardBuffer = std::make_shared<ForwardBuffer>();
		m_ForwardBuffer->SceneColorBuffer[0] = BufferManager::CreateColorBuffer(L"Scene Color Buffer", width, height, Config::backBufferFormat);
		m_ForwardBuffer->DepthBuffer = BufferManager::CreateDepthBuffer(L"Scene Depth Buffer", width, height, Config::depthBufferFormat);


	}

	void ForwardRenderer::DestroyForwardBufferTargets()
	{
		m_ForwardBuffer->SceneColorBuffer[0]->Destroy();
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

	const GraphicsPSO& ForwardRenderer::GetPSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	uint8_t ForwardRenderer::GetPSOIndex(const PipelineDesc& desc)
	{
		GraphicsPSO ColorPSO = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		uint32_t Requirements = kVertex_Position | kVertex_Normal;
		ASSERT((vertexFlags & Requirements) == Requirements);

		std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
		if (vertexFlags & kVertex_Position)
			vertexLayout.push_back({ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Normal)
			vertexLayout.push_back({ "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Tangent)
			vertexLayout.push_back({ "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV0)
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		else
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());
		ColorPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
		ColorPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));

		D3D12_RASTERIZER_DESC rasterizer = RasterizerDefault;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0)
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		ColorPSO.SetRasterizerState(rasterizer);

		if (desc.PassType == RenderPassType::Depth || desc.PassType == RenderPassType::Shadow)
		{
			ColorPSO.SetBlendState(BlendNoColorWrite);
			ColorPSO.SetDepthStencilState(DepthStateReadWrite);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaBlend) != 0)
		{
			ColorPSO.SetBlendState(BlendPreMultiplied);
			ColorPSO.SetDepthStencilState(DepthStateReadOnly);
		}

		ColorPSO.Finalize();

		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (ColorPSO.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
				return (uint8_t)i;
		}

		m_PSOCache.push_back(ColorPSO);
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique PSOs");
		return (uint8_t)m_PSOCache.size() - 1;
	}


	void ForwardRenderer::BindRenderState(GraphicsContext& context)
	{
		context.SetRootSignature(m_RootSig);
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());
	}

	void ForwardRenderer::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material)
	{
		context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[material.SRVTable]);
		context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[material.SamplerTable]);
	}



} // namespace Renderer
