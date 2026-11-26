// ============================================================================
// 模块：ShadowPass（可插拔方向光阴影 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/ShadowPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "04_Renderer/Passes/CascadedShadowMap.h"
#include "04_Renderer/Passes/SimpleShadowMap.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 ShadowDepthVS_cso / ShadowDepthAlphaTestPS_cso 的唯一包含点（防静态库 ODR 冲突）
#include "03_Shadow/ShadowDepthVS.h"
#include "03_Shadow/ShadowDepthAlphaTestPS.h"

#include <algorithm>

namespace Renderer
{
	ShadowParams ShadowParams::FromRenderParams(const RenderParams& p)
	{
		ShadowParams sp;
		sp.Algorithm       = (p.ShadowAlgorithm == 1u) ? 1u : 0u;
		sp.Resolution      = (std::clamp)(p.ShadowResolution, 256u, 4096u);
		sp.CascadeCount    = (std::clamp)(p.ShadowCascadeCount, 2u, 4u);
		sp.NormalBias      = p.ShadowNormalBias;
		sp.DepthBias       = p.ShadowDepthBias;
		sp.PCFRadius       = p.ShadowPCFRadius;
		sp.Strength        = p.ShadowStrength;
		sp.SunDirection[0] = p.SunDirection[0];
		sp.SunDirection[1] = p.SunDirection[1];
		sp.SunDirection[2] = p.SunDirection[2];
		return sp;
	}

	bool ShadowPass::Initialize(RenderServices& services)
	{
		(void)services;
		if (m_Initialized)
			return true;

		BuildRootSignature();
		BuildBasePSOs();
		m_Initialized = true;
		return true;
	}

	void ShadowPass::Shutdown(RenderServices* services)
	{
		if (!m_Initialized)
			return;

		if (m_Algorithm != nullptr)
		{
			if (services != nullptr)
				m_Algorithm->Shutdown(services->GetReleaseQueue());
			else
				m_Algorithm->Shutdown(Graphics::GetDeferredReleaseQueue());
			m_Algorithm.reset();
		}

		m_PSOCache.clear();
		m_RootSig.Reset(0, 0);
		m_ActiveAlgType    = 0xFFFFFFFFu;
		m_ActiveResolution = 0;
		m_ActiveCascades   = 0;
		m_LastSwitchError.clear();
		m_Initialized = false;
	}

	void ShadowPass::BuildRootSignature()
	{
		static_assert(static_cast<uint32_t>(kShadowMeshCBV) == static_cast<uint32_t>(kMeshConstants),
			"ShadowPass kShadowMeshCBV must equal BaseRenderer::kMeshConstants (0) for RasterDrawHelper");

		m_RootSig.Reset(kShadowNumSlots, 0);
		m_RootSig[kShadowMeshCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		m_RootSig[kShadowViewCBV].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_VERTEX);
		m_RootSig[kShadowMaterialCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kShadowMaterialSRV].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 5, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[kShadowMaterialSmp].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 5, D3D12_SHADER_VISIBILITY_PIXEL);

