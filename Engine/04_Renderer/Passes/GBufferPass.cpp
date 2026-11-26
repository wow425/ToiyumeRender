// ============================================================================
// 模块：GBufferPass（延迟渲染几何与材质 MRT 写入 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/GBufferPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "00_Core/Math/VectorMath.h"
#include "03_AssetSystem/Assets/Constants/ConstantBuffers.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 StaticMeshVS_cso / GBufferPS_cso 的唯一包含点（避免静态库 LNK2005 ODR 冲突）
#include "02_Deferred/StaticMeshVS.h"
#include "02_Deferred/GBufferPS.h"

namespace Renderer
{
	bool GBufferPass::Initialize(RenderServices& services)
	{
		if (m_Initialized)
			return true;

		BuildRootSignature();
		BuildDefaultPSO();
		m_CommonTextures = services.GetDescriptorHeap().AllocPersistentSingle();
		m_Initialized = true;
		return true;
	}

	void GBufferPass::Shutdown(RenderServices* services)
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

	void GBufferPass::BuildRootSignature()
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

		m_RootSig.Finalize(L"GBufferPass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void GBufferPass::BuildDefaultPSO()
	{
		using namespace Graphics;
		using namespace Renderer::Deferred;

		ASSERT(m_PSOCache.empty());

		m_DefaultPSO = GraphicsPSO(L"GBufferPass : Default PSO");
		m_DefaultPSO.SetRootSignature(m_RootSig);
		m_DefaultPSO.SetRasterizerState(RasterizerDefault);
		m_DefaultPSO.SetBlendState(BlendDisable);
		m_DefaultPSO.SetDepthStencilState(DepthStateReadWrite);
		m_DefaultPSO.SetInputLayout(0, nullptr);
		m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_DefaultPSO.SetRenderTargetFormats(4, GBufferFormats, SceneDepthBufferFormat);
		m_DefaultPSO.SetVertexShader(StaticMeshVS_cso, sizeof(StaticMeshVS_cso));
		m_DefaultPSO.SetPixelShader(GBufferPS_cso, sizeof(GBufferPS_cso));
	}

	uint8_t GBufferPass::GetPSOIndex(const PipelineDesc& desc)
	{
		using namespace Graphics;

		GraphicsPSO pso = m_DefaultPSO;

		const uint32_t vertexFlags = desc.VertexFlags;
		const uint32_t requirements = kVertex_Position | kVertex_Normal | kVertex_Tangent;
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
		if (vertexFlags & kVertex_UV1)
			vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,      0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		if (vertexFlags & kVertex_Color0)
			vertexLayout.push_back({ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });

		pso.SetInputLayout(static_cast<uint32_t>(vertexLayout.size()), vertexLayout.data());

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
			// [WP-1D / TD-06e] glTF MASK（AlphaTest）主 GBuffer Pass 采用 LESS_EQUAL
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
		ASSERT(m_PSOCache.size() <= 256, "Ran out of room for unique GBuffer PSOs");
		return static_cast<uint8_t>(m_PSOCache.size() - 1);
	}

	const GraphicsPSO& GBufferPass::GetOrCreatePSO(const PipelineDesc& desc)
	{
		return m_PSOCache[GetPSOIndex(desc)];
	}

	void GBufferPass::BindMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const
	{
		const auto& heap = services.GetDescriptorHeap();
		context.SetConstantBuffer(kMaterialConstants, material.MaterialCBV);
		context.SetDescriptorTable(kMaterialSRVs, heap.GetCbvSrvUavHandle(material.SRVTable).Gpu);
		context.SetDescriptorTable(kMaterialSamplers, heap.GetSamplerHandle(material.SamplerTable).Gpu);
	}

	void GBufferPass::Execute(GraphicsContext& context,
		const RenderFrameDesc& frame,
		RasterDrawHelper& drawHelper,
		const FrameBlackboard& blackboard,
		DrawPass maxPass,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		RenderServices& services,
		const Math::Matrix4* viewProjOverride)
	{
		if (!m_Initialized || frame.Camera == nullptr)
			return;

		ColorBuffer* gbuffers[4] = {
			blackboard.GetColorBuffer(FrameResourceKey::GBuffer_BaseColor),
			blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Normal),
			blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Material),
			blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Emission),
		};
		DepthBuffer* sceneDepth = blackboard.Core.SceneDepth;
		ColorBuffer* sceneColor = blackboard.Core.LinearHDRColor != nullptr
			? blackboard.Core.LinearHDRColor
			: blackboard.Core.SceneColorLDR;

		if (gbuffers[0] == nullptr || gbuffers[1] == nullptr || gbuffers[2] == nullptr ||
			gbuffers[3] == nullptr || sceneDepth == nullptr || sceneColor == nullptr)
		{
			return;
		}

		// [契约 1] 帧首一次性绑定 4 分区单可见堆（整帧 SetDescriptorHeaps <= 1）
		context.SetRootSignature(m_RootSig);
		services.GetDescriptorHeap().BindToContext(context);
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[4];
		for (uint32_t i = 0; i < 4; ++i)
		{
			context.TransitionResource(*gbuffers[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.ClearColor(*gbuffers[i]);
			rtvHandles[i] = gbuffers[i]->GetRTV();
		}
		context.TransitionResource(*sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.TransitionResource(*sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.ClearColor(*sceneColor);
		context.ClearDepth(*sceneDepth);

		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTargets(4, rtvHandles, sceneDepth->GetDSV());
		context.FlushResourceBarriers();

		GlobalConstants globals;
		globals.ViewProjMatrix = (viewProjOverride != nullptr) ? *viewProjOverride : frame.Camera->GetViewProjMatrix();
		globals.CameraPos.SetXYZ(frame.Camera->GetPosition());
		globals.SunDirection = Math::Vector4(Math::kZero);
		globals.SunIntensity = Math::Vector4(Math::kZero);
		context.SetDynamicConstantBufferView(kCommonCBV, sizeof(globals), &globals);

		context.PIXBeginEvent(L"GBufferPass");
		drawHelper.Draw(
			context,
			maxPass,
			[this](const PipelineDesc& desc) -> const GraphicsPSO& { return this->GetOrCreatePSO(desc); },
			[this, &services](GraphicsContext& ctx, const Scene::Material::Material& mat) { this->BindMaterial(ctx, mat, services); });
		context.PIXEndEvent();
	}
}
