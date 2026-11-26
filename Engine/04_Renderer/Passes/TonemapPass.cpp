// ============================================================================
// 模块：TonemapPass（全管线唯一共享显示编码 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-2C / 契约 5）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/TonemapPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include <cstdio>
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"

#include "02_Deferred/FullScreenVS.h"
#include "04_PostProcess/TonemapPS.h"
#include "04_PostProcess/TonemapBindlessPS.h"

namespace Renderer
{
	bool TonemapPass::Initialize(DXGI_FORMAT ldrFormat, RenderServices* services)
	{
		using namespace Graphics;

		SamplerDesc pointClampDesc;
		pointClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		pointClampDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampDesc.MaxAnisotropy = 1;

		m_RootSig.Reset(2, 1);
		m_RootSig.InitStaticSampler(0, pointClampDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[1].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig.Finalize(L"TonemapPass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		m_PSO = GraphicsPSO(L"TonemapPass PSO");
		m_PSO.SetRootSignature(m_RootSig);
		m_PSO.SetRasterizerState(RasterizerFullScreen);
		m_PSO.SetBlendState(BlendDisable);
		m_PSO.SetDepthStencilState(DepthStateDisabled);
		m_PSO.SetInputLayout(0, nullptr);
		m_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_PSO.SetRenderTargetFormat(ldrFormat, DXGI_FORMAT_UNKNOWN);
		m_PSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
		m_PSO.SetPixelShader(TonemapPS_cso, sizeof(TonemapPS_cso));
		m_PSO.Finalize();
		m_LdrFormat = ldrFormat;

		// [WP-3A / 契约 7.1] 在 SM 6.6 + Tier 3 硬件上同步构造 Bindless PSO（零描述符表根参数，直读 ResourceDescriptorHeap[]）
		m_UseBindless = false;
		if (services != nullptr && services->IsBindlessSM66Ready())
		{
			m_BindlessPSO = GraphicsPSO(L"TonemapPass Bindless SM6.6 PSO");
			m_BindlessPSO.SetRootSignature(services->GetBindlessGraphicsRootSignature());
			m_BindlessPSO.SetRasterizerState(RasterizerFullScreen);
			m_BindlessPSO.SetBlendState(BlendDisable);
			m_BindlessPSO.SetDepthStencilState(DepthStateDisabled);
			m_BindlessPSO.SetInputLayout(0, nullptr);
			m_BindlessPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			m_BindlessPSO.SetRenderTargetFormat(ldrFormat, DXGI_FORMAT_UNKNOWN);
			m_BindlessPSO.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
			m_BindlessPSO.SetPixelShader(TonemapBindlessPS_cso, sizeof(TonemapBindlessPS_cso));
			m_BindlessPSO.Finalize();
			m_UseBindless = (m_BindlessPSO.GetPipelineStateObject() != nullptr);
		}

		LOG_INFO(L"[WP-3A][SM6.6] TonemapPass initialized (path=%ws)\n",
			m_UseBindless ? L"SM6.6_Bindless_ResourceDescriptorHeap" : L"SM6.4_Traditional_DescriptorTable");

		m_Initialized = true;
		return true;
	}

	void TonemapPass::Shutdown()
	{
		m_HotPSO.Reset();
		m_HotBindlessPSO.Reset();
		m_RootSig.Reset(0, 0);
		m_LdrFormat   = DXGI_FORMAT_UNKNOWN;
		m_UseBindless = false;
		m_Initialized = false;
	}

	void TonemapPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized)
			return;

		if (m_UseBindless && services.IsBindlessSM66Ready())
		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "TonemapBindlessPS";
			desc.SourceRelPath = L"Engine/Shader/04_PostProcess/TonemapBindlessPS.hlsl";
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
			desc.Id            = "TonemapPS";
			desc.SourceRelPath = L"Engine/Shader/04_PostProcess/TonemapPS.hlsl";
			desc.EntryPoint    = L"MainPS";
			desc.TargetProfile = L"ps_6_4";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
				return HotReloadPixelShader(dxil, /*forBindless=*/false, services, outErr);
			};
			reloader.Register(std::move(desc));
		}
	}

	bool TonemapPass::HotReloadPixelShader(const std::vector<uint8_t>& dxil, bool forBindless,
		RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "TonemapPass not initialized or empty DXIL";
			return false;
		}

		ID3D12RootSignature* rs = forBindless
			? services.GetBindlessGraphicsRootSignature().GetSignature()
			: m_RootSig.GetSignature();
		if (rs == nullptr)
		{
			outErr = "RootSignature is null";
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
		psoDesc.RTVFormats[0]         = m_LdrFormat;
		psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
		psoDesc.SampleDesc.Count      = 1;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate;
		const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&psoDesc, TY_IID_PPV_ARGS(&candidate));
		if (FAILED(hr) || candidate == nullptr)
		{
			char buf[128] = {};
			std::snprintf(buf, sizeof(buf), "CreateGraphicsPipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(forBindless ? L"TonemapPass HotReload Bindless PSO" : L"TonemapPass HotReload PSO");

		// [契约 7.2 事务化替换] 旧热重载 PSO 经帧尾延迟释放队列退场，绝不在途析构！
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

	void TonemapPass::Execute(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const RenderParams& params,
		bool bypassDisplayEncode,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		RenderServices& services) const
	{
		if (!m_Initialized)
			return;

		ColorBuffer* hdrInput  = blackboard.Core.LinearHDRColor;
		ColorBuffer* ldrOutput = blackboard.Core.SceneColorLDR;
		if (hdrInput == nullptr || ldrOutput == nullptr)
			return;

		context.PIXBeginEvent(L"TonemapPass");

		// 若前置 Pass（如空场景清屏）尚未在本命令列表绑定单可见堆，此处补绑一次，仍严格保证整帧 <= 1
		if (context.GetSetDescriptorHeapsCount() == 0)
		{
			services.GetDescriptorHeap().BindToContext(context);
		}

		context.TransitionResource(*hdrInput,  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*ldrOutput, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.FlushResourceBarriers();

		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTarget(ldrOutput->GetRTV());

		// [契约 1] 经 4 分区单可见堆 Zone B 瞬态暂存 LinearHDRColor SRV，零切堆并获得全局 Bindless HeapIndex
		const BindlessAllocation stagedHdr =
			services.GetDescriptorHeap().StageTransientDescriptor(hdrInput->GetSRV());

		Constants cb = {};
		cb.Exposure            = params.Exposure;
		cb.EnableTonemap       = params.EnableTonemap ? 1u : 0u;
		cb.EnableSRGBEncode    = params.EnableSRGBEncode ? 1u : 0u;
		cb.BypassDisplayEncode = bypassDisplayEncode ? 1u : 0u;

		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		if (m_UseBindless && services.IsBindlessSM66Ready())
		{
			// [WP-3A / 契约 7.1] SM 6.6 Bindless：仅传 1 个 uint 根常量下标 + b2 动态 CBV，零描述符表绑定！
			context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			ID3D12PipelineState* pso = m_HotBindlessPSO ? m_HotBindlessPSO.Get() : m_BindlessPSO.GetPipelineStateObject();
			context.SetPipelineStateRaw(pso);
			context.SetConstants(0, stagedHdr.HeapIndex);
			context.SetDynamicConstantBufferView(2, sizeof(cb), &cb);
		}
		else
		{
			context.SetRootSignature(m_RootSig);
			ID3D12PipelineState* pso = m_HotPSO ? m_HotPSO.Get() : m_PSO.GetPipelineStateObject();
			context.SetPipelineStateRaw(pso);
			context.SetDescriptorTable(0, stagedHdr.Gpu);
			context.SetDynamicConstantBufferView(1, sizeof(cb), &cb);
		}

		context.Draw(3, 0);

		context.PIXEndEvent();
	}
}
