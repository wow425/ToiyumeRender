
#include "00_Core/PCH.h"
#include "DeferredRenderer.h"
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


#include "StaticMeshVS.h"
#include "GBufferPS.h"

#pragma warning(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer::Deferred
{
	RendererAutoRegister<DeferredRenderer> s_RegisterForwardRenderer(L"DeferredRenderer");
}


namespace Renderer::Deferred
{
	bool DeferredRenderer::Initialize(const RendererCreateDesc& desc)
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

			m_CreateDesc = desc;
		}



		BuildRootSignature();
		BuildPSOs(); // 停留于此
		BuildDescriptorHeaps();
		TextureManager::Initialize(L"");
		CreateDeferredBufferTargets();
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

	void DeferredRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		DestroyDeferredBufferTargets();

		m_PSOCache.clear();
		s_TextureHeap.Destroy();
		s_SamplerHeap.Destroy();

		TextureManager::Shutdown();

		m_Initialized = false;
	}

	void DeferredRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width = width;
		m_CreateDesc.height = height;

		if (!m_Initialized) return;

		DestroyDeferredBufferTargets();
		CreateDeferredBufferTargets();
	}

	void DeferredRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;


	}

	void DeferredRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		// 模型
		{
			for (auto* model : frame.Models)
			{
				if (model->IsDirty())
				{
					model->Update(gfxContext, frame.delatT);
					this->ModelSort(*model);
				}
			}
		}

		// 这里放每帧更新：
		// 1. 相机常量 m_CameraController管理分离
		// 2. 灯光常量
		// 3. 材质/对象常量
		// 4. 历史帧信息（TAA / Motion Vector）


	}

	void DeferredRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass,
		BatchType batchType)
	{


		// Buffer重置
		{
			for (auto& GBuffer : m_DeferredBuffer->GBuffers)
			{
				context.TransitionResource(*GBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
			context.TransitionResource(*m_DeferredBuffer->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			context.TransitionResource(*m_DeferredBuffer->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
			context.ClearDepth(*m_DeferredBuffer->SceneDepthBuffer);
			context.ClearColor(*m_DeferredBuffer->SceneColorBuffer);

			context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor()); // 设置视口和裁剪矩形
		}
		// 全局绑定
		{
			this->UpdateGlobalDescriptors();
			this->BindRenderState(context);

			// Set common textures
			context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);
			// Set common shader constants
			GlobalConstants globals;
			globals.ViewProjMatrix = frame.Camera->GetViewProjMatrix();
			globals.CameraPos = frame.Camera->GetPosition();
			context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);
		}

		context.PIXBeginEvent(L"DeferredRenderer");
		// 1.ShadowPass
		{
			context.PIXSetEvent(L"ShadowPass");
		}
		// 2. GBufferPass
		{
			context.PIXSetEvent(L"GBufferPass");

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
		// 3. SSAOPass
		{
			context.PIXSetEvent(L"SSAOPass");

		}
		// 4. DeferredLightingPass
		{
			context.PIXSetEvent(L"DeferredLightingPass");
		}
		// 这里是渲染调度入口。
		// 你后续把具体 Pass 拆出来后，按这个顺序接：
		// 2. GBufferPass
		// 3. SSAOPass
		// 4. DeferredLightingPass
		// 5. TransparentForwardPass
		// 6. SkyPass
		// 7. Tonemap / Bloom / TAA
		//
	}

	void DeferredRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		(void)frame;
		DefaultSorter.Reset();
		// 模型
		{
			for (auto& model : frame.Models)
			{
				model->ClearDirty();
			}
		}
	}

	void DeferredRenderer::BuildDescriptorHeaps()
	{
		s_TextureHeap.Create(L"Forward Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		s_SamplerHeap.Create(L"Forward Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = s_TextureHeap.Alloc(1);
	}

	void DeferredRenderer::BuildRootSignature()
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
		m_RootSig.Finalize(L"DeferredRenderer RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void DeferredRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT depthFormat = SceneDepthBufferFormat;

		GraphicsPSO GBufferPSO(L"DeferredRenderer : GBuffer  PSO");
		GBufferPSO.SetRootSignature(m_RootSig);                                           // 根签名
		GBufferPSO.SetRasterizerState(RasterizerDefault);                                 // 光栅状态
		GBufferPSO.SetBlendState(BlendDisable);                                           // 混合模式     默认关闭
		GBufferPSO.SetDepthStencilState(DepthStateReadWrite);                              // 深度模板状态 DepthStateDisabled  DepthStateReadWrite
		GBufferPSO.SetInputLayout(0, nullptr);                                            // 输入布局
		GBufferPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
		GBufferPSO.SetRenderTargetFormats((uint32_t)GBufferSlot::GBuffer_Count, GBufferFormats, depthFormat);                 // GBuffer MRT
		GBufferPSO.SetVertexShader(StaticMeshVS_cso, sizeof(StaticMeshVS_cso));
		GBufferPSO.SetPixelShader(GBufferPS_cso, sizeof(GBufferPS_cso));
		m_DefaultPSO = GBufferPSO;
		//GBufferPSO.Finalize();

		//m_PSOCache.push_back(GBufferPSO);
	}

	// 现代renderer是自己持有资源，buffermanager负责管理资源
	void DeferredRenderer::CreateDeferredBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0) return;

		const uint32_t width = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_DeferredBuffer = std::make_shared<DeferredBuffer>();

		m_DeferredBuffer->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor] =
			BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_BaseColor]);

		m_DeferredBuffer->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal] =
			BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Normal]);

		m_DeferredBuffer->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material] =
			BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Material]);

		m_DeferredBuffer->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission] =
			BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Emission]);


		m_DeferredBuffer->SceneColorBuffer = BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, SceneColorBufferFormat);
		m_DeferredBuffer->SceneDepthBuffer = BufferManager::CreateDepthBuffer(L"Deferred SceneColorBuffer", width, height, SceneDepthBufferFormat);
		m_DeferredBuffer->VelocityBuffer = BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, VelocityBufferFormat);


	}

	void DeferredRenderer::DestroyDeferredBufferTargets()
	{
		BufferManager::DestroyAll();
	}

	void DeferredRenderer::UpdateGlobalDescriptors()
	{
		// 这里以后放：
		// - 阴影图 SRV
		// - SSAO 结果 SRV
		// - IBL 贴图 SRV
		// - 环境贴图 SRV
		// - 历史帧颜色 SRV
	}

	const GraphicsPSO& DeferredRenderer::GetPSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	uint8_t DeferredRenderer::GetPSOIndex(const PipelineDesc& desc)
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
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());

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

	// 根签名，资源堆，sampler堆,图元
	void DeferredRenderer::BindRenderState(GraphicsContext& context)
	{
		context.SetRootSignature(m_RootSig);
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	void DeferredRenderer::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material)
	{
		context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[material.SRVTable]);
		context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[material.SamplerTable]);
	}



} // namespace Renderer::Deferred
