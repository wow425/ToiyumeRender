// ============================================================================
// 模块：ForwardMainPass（前向管线主几何绘制 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/ForwardMainPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "00_Core/Math/VectorMath.h"
#include "03_AssetSystem/Assets/Constants/ConstantBuffers.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"

// 本文件是 DefaultVS_cso / DefaultPS_cso 的唯一包含点（避免静态库 LNK2005 ODR 冲突）
#include "01_Default/DefaultVS.h"
#include "01_Default/DefaultPS.h"

namespace Renderer
{
	bool ForwardMainPass::Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, RenderServices& services)
	{
		if (m_Initialized)
			return true;

		BuildRootSignature();
		BuildDefaultPSO(colorFormat, depthFormat);

		m_CommonTextures = services.GetDescriptorHeap().AllocPersistentSingle();
		if (m_CommonTextures.IsValid())
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE src =
				Scene::LightingSystem::m_LightGPUBuffer.GetSRV().ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN
				? Scene::LightingSystem::m_LightGPUBuffer.GetSRV()
				: Graphics::GetDefaultTexture(Graphics::kBlackOpaque2D);
			Graphics::g_Device->CopyDescriptorsSimple(1, m_CommonTextures.Cpu, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		m_Initialized = true;
		return true;
	}

	void ForwardMainPass::Shutdown(RenderServices* services)
	{
		if (!m_Initialized)
			return;

		m_PSOCache.clear();
		if (services != nullptr && m_CommonTextures.IsValid())
		{
			services->GetDescriptorHeap().FreePersistentSingle(
				m_CommonTextures.HeapIndex, &services->GetReleaseQueue());
			m_CommonTextures = {};
		}
		m_RootSig.Reset(0, 0);
		m_Initialized = false;
	}

	void ForwardMainPass::BuildRootSignature()
	{
		SamplerDesc defaultSamplerDesc;
		defaultSamplerDesc.MaxAnisotropy = 8;

		m_RootSig.Reset(kNumRootBindings, 1);
		m_RootSig.InitStaticSampler(10, defaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kCommonCBV].InitAsConstantBuffer(1);

		m_RootSig.Finalize(L"ForwardMainPass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void ForwardMainPass::BuildDefaultPSO(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
	{
		using namespace Graphics;

		ASSERT(m_PSOCache.empty());

		m_DefaultPSO = GraphicsPSO(L"ForwardMainPass : Default PSO");
		m_DefaultPSO.SetRootSignature(m_RootSig);
		m_DefaultPSO.SetRasterizerState(RasterizerDefault);
		m_DefaultPSO.SetBlendState(BlendDisable);
		m_DefaultPSO.SetDepthStencilState(DepthStateReadWrite);
		m_DefaultPSO.SetInputLayout(0, nullptr);
		m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_DefaultPSO.SetRenderTargetFormats(1, &colorFormat, depthFormat);
		m_DefaultPSO.SetVertexShader(DefaultVS_cso, sizeof(DefaultVS_cso));
		m_DefaultPSO.SetPixelShader(DefaultPS_cso, sizeof(DefaultPS_cso));
	}

	uint8_t ForwardMainPass::GetPSOIndex(const PipelineDesc& desc)
	{
		using namespace Graphics;

		GraphicsPSO pso = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		const uint32_t requirements = kVertex_Position | kVertex_Normal;
		ASSERT((vertexFlags & requirements) == requirements);

		std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
		if (vertexFlags & kVertex_Position)
			vertexLayout.push_back({ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Normal)
			vertexLayout.push_back({ "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Tangent)
			vertexLayout.push_back({ "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV0)
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,      0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		else
			vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,      0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,      0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Color0)
			vertexLayout.push_back({ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		pso.SetInputLayout(static_cast<uint32_t>(vertexLayout.size()), vertexLayout.data());
		pso.SetVertexShader(DefaultVS_cso, sizeof(DefaultVS_cso));
		pso.SetPixelShader(DefaultPS_cso, sizeof(DefaultPS_cso));

		D3D12_RASTERIZER_DESC rasterizer = RasterizerDefault;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0)
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		pso.SetRasterizerState(rasterizer);

		if (desc.PassType == RenderPassType::Depth || desc.PassType == RenderPassType::Shadow)
		{
			pso.SetBlendState(BlendNoColorWrite);
			pso.SetDepthStencilState(DepthStateReadWrite);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaBlend) != 0)
		{
			pso.SetBlendState(BlendPreMultiplied);
			pso.SetDepthStencilState(DepthStateReadOnly);
		}
		else if ((desc.MaterialFlags & Scene::Material::kMaterial_AlphaTest) != 0)
		{
			D3D12_DEPTH_STENCIL_DESC maskDepth = DepthStateReadWrite;
			maskDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			pso.SetDepthStencilState(maskDepth);
		}

		pso.Finalize();

		for (uint32_t i = 0; i < m_PSOCache.size(); ++i)
		{
			if (pso.GetPipelineStateObject() == m_PSOCache[i].GetPipelineStateObject())
				return static_cast<uint8_t>(i);
		}

		m_PSOCache.push_back(pso);
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique Forward PSOs");
		return static_cast<uint8_t>(m_PSOCache.size() - 1);
	}

	const GraphicsPSO& ForwardMainPass::GetOrCreatePSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	void ForwardMainPass::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const
	{
		const auto& heap = services.GetDescriptorHeap();
		context.SetDescriptorTable(kMaterialSRVs, heap.GetCbvSrvUavHandle(material.SRVTable).Gpu);
		context.SetDescriptorTable(kMaterialSamplers, heap.GetSamplerHandle(material.SamplerTable).Gpu);
	}

	void ForwardMainPass::Execute(GraphicsContext& context,
		const RenderFrameDesc& frame,
		RasterDrawHelper& drawHelper,
		const FrameBlackboard& blackboard,
		DrawPass maxPass,
		BatchType batchType,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		RenderServices& services)
	{
		if (!m_Initialized || frame.Camera == nullptr)
			return;

		DepthBuffer* depthBuffer = blackboard.Core.SceneDepth;
		ColorBuffer* sceneColor  = blackboard.Core.LinearHDRColor != nullptr
			? blackboard.Core.LinearHDRColor
			: blackboard.Core.SceneColorLDR;
		if (depthBuffer == nullptr || sceneColor == nullptr)
			return;

		context.TransitionResource(*depthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.TransitionResource(*sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
		context.ClearDepth(*depthBuffer);
		context.ClearColor(*sceneColor);
		context.SetViewportAndScissor(viewport, scissor);

		// [契约 1] 一次性绑定 4 分区单可见堆，整帧 SetDescriptorHeaps <= 1
		context.SetRootSignature(m_RootSig);
		services.GetDescriptorHeap().BindToContext(context);
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		context.SetDescriptorTable(kCommonSRVs, m_CommonTextures.Gpu);

		GlobalConstants globals;
		globals.ViewProjMatrix = frame.Camera->GetViewProjMatrix();
		globals.CameraPos.SetXYZ(frame.Camera->GetPosition());
		globals.SunDirection = Math::Vector4(Math::kZero);
		globals.SunIntensity = Math::Vector4(Math::kZero);
		context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);

		context.PIXBeginEvent(L"ForwardMainPass");

		const MeshSorter& sorter = drawHelper.GetSorter();
		DrawPass currentPass = const_cast<MeshSorter&>(sorter).GetCurrentPass();
		const uint32_t* passCounts = sorter.GetPassCounts();
		uint32_t currentDraw = const_cast<MeshSorter&>(sorter).GetCurrentDraw();
		const std::vector<uint64_t> sortKeys = const_cast<MeshSorter&>(sorter).GetSortkeys();
		const std::vector<MeshSorter::SortObject> sortObjects = const_cast<MeshSorter&>(sorter).GetSortObjects();

		for (; currentPass <= maxPass; currentPass = static_cast<DrawPass>(currentPass + 1))
		{
			const uint32_t passCount = passCounts[currentPass];
			if (passCount == 0)
				continue;

			if (batchType == kDefault)
			{
				switch (currentPass)
				{
				case kZPass:
					context.TransitionResource(*depthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.SetDepthStencilTarget(depthBuffer->GetDSV());
					break;
				case kOpaque:
					context.TransitionResource(*depthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					context.TransitionResource(*sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(sceneColor->GetRTV(), depthBuffer->GetDSV());
					break;
				case kTransparent:
					context.TransitionResource(*depthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
					context.TransitionResource(*sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.SetRenderTarget(sceneColor->GetRTV(), depthBuffer->GetDSV_DepthReadOnly());
					break;
				default:
					break;
				}
			}

			context.SetViewportAndScissor(viewport, scissor);
			context.FlushResourceBarriers();

			const uint32_t lastDraw = currentDraw + passCount;
			while (currentDraw < lastDraw)
			{
				MeshSorter::SortKey key;
				key.value = sortKeys[currentDraw];
				const MeshSorter::SortObject& object = sortObjects[key.objectIdx];
				const Scene::Model::Mesh& mesh = *object.mesh;
				const Scene::Material::Material& material = *object.material;
				const PipelineDesc& desc = PipelineStateCache::GetPipelineDesc(static_cast<uint16_t>(key.psoIdx));

				context.SetConstantBuffer(kMeshConstants, object.meshCBV);
				context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
				BindMaterial(context, material, services);
				context.SetPipelineState(GetOrCreatePSO(desc));

				if (currentPass == kZPass)
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbDepthOffset, mesh.vbDepthSize, mesh.vbDepthStride });
				else
					context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });

				context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, static_cast<DXGI_FORMAT>(mesh.ibFormat) });

				for (uint32_t i = 0; i < mesh.numDraws; ++i)
				{
					context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
				}
				++currentDraw;
			}
		}

		context.PIXEndEvent();
	}
}
