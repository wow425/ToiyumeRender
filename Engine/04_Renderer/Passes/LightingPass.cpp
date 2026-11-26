// ============================================================================
// 模块：LightingPass（延迟光照全屏着色 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-2A -> WP-2C -> WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/LightingPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Lighting/LightingSystem.h"

#include "02_Deferred/FullScreenVS.h"
#include "02_Deferred/LightingPS.h"
#include "02_Deferred/LightingBindlessPS.h"

#include <algorithm>
#include <cstdio>

namespace Renderer
{
	bool LightingPass::Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, RenderServices* services)
	{
		using namespace Graphics;

		SamplerDesc linearSamplerDesc;
		linearSamplerDesc.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		linearSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linearSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linearSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		m_RootSig.Reset(static_cast<uint32_t>(Slot::Count), 1);
		m_RootSig.InitStaticSampler(0, linearSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		m_RootSig[static_cast<uint32_t>(Slot::GBuffer_BaseColor)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::GBuffer_BaseColor), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::GBuffer_Normal)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::GBuffer_Normal), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::GBuffer_Material)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::GBuffer_Material), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::GBuffer_Emission)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::GBuffer_Emission), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::Depth)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::Depth), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::IBL_Irradiance)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::IBL_Irradiance), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::IBL_Prefilter)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::IBL_Prefilter), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::IBL_BRDFLUT)].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<uint32_t>(Slot::IBL_BRDFLUT), 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::Light)].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[static_cast<uint32_t>(Slot::Camera)].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_PIXEL);

		m_RootSig.Finalize(L"DeferredRenderer Lighting Pass RootSig",
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		m_PSO = GraphicsPSO(L"DeferredRenderer : Lighting PSO");
		m_PSO.SetRootSignature(m_RootSig);
		m_PSO.SetRasterizerState(RasterizerFullScreen);
		m_PSO.SetBlendState(BlendDisable);
		m_PSO.SetDepthStencilState(DepthStateDisabled);
		m_PSO.SetInputLayout(0, nullptr);
		m_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_PSO.SetRenderTargetFormat(colorFormat, depthFormat);
		m_PSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
		m_PSO.SetPixelShader(LightingPS_cso, sizeof(LightingPS_cso));
		m_PSO.Finalize();

		m_ColorFormat = colorFormat;
		m_DepthFormat = depthFormat;

		// [WP-3C / 契约 7.1] 在 SM 6.6 + Tier 3 硬件上构造 Bindless Lighting PSO：
		// 复用全局共享 BindlessGraphicsRootSignature（自带 s4 ShadowComparison 静态采样器），
		// 直接经 ResourceDescriptorHeap[] 采样 GBuffer / IBL / ShadowMap / SSAOMap，零描述符表根参数。
		m_UseBindless = false;
		if (services != nullptr && services->IsBindlessSM66Ready())
		{
			m_BindlessPSO = GraphicsPSO(L"DeferredRenderer : Lighting Bindless SM6.6 PSO");
			m_BindlessPSO.SetRootSignature(services->GetBindlessGraphicsRootSignature());
			m_BindlessPSO.SetRasterizerState(RasterizerFullScreen);
			m_BindlessPSO.SetBlendState(BlendDisable);
			m_BindlessPSO.SetDepthStencilState(DepthStateDisabled);
			m_BindlessPSO.SetInputLayout(0, nullptr);
			m_BindlessPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			m_BindlessPSO.SetRenderTargetFormat(colorFormat, depthFormat);
			m_BindlessPSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
			m_BindlessPSO.SetPixelShader(LightingBindlessPS_cso, sizeof(LightingBindlessPS_cso));
			m_BindlessPSO.Finalize();
			m_UseBindless = (m_BindlessPSO.GetPipelineStateObject() != nullptr);
		}

		LOG_INFO(L"[WP-3C][LightingPass] initialized (path=%ws)\n",
			m_UseBindless ? L"SM6.6_Bindless_Shadow_SSAO" : L"SM6.4_Traditional_Fallback");

		m_Initialized = true;
		return true;
	}

	void LightingPass::Shutdown()
	{
		m_HotPSO.Reset();
		m_HotBindlessPSO.Reset();
		m_RootSig.Reset(0, 0);
		m_UseBindless = false;
		m_Initialized = false;
	}

	void LightingPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized)
			return;

		if (m_UseBindless && services.IsBindlessSM66Ready())
		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "LightingBindlessPS";
			desc.SourceRelPath = L"Engine/Shader/02_Deferred/LightingBindlessPS.hlsl";
			desc.EntryPoint    = L"MainPS";
			desc.TargetProfile = L"ps_6_6";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
				return HotReloadPixelShader(dxil, /*forBindless=*/true, services, outErr);
			};
			reloader.Register(std::move(desc));
		}
		else
		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "LightingPS";
			desc.SourceRelPath = L"Engine/Shader/02_Deferred/LightingPS.hlsl";
			desc.EntryPoint    = L"MainPS";
			desc.TargetProfile = L"ps_6_4";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
				return HotReloadPixelShader(dxil, /*forBindless=*/false, services, outErr);
			};
			reloader.Register(std::move(desc));
		}
	}

	bool LightingPass::HotReloadPixelShader(const std::vector<uint8_t>& dxil, bool forBindless,
		RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "LightingPass not initialized or empty DXIL";
			return false;
		}
		ID3D12RootSignature* rs = forBindless
			? services.GetBindlessGraphicsRootSignature().GetSignature()
			: m_RootSig.GetSignature();
		if (rs == nullptr)
		{
			outErr = "LightingPass RootSignature is null";
			return false;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature        = rs;
		psoDesc.VS                    = { FullScreenVS_cso, sizeof(FullScreenVS_cso) };
		psoDesc.PS                    = { dxil.data(), dxil.size() };
		psoDesc.BlendState            = Graphics::BlendDisable;
		psoDesc.SampleMask            = UINT_MAX;
		psoDesc.RasterizerState       = Graphics::RasterizerFullScreen;
		psoDesc.DepthStencilState     = Graphics::DepthStateDisabled;
		psoDesc.InputLayout           = { nullptr, 0 };
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets      = 1;
		psoDesc.RTVFormats[0]         = m_ColorFormat;
		psoDesc.DSVFormat             = m_DepthFormat;
		psoDesc.SampleDesc.Count      = 1;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate;
		const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&psoDesc, TY_IID_PPV_ARGS(&candidate));
		if (FAILED(hr) || candidate == nullptr)
		{
			char buf[128] = {};
			std::snprintf(buf, sizeof(buf), "LightingPass CreateGraphicsPipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(forBindless ? L"LightingPass HotReload Bindless PSO" : L"LightingPass HotReload PSO");

		auto& slot = forBindless ? m_HotBindlessPSO : m_HotPSO;
		if (slot != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> oldUnk;
			slot.As(&oldUnk);
			services.GetReleaseQueue().Enqueue(std::move(oldUnk));
		}
		slot = std::move(candidate);
		return true;
	}

	void LightingPass::Execute(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const Scene::Camera::Camera& camera,
		uint32_t rtWidth,
		uint32_t rtHeight,
		RenderServices& services,
		const ExecutionExtras* extras) const
	{
		if (!m_Initialized)
			return;

		ColorBuffer* gbufBaseColor = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_BaseColor);
		ColorBuffer* gbufNormal    = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Normal);
		ColorBuffer* gbufMaterial  = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Material);
		ColorBuffer* gbufEmission  = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Emission);
		DepthBuffer* sceneDepth    = blackboard.Core.SceneDepth;
		ColorBuffer* outputColor   = blackboard.Core.LinearHDRColor != nullptr
			? blackboard.Core.LinearHDRColor
			: blackboard.Core.SceneColorLDR;

		if (gbufBaseColor == nullptr || gbufNormal == nullptr || gbufMaterial == nullptr ||
			gbufEmission == nullptr || sceneDepth == nullptr || outputColor == nullptr)
		{
			return;
		}

		context.PIXBeginEvent(L"DeferredLightingPass");

		// 1. 资源状态转换：GBuffer -> SRV，Depth -> 只读深度+SRV（兼容紧随其后的 Skybox 只读深度测试），Output -> RTV
		context.TransitionResource(*gbufBaseColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*gbufNormal,    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*gbufMaterial,  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*gbufEmission,  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*sceneDepth,    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*outputColor,   D3D12_RESOURCE_STATE_RENDER_TARGET);

		ColorBuffer* iblIrradiance = blackboard.GetColorBuffer(FrameResourceKey::IBL_Irradiance);
		ColorBuffer* iblPrefilter  = blackboard.GetColorBuffer(FrameResourceKey::IBL_Prefilter);
		ColorBuffer* iblBrdfLut    = blackboard.GetColorBuffer(FrameResourceKey::IBL_BRDFLUT);
		const bool iblReady = (iblIrradiance != nullptr && iblPrefilter != nullptr && iblBrdfLut != nullptr);
		if (iblReady)
		{
			context.TransitionResource(*iblIrradiance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context.TransitionResource(*iblPrefilter,  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context.TransitionResource(*iblBrdfLut,    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		context.FlushResourceBarriers();

		// 2. [契约 1] 全部 8 个基础输入 SRV 一次性暂存至统一堆 Zone B（整帧零切堆）
		const D3D12_CPU_DESCRIPTOR_HANDLE lightingSrvs[8] = {
			gbufBaseColor->GetSRV(),
			gbufNormal->GetSRV(),
			gbufMaterial->GetSRV(),
			gbufEmission->GetSRV(),
			sceneDepth->GetDepthSRV(),
			iblReady ? iblIrradiance->GetSRV() : Graphics::GetDefaultTexture(Graphics::kBlackCubeMap),
			iblReady ? iblPrefilter->GetSRV()  : Graphics::GetDefaultTexture(Graphics::kBlackCubeMap),
			iblReady ? iblBrdfLut->GetSRV()    : Graphics::GetDefaultTexture(Graphics::kBlackOpaque2D),
		};

		UnifiedDescriptorHeap& unifiedHeap = services.GetDescriptorHeap();
		const BindlessAllocation stagedBlock = unifiedHeap.StageTransientDescriptors(lightingSrvs, 8);

		const float rtW = static_cast<float>((std::max)(1u, rtWidth));
		const float rtH = static_cast<float>((std::max)(1u, rtHeight));
		const Math::Matrix4 invViewProj =
			(extras != nullptr && extras->InvViewProjOverride != nullptr)
			? *extras->InvViewProjOverride
			: camera.GetInViewProjMatrix();

		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetRenderTarget(outputColor->GetRTV());

		if (m_UseBindless && services.IsBindlessSM66Ready())
		{
			// ---- SM 6.6 Bindless 主路径（支持 ShadowMap/CSM + SSAO） ----
			uint32_t shadowIdx = kInvalidBindlessIndex;
			if (extras != nullptr && extras->Shadow != nullptr &&
				extras->Shadow->CascadeCount > 0u &&
				extras->Shadow->ShadowMapBindlessIdx != kInvalidBindlessIndex)
			{
				shadowIdx = extras->Shadow->ShadowMapBindlessIdx;
			}

			uint32_t ssaoIdx = kInvalidBindlessIndex;
			const bool ssaoOn = (extras != nullptr && extras->EnableSSAO && extras->SSAOMap != nullptr);
			if (ssaoOn)
			{
				const BindlessAllocation stagedSsao = unifiedHeap.StageTransientDescriptor(extras->SSAOMap->GetSRV());
				ssaoIdx = stagedSsao.HeapIndex;
			}

			// [契约 4] 若 FrameBlackboard::PrimarySurface 已完成 Bindless SRV 暂存，直接消费其统一表面下标
			// （AlbedoAO / Normal / Material / Emission / Depth），验证延迟管线对统一表面契约的真实闭环消费。
			const PrimarySurfaceDesc& surf = blackboard.PrimarySurface;
			const bool usePrimarySurface = surf.IsBindlessReady() && surf.Emission.HasBindlessSrv();
			const uint32_t rootIndices[12] = {
				usePrimarySurface ? surf.AlbedoAO.SrvBindlessIdx : (stagedBlock.HeapIndex + 0u),
				usePrimarySurface ? surf.Normal.SrvBindlessIdx   : (stagedBlock.HeapIndex + 1u),
				usePrimarySurface ? surf.Material.SrvBindlessIdx : (stagedBlock.HeapIndex + 2u),
				usePrimarySurface ? surf.Emission.SrvBindlessIdx : (stagedBlock.HeapIndex + 3u),
				usePrimarySurface ? surf.Depth.SrvBindlessIdx    : (stagedBlock.HeapIndex + 4u),
				stagedBlock.HeapIndex + 5u,
				stagedBlock.HeapIndex + 6u,
				stagedBlock.HeapIndex + 7u,
				shadowIdx,
				ssaoIdx,
				0u,
				0u,
			};

			ExtraConstants extraCB = {};
			extraCB.CameraPos.SetXYZ(camera.GetPosition());
			extraCB.InvViewProj         = invViewProj;
			extraCB.RenderTargetSize    = DirectX::XMFLOAT2(rtW, rtH);
			extraCB.InvRenderTargetSize = DirectX::XMFLOAT2(1.0f / rtW, 1.0f / rtH);
			extraCB.ViewMatrix          = camera.GetViewMatrix();

			if (shadowIdx != kInvalidBindlessIndex)
			{
				const ShadowOutput& sh = *extras->Shadow;
				for (uint32_t c = 0; c < 4u; ++c)
					extraCB.LightViewProj[c] = sh.LightViewProj[c];
				extraCB.CascadeSplits = DirectX::XMFLOAT4(
					sh.SplitDepths[0], sh.SplitDepths[1], sh.SplitDepths[2], sh.SplitDepths[3]);
				extraCB.ShadowParams0 = DirectX::XMFLOAT4(
					static_cast<float>(sh.CascadeCount),
					sh.NormalBias,
					sh.DepthBias,
					sh.PCFRadius);
				extraCB.ShadowParams1 = DirectX::XMFLOAT4(
					sh.Resolution > 0u ? (1.0f / static_cast<float>(sh.Resolution)) : 0.0f,
					sh.Strength,
					ssaoOn ? 1.0f : 0.0f,
					0.0f);
			}
			else
			{
				extraCB.ShadowParams0 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
				extraCB.ShadowParams1 = DirectX::XMFLOAT4(0.0f, 0.0f, ssaoOn ? 1.0f : 0.0f, 0.0f);
			}

			context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			context.SetPipelineStateRaw(m_HotBindlessPSO ? m_HotBindlessPSO.Get() : m_BindlessPSO.GetPipelineStateObject());
			context.SetConstantArray(0, 12, rootIndices);
			context.SetConstantBuffer(1, Scene::LightingSystem::m_LightGPUBuffer.GetGpuVirtualAddress());
			context.SetDynamicConstantBufferView(2, sizeof(extraCB), &extraCB);
		}
		else
		{
			// ---- SM 6.4 传统描述符表回退路径 ----
			context.SetRootSignature(m_RootSig);
			context.SetPipelineStateRaw(m_HotPSO ? m_HotPSO.Get() : m_PSO.GetPipelineStateObject());
			for (uint32_t slot = 0; slot < 8; ++slot)
			{
				context.SetDescriptorTable(slot, unifiedHeap.GetCbvSrvUavHandle(stagedBlock.HeapIndex + slot).Gpu);
			}

			context.SetConstantBuffer(static_cast<uint32_t>(Slot::Light),
				Scene::LightingSystem::m_LightGPUBuffer.GetGpuVirtualAddress());

			Scene::Camera::CameraData cameraCB = {};
			cameraCB.CameraPos.SetXYZ(camera.GetPosition());
			cameraCB.InvViewProj         = invViewProj;
			cameraCB.RenderTargetSize    = DirectX::XMFLOAT2(rtW, rtH);
			cameraCB.InvRenderTargetSize = DirectX::XMFLOAT2(1.0f / rtW, 1.0f / rtH);
			context.SetDynamicConstantBufferView(static_cast<uint32_t>(Slot::Camera), sizeof(cameraCB), &cameraCB);
		}

		context.Draw(3, 0);

		context.PIXEndEvent();
	}
}
