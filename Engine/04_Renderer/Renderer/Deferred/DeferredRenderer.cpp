
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
#include "FullScreenVS.h"
#include "LightingPS.h"

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
		// GBuffer Pass
		{
			BuildRootSignature();
			BuildPSOs(); // 停留于此
			BuildDescriptorHeaps();
			TextureManager::Initialize(L"");
			CreateDeferredBufferTargets();
		}
		// Lighting Pass
		{
			BuildLightingSignature();
			BuildLightingPSO();

			Scene::LightingSystem::InitializeResources();
			Scene::LightingSystem::CreateLights();
		}

		uint32_t DestCount = 1;
		uint32_t SourceCounts[] = { 1 };

		D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[] =
		{
			Scene::LightingSystem::m_LightGPUBuffer.GetSRV(),
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



	void DeferredRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		/*
			Depth PrePass
			↓
			Geometry Pass(GBuffer)
			↓
			SSAO / GTAO
			↓
			Shadow Pass
			↓
			Deferred Lighting
			↓
			IBL / Sky
			↓
			Transparent Forward Pass
			↓
			PostProcess
Bloom
DOF
Fog
SSR
ColorGrading
Chromatic
Vignette
			↓
			TAA
			↓
			Tonemap
			↓
			Present
		*
		*
		*/


		// 全局绑定
		{
			// this->UpdateGlobalDescriptors();
			// Set common textures
			// context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);
			// Set common shader constants
		}



		context.PIXBeginEvent(L"DeferredRenderer");

		// 1. GBufferPass
		{
			this->BindRenderState(context);

			// Buffer处理
			{
				for (auto& GBuffer : m_GBuffers->GBuffers)
				{
					context.TransitionResource(*GBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.ClearColor(*GBuffer);
				}
				context.TransitionResource(*m_GBuffers->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				context.TransitionResource(*m_GBuffers->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.ClearColor(*m_GBuffers->SceneColorBuffer);
				context.ClearDepth(*m_GBuffers->SceneDepthBuffer);

				context.SetViewportAndScissor(this->GetMainViewport(), this->GetMainScissor()); // 设置视口和裁剪矩形
				context.SetRenderTargets((uint32_t)GBufferSlot::GBuffer_Count, m_CPUGBuffers, m_GBuffers->SceneDepthBuffer->GetDSV());

				context.FlushResourceBarriers();
			}

			GlobalConstants globals;
			globals.ViewProjMatrix = frame.Camera->GetViewProjMatrix();
			globals.CameraPos = frame.Camera->GetPosition();
			context.SetDynamicConstantBufferView(kCommonCBV, sizeof(globals), &globals);

			context.PIXBeginEvent(L"GBufferPass");

			auto CurrentPass = this->DefaultSorter.GetCurrentPass();
			const uint32_t* PassCounts = this->DefaultSorter.GetPassCounts();
			auto CurrentDraw = this->DefaultSorter.GetCurrentDraw();
			auto Sortkeys = this->DefaultSorter.GetSortkeys();
			auto SortObjects = this->DefaultSorter.GetSortObjects();
			for (; CurrentPass <= pass; CurrentPass = (DrawPass)(CurrentPass + 1))
			{
				const uint32_t passCount = PassCounts[CurrentPass];
				if (passCount == 0) continue;

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
						this->BindMaterial(context, material);

						context.SetPipelineState(this->GetPSO(desc)); // 绑定PSO！！！
					}

					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
					context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, (DXGI_FORMAT)mesh.ibFormat });

					for (uint32_t i = 0; i < mesh.numDraws; ++i)
					{
						context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
					}
					++CurrentDraw;
				}
			}
			context.PIXEndEvent();
			context.FlushResourceBarriers();
		} // 2. GBufferPass

		// 3. SSAOPass
		{
			context.PIXBeginEvent(L"SSAOPass");
			context.PIXEndEvent();

		} // 3. SSAOPass

		// 4.ShadowPass
		{
			context.PIXBeginEvent(L"ShadowPass");
			context.PIXEndEvent();
		} // 4.ShadowPass

		// 5. LightingPass
		{
			context.PIXBeginEvent(L"DeferredLightingPass");

			context.SetRootSignature(m_LightingRootSig);
			context.ClearColor(*m_GBuffers->SceneColorBuffer);

			// 1. 资源处理
			{
				for (auto& GBuffer : m_GBuffers->GBuffers)
				{
					context.TransitionResource(*GBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

				}
				// Depth
				context.TransitionResource(*m_GBuffers->SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				// Lighting Output
				context.TransitionResource(*m_GBuffers->SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);

				context.FlushResourceBarriers();

				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_BaseColor, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Normal, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Material, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::GBuffer_Emission, 0, m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission]->GetSRV());
				context.SetDynamicDescriptor((uint32_t)LightingSlot::Depth, 0, m_GBuffers->SceneDepthBuffer->GetDepthSRV());
				context.SetConstantBuffer((uint32_t)LightingSlot::Light, Scene::LightingSystem::m_LightGPUBuffer.GetGpuVirtualAddress());
				Scene::Camera::CameraData CameraCB;
				CameraCB.CameraPos.SetXYZ(frame.Camera->GetPosition());
				CameraCB.InvViewProj = frame.Camera->GetInViewProjMatrix();
				context.SetDynamicConstantBufferView((uint32_t)LightingSlot::Camera, sizeof(CameraCB), &CameraCB);
				context.SetRenderTarget(m_GBuffers->SceneColorBuffer->GetRTV());

			}

			// 3. 绘制全屏几何体
			context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context.SetPipelineState(m_LightingPSO);
			context.Draw(3, 0);

			context.PIXEndEvent();
		} // 5. DeferredLightingPass

		// 6. IBL / Sky Pass
		{
			context.PIXBeginEvent(L"IBLPass");
			context.PIXEndEvent();
		}  // 6. IBL / Sky Pass

		// 7. Transparent Forward Pass 
		{
			context.PIXBeginEvent(L"Transparent Forward Pass ");
			context.PIXEndEvent();
		} // 7. Transparent Forward Pass

		// 8. PostProcess Pass 
		{
			context.PIXBeginEvent(L"PostProcess Pass ");
			context.PIXEndEvent();
		} // 8. PostProcess Pass 

		// 9. TAA Pass 
		{
			context.PIXBeginEvent(L"TAA Pass");
			context.PIXEndEvent();
		} // 9. TAA Pass

		// 10. Tonemap Pass 
		{
			context.PIXBeginEvent(L"Tonemap Pass");
			context.PIXEndEvent();
		} // 10. Tonemap Pass
		context.PIXEndEvent();
	}

	void DeferredRenderer::EndFrame(::GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		(void)frame;
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
		s_TextureHeap.Create(L"Deferred Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
		s_SamplerHeap.Create(L"Deferred Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

		// 预留一块“全局通用贴图”描述符区域
		m_CommonTextures = s_TextureHeap.Alloc(1);
	}

	void DeferredRenderer::BuildRootSignature()
	{
		using namespace Renderer;

		SamplerDesc DefaultSamplerDesc;
		DefaultSamplerDesc.MaxAnisotropy = 8;

		SamplerDesc PointSamplerDesc; // Lighting Pass是逐像素的
		PointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		PointSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		// 根签名设置并finalize
		m_GBufferRootSig.Reset(kNumRootBindings, 1); // 初始化分配根参数内存
		m_GBufferRootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 静态采样器
		//m_RootSig.InitStaticSampler(11, SamplerShadowDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		//m_RootSig.InitStaticSampler(12, CubeMapSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_GBufferRootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量        
		m_GBufferRootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量     
		m_GBufferRootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
		m_GBufferRootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
		m_GBufferRootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
		m_GBufferRootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV
		m_GBufferRootSig.Finalize(L"DeferredRenderer GBuffer Pass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void DeferredRenderer::BuildPSOs()
	{
		// “基础 PSO 模板”
		ASSERT(m_PSOCache.empty());

		DXGI_FORMAT depthFormat = SceneDepthBufferFormat;

		GraphicsPSO GBufferPSO(L"DeferredRenderer : GBuffer PSO");
		GBufferPSO.SetRootSignature(m_GBufferRootSig);                                           // 根签名
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

		m_GBuffers = std::make_shared<DeferredBuffer>();
		int i = 0;

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor] =
			BufferManager::CreateColorBuffer(L"GBuffer_BaseColor", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_BaseColor]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal] =
			BufferManager::CreateColorBuffer(L"GBuffer_Normal", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Normal]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material] =
			BufferManager::CreateColorBuffer(L"GBuffer_Material", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Material]);

		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission] =
			BufferManager::CreateColorBuffer(L"GBuffer_Emission", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Emission]);

		for (auto& buffer : m_GBuffers->GBuffers)
		{
			m_CPUGBuffers[i++] = buffer->GetRTV();
		}

		m_GBuffers->SceneColorBuffer = BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, SceneColorBufferFormat);
		m_GBuffers->SceneDepthBuffer = BufferManager::CreateDepthBuffer(L"Deferred SceneDepthBuffer", width, height, SceneDepthBufferFormat);
		m_GBuffers->VelocityBuffer = BufferManager::CreateColorBuffer(L"Deferred VelocityBuffer", width, height, VelocityBufferFormat);


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
		GraphicsPSO GBufferPSO = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		uint32_t Requirements = kVertex_Position | kVertex_Normal | kVertex_Tangent;
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

		GBufferPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());

		D3D12_RASTERIZER_DESC rasterizer = RasterizerDefault;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0)
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		GBufferPSO.SetRasterizerState(rasterizer);

		if (desc.PassType == RenderPassType::Depth || desc.PassType == RenderPassType::Shadow)
		{
			GBufferPSO.SetBlendState(BlendNoColorWrite);
			GBufferPSO.SetDepthStencilState(DepthStateReadWrite);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaBlend) != 0)
		{
			GBufferPSO.SetBlendState(BlendPreMultiplied);
			GBufferPSO.SetDepthStencilState(DepthStateReadOnly);
		}

		GBufferPSO.Finalize();

		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (GBufferPSO.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
				return (uint8_t)i;
		}

		m_PSOCache.push_back(GBufferPSO);
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique PSOs");
		return (uint8_t)m_PSOCache.size() - 1;
	}

	// 根签名，资源堆，sampler堆,图元
	void DeferredRenderer::BindRenderState(GraphicsContext& context)
	{
		context.SetRootSignature(m_GBufferRootSig);
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	void DeferredRenderer::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material)
	{
		context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
		context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[material.SRVTable]);
		context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[material.SamplerTable]);
	}


	void DeferredRenderer::BuildLightingPSO()
	{
		m_LightingPSO = m_DefaultPSO;



		m_LightingPSO.SetName(L"DeferredRenderer : Lighting PSO");
		m_LightingPSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
		m_LightingPSO.SetPixelShader(LightingPS_cso, sizeof(LightingPS_cso));
		m_LightingPSO.SetRenderTargetFormat(SceneColorBufferFormat, SceneDepthBufferFormat);
		m_LightingPSO.SetDepthStencilState(Graphics::DepthStateDisabled);
		m_LightingPSO.SetRasterizerState(RasterizerFullScreen);
		m_LightingPSO.SetRootSignature(m_LightingRootSig);



		m_LightingPSO.Finalize();
	}

	void DeferredRenderer::BuildLightingSignature()
	{
		// GBuffer作为SRV，光照跟camera作为常量
		using namespace Renderer;

		SamplerDesc PointSamplerDesc; // Lighting Pass是逐像素的
		PointSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		PointSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		PointSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		// 根签名设置并finalize
		m_LightingRootSig.Reset((uint32_t)LightingSlot::Count, 0);
		// 直接Texture.Load()，不用sampler
		// m_LightingRootSig.InitStaticSampler(0, PointSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL); // 点采样器s0
		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_BaseColor].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_BaseColor, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t0

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Normal].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Normal, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t1

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Material].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Material, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t2

		m_LightingRootSig[(uint32_t)LightingSlot::GBuffer_Emission].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::GBuffer_Emission, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t3

		m_LightingRootSig[(uint32_t)LightingSlot::Depth].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (uint32_t)LightingSlot::Depth, 1, D3D12_SHADER_VISIBILITY_PIXEL); // t4

		m_LightingRootSig[(uint32_t)LightingSlot::Light].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // b0
		m_LightingRootSig[(uint32_t)LightingSlot::Camera].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_PIXEL); // b1

		m_LightingRootSig.Finalize(L"DeferredRenderer Lighting Pass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}
} // namespace Renderer::Deferred
