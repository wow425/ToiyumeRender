#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Common/GFL2DebugOverlayPass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"

#include "02_Deferred/FullScreenVS.h"
#include "08_Character/GFL2DebugViewPS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	bool GFL2DebugOverlayPass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M2] GFL2DebugOverlayPass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_PSO = GraphicsPSO(L"GFL2DebugOverlayPass FullScreen PSO");
		m_PSO.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		m_PSO.SetRasterizerState(Graphics::RasterizerFullScreen);
		m_PSO.SetBlendState(Graphics::BlendDisable);
		m_PSO.SetDepthStencilState(Graphics::DepthStateDisabled);
		m_PSO.SetSampleMask(UINT_MAX);
		m_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_PSO.SetInputLayout(0, nullptr);
		m_PSO.SetRenderTargetFormat(Deferred::LinearHDRBufferFormat, DXGI_FORMAT_UNKNOWN);
		m_PSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
		m_PSO.SetPixelShader(GFL2DebugViewPS_cso, sizeof(GFL2DebugViewPS_cso));
		m_PSO.Finalize();

		m_Initialized = true;
		return true;
	}

	void GFL2DebugOverlayPass::Shutdown()
	{
		m_HotPSO.Reset();
		m_Initialized = false;
	}

	bool GFL2DebugOverlayPass::RebuildPSO(const void* psBytecode, size_t psSize, Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || psBytecode == nullptr || psSize == 0)
		{
			outErr = "GFL2DebugOverlayPass not initialized or empty PS bytecode";
			return false;
		}

		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature        = rs;
		d.VS                    = { FullScreenVS_cso, sizeof(FullScreenVS_cso) };
		d.PS                    = { psBytecode, psSize };
		d.BlendState            = Graphics::BlendDisable;
		d.SampleMask            = UINT_MAX;
		d.RasterizerState       = Graphics::RasterizerFullScreen;
		d.DepthStencilState     = Graphics::DepthStateDisabled;
		d.InputLayout           = { nullptr, 0 };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets      = 1;
		d.RTVFormats[0]         = Deferred::LinearHDRBufferFormat;
		d.DSVFormat             = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count      = 1;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> cand;
		const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&cand));
		if (FAILED(hr) || cand == nullptr)
		{
			char buf[128] = {};
			std::snprintf(buf, sizeof(buf), "GFL2DebugOverlayPass CreatePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
			outErr = buf;
			return false;
		}
		cand->SetName(L"GFL2DebugOverlayPass Hot PSO");

		if (m_HotPSO != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> unk;
			m_HotPSO.As(&unk);
			services.GetReleaseQueue().Enqueue(std::move(unk));
		}
		m_HotPSO = std::move(cand);
		return true;
	}

	void GFL2DebugOverlayPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		Graphics::ShaderHotReloadDesc desc;
		desc.Id            = "GFL2DebugViewPS";
		desc.SourceRelPath = L"Engine/Shader/08_Character/GFL2DebugViewPS.hlsl";
		desc.EntryPoint    = L"MainPS";
		desc.TargetProfile = L"ps_6_6";
		desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
		{
			return RebuildPSO(dxil.data(), dxil.size(), services, outErr);
		};
		reloader.Register(std::move(desc));
	}

	void GFL2DebugOverlayPass::Execute(
		GraphicsContext& context,
		ColorBuffer& linearHDR,
		DepthBuffer& sceneDepth,
		uint32_t debugViewMode,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services)
	{
		if (!m_Initialized)
			return;

		context.PIXBeginEvent(L"GFL2DebugOverlayPass (Stencil/Depth)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.FlushResourceBarriers();

		const Renderer::BindlessAllocation stagedStencil =
			services.GetDescriptorHeap().StageTransientDescriptor(sceneDepth.GetStencilSRV());
		const Renderer::BindlessAllocation stagedDepth =
			services.GetDescriptorHeap().StageTransientDescriptor(sceneDepth.GetDepthSRV());

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		ID3D12PipelineState* raw = m_HotPSO ? m_HotPSO.Get() : m_PSO.GetPipelineStateObject();
		context.SetPipelineStateRaw(raw);

		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTarget(linearHDR.GetRTV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		::GFL2::GFL2RootConstants rc = {};
		rc.AuxTex0SrvIdx = stagedStencil.HeapIndex;
		rc.AuxTex1SrvIdx = stagedDepth.HeapIndex;
		rc.PassSubMode   = debugViewMode;
		context.SetConstantArray(0, 16, &rc);

		context.Draw(3, 0);
		context.PIXEndEvent();
	}
}