		m_RootSig.Finalize(L"ShadowPass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	}

	void ShadowPass::BuildBasePSOs()
	{
		using namespace Graphics;

		// [Shadow Pancaking + Slope Bias]
		//   - DepthClipEnable = FALSE：允许位于光源近平面后方的遮挡体贴合到 z=0，防止高大建筑顶部被裁漏光；
		//   - DepthBias + SlopeScaledDepthBias：硬件级斜面深度偏置，配合着色器法线偏移双重消除 Shadow Acne。
		D3D12_RASTERIZER_DESC shadowRast = RasterizerDefault;
		shadowRast.DepthBias            = 100;
		shadowRast.DepthBiasClamp       = 0.01f;
		shadowRast.SlopeScaledDepthBias = 1.5f;
		shadowRast.DepthClipEnable      = FALSE;

		m_OpaqueBasePSO = GraphicsPSO(L"ShadowPass : Opaque Depth-Only PSO");
		m_OpaqueBasePSO.SetRootSignature(m_RootSig);
		m_OpaqueBasePSO.SetRasterizerState(shadowRast);
		m_OpaqueBasePSO.SetBlendState(BlendNoColorWrite);
		m_OpaqueBasePSO.SetDepthStencilState(DepthStateReadWrite);
		m_OpaqueBasePSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_OpaqueBasePSO.SetDepthTargetFormat(Renderer::Deferred::ShadowMapBufferFormat);
		m_OpaqueBasePSO.SetVertexShader(ShadowDepthVS_cso, sizeof(ShadowDepthVS_cso));
		// 不透明网格不设 PixelShader，走 GPU 双倍纯深度光栅化通路

		m_AlphaTestBasePSO = m_OpaqueBasePSO;
		m_AlphaTestBasePSO.SetName(L"ShadowPass : AlphaTest Depth PSO");
		m_AlphaTestBasePSO.SetPixelShader(ShadowDepthAlphaTestPS_cso, sizeof(ShadowDepthAlphaTestPS_cso));
	}

	const GraphicsPSO& ShadowPass::GetOrCreatePSO(const PipelineDesc& desc)
	{
		const bool isAlphaTest = (desc.MaterialFlags & Scene::Material::kMaterial_AlphaTest) != 0;
		GraphicsPSO pso = isAlphaTest ? m_AlphaTestBasePSO : m_OpaqueBasePSO;

		const uint32_t vertexFlags = desc.VertexFlags;
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

		pso.SetInputLayout(static_cast<uint32_t>(vertexLayout.size()), vertexLayout.data());

		D3D12_RASTERIZER_DESC shadowRast = Graphics::RasterizerDefault;
		shadowRast.DepthBias            = 100;
		shadowRast.DepthBiasClamp       = 0.01f;
		shadowRast.SlopeScaledDepthBias = 1.5f;
		shadowRast.DepthClipEnable      = FALSE;
		if ((desc.MaterialFlags & Scene::Material::kMaterial_DoubleSided) != 0 || isAlphaTest)
			shadowRast.CullMode = D3D12_CULL_MODE_NONE;
		pso.SetRasterizerState(shadowRast);

		pso.Finalize();

		for (const auto& cached : m_PSOCache)
		{
			if (cached.GetPipelineStateObject() == pso.GetPipelineStateObject())
				return cached;
		}
		m_PSOCache.push_back(pso);
		ASSERT(m_PSOCache.size() <= 128, "Ran out of room for unique ShadowPass PSOs");
		return m_PSOCache.back();
	}

	void ShadowPass::BindShadowMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const
	{
		const auto& heap = services.GetDescriptorHeap();
		context.SetConstantBuffer(kShadowMaterialCBV, material.MaterialCBV);
		context.SetDescriptorTable(kShadowMaterialSRV, heap.GetCbvSrvUavHandle(material.SRVTable).Gpu);
		context.SetDescriptorTable(kShadowMaterialSmp, heap.GetSamplerHandle(material.SamplerTable).Gpu);
	}

	bool ShadowPass::EnsureAlgorithm(RenderServices& services, const RenderParams& params)
	{
		if (!m_Initialized)
			return false;

		const ShadowParams sp = ShadowParams::FromRenderParams(params);
		const uint32_t targetCascades = (sp.Algorithm == 1u) ? sp.CascadeCount : 1u;

		if (m_Algorithm != nullptr &&
			m_ActiveAlgType == sp.Algorithm &&
			m_ActiveResolution == sp.Resolution &&
			m_ActiveCascades == targetCascades)
		{
			return true;
		}

		// [契约 2/4 先备后切协议] CheckSupport -> VramBudgetGuard -> 候选 Initialize -> 原子替换 + 旧实例延迟释放
		std::unique_ptr<IShadowAlgorithm> candidate;
		if (sp.Algorithm == 1u)
			candidate = std::make_unique<CascadedShadowMap>();
		else
			candidate = std::make_unique<SimpleShadowMap>();

		const AlgorithmCapsReport capsReport = candidate->CheckSupport(Graphics::GetGpuCaps(), sp);
		if (!capsReport.Supported)
		{
			m_LastSwitchError = capsReport.UnsupportedReason.empty()
				? "Shadow algorithm unsupported on current GPU"
				: capsReport.UnsupportedReason;
			LOG_ERROR(L"[ShadowPass] 拒绝切换至 %hs：%hs\n", candidate->GetId(), m_LastSwitchError.c_str());
			return (m_Algorithm != nullptr);
		}

		std::string vramErr;
		if (!Graphics::CheckVramBudgetForAllocation(capsReport.EstimatedVramBytes, 0.90f, &vramErr))
		{
			m_LastSwitchError = vramErr;
			LOG_ERROR(L"[ShadowPass][VramBudgetGuard] 拒绝切换至 %hs：%hs（旧算法保持完好）\n",
				candidate->GetId(), vramErr.c_str());
			return (m_Algorithm != nullptr);
		}

		std::string initErr;
		if (!candidate->Initialize(services, sp, &initErr))
		{
			m_LastSwitchError = initErr;
			LOG_ERROR(L"[ShadowPass] 候选阴影算法 %hs 初始化失败：%hs\n", candidate->GetId(), initErr.c_str());
			candidate->Shutdown(services.GetReleaseQueue());
			return (m_Algorithm != nullptr);
		}

		std::unique_ptr<IShadowAlgorithm> oldAlg = std::move(m_Algorithm);
		m_Algorithm        = std::move(candidate);
		m_ActiveAlgType    = sp.Algorithm;
		m_ActiveResolution = sp.Resolution;
		m_ActiveCascades   = targetCascades;
		m_LastSwitchError.clear();

		if (oldAlg != nullptr)
		{
			LOG_INFO(L"[WP-3C][ShadowPass] 热切换阴影算法：%hs -> %hs (res=%u, cascades=%u)\n",
				oldAlg->GetId(), m_Algorithm->GetId(), m_ActiveResolution, m_ActiveCascades);
			oldAlg->Shutdown(services.GetReleaseQueue());
		}
		else
		{
			LOG_INFO(L"[WP-3C][ShadowPass] 初始化阴影算法：%hs (res=%u, cascades=%u)\n",
				m_Algorithm->GetId(), m_ActiveResolution, m_ActiveCascades);
		}
		return true;
	}

	ShadowOutput ShadowPass::Execute(GraphicsContext& context,
		const RenderFrameDesc& frame,
		RasterDrawHelper& drawHelper,
		const RenderParams& params,
		RenderServices& services)
	{
		ShadowOutput out;
		if (!m_Initialized || frame.Camera == nullptr || m_Algorithm == nullptr)
			return out;

		// 若本命令列表尚未绑定统一单可见堆，则帧首绑定一次（整帧 SetDescriptorHeaps 仍严格 <= 1）
		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.SetRootSignature(m_RootSig);
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		const ShadowParams sp = ShadowParams::FromRenderParams(params);
		auto drawCascade = [this, &drawHelper, &services](GraphicsContext& ctx, const Math::Matrix4& lightViewProj)
		{
			struct alignas(16) ShadowViewCB
			{
				Math::Matrix4 LightViewProj;
			};
			static_assert(sizeof(ShadowViewCB) == 64 && sizeof(ShadowViewCB) % 16 == 0,
				"ShadowViewCB must match ShadowDepthVS.hlsl cbuffer ShadowViewConstants (64B)");

			ShadowViewCB cb = { lightViewProj };
			ctx.SetDynamicConstantBufferView(kShadowViewCBV, sizeof(cb), &cb);

			drawHelper.Draw(
				ctx,
				DrawPass::kOpaque,
				[this](const PipelineDesc& desc) -> const GraphicsPSO& { return this->GetOrCreatePSO(desc); },
				[this, &services](GraphicsContext& c, const Scene::Material::Material& mat) { this->BindShadowMaterial(c, mat, services); });
		};

		return m_Algorithm->Render(context, *frame.Camera, frame.Models, sp, services, drawCascade);
	}
}
