#include "00_Core/PCH.h"
#include "04_Renderer/Passes/PostProcess/GFL2PostProcessPass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/Passes/TAAPass.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"

#include "02_Deferred/FullScreenVS.h"
#include "08_Character/GFL2SSAOCS.h"
#include "08_Character/GFL2SSAOCompositePS.h"
#include "08_Character/GFL2TAACS.h"
#include "08_Character/GFL2BloomPS.h"
#include "08_Character/GFL2UberPostLUTPS.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Renderer::GFL2
{
	void GFL2PostTargets::Create(uint32_t w, uint32_t h)
	{
		Destroy();
		if (w == 0 || h == 0)
			return;
		Width  = w;
		Height = h;

		const uint32_t hw = (std::max)(w / 2u, 1u);
		const uint32_t hh = (std::max)(h / 2u, 1u);
		SSAORaw = Graphics::BufferManager::CreateColorBuffer(L"GFL2 SSAORaw", hw, hh, DXGI_FORMAT_R16_FLOAT);

		const Color hdrClear(0.0f, 0.0f, 0.0f, 1.0f);
		TAAHistory[0] = Graphics::BufferManager::CreateColorBuffer(L"GFL2 TAAHist0", w, h, Deferred::LinearHDRBufferFormat, 1, &hdrClear);
		TAAHistory[1] = Graphics::BufferManager::CreateColorBuffer(L"GFL2 TAAHist1", w, h, Deferred::LinearHDRBufferFormat, 1, &hdrClear);
		PostWorkHDR   = Graphics::BufferManager::CreateColorBuffer(L"GFL2 PostWorkHDR", w, h, Deferred::LinearHDRBufferFormat, 1, &hdrClear);

		uint32_t bw = hw;
		uint32_t bh = hh;
		for (uint32_t i = 0; i < kBloomMipCount; ++i)
		{
			wchar_t name[64] = {};
			std::swprintf(name, 64, L"GFL2 BloomMip%u", i);
			BloomMips[i] = Graphics::BufferManager::CreateColorBuffer(name, bw, bh, DXGI_FORMAT_R11G11B10_FLOAT, 1, &hdrClear);
			bw = (std::max)(bw / 2u, 1u);
			bh = (std::max)(bh / 2u, 1u);
		}
	}

	void GFL2PostTargets::Destroy()
	{
		Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 SSAORaw", SSAORaw);
		Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 TAAHist0", TAAHistory[0]);
		Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 TAAHist1", TAAHistory[1]);
		Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 PostWorkHDR", PostWorkHDR);
		for (uint32_t i = 0; i < kBloomMipCount; ++i)
		{
			wchar_t name[64] = {};
			std::swprintf(name, 64, L"GFL2 BloomMip%u", i);
			Graphics::BufferManager::ReleaseColorBuffer(name, BloomMips[i]);
		}
		Width  = 0;
		Height = 0;
	}

	namespace
	{
		D3D12_BLEND_DESC MakeSSAOCompositeMultiplyBlend()
		{
			D3D12_BLEND_DESC bd = Graphics::BlendDisable;
			bd.RenderTarget[0].BlendEnable           = TRUE;
			bd.RenderTarget[0].SrcBlend              = D3D12_BLEND_DEST_COLOR;
			bd.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
			bd.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
			bd.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
			bd.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			return bd;
		}

		void InitFsPSO(
			GraphicsPSO& pso,
			const wchar_t* name,
			Renderer::RenderServices& services,
			const void* ps,
			size_t psSize,
			DXGI_FORMAT rtvFmt,
			const D3D12_BLEND_DESC& blend)
		{
			pso = GraphicsPSO(name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			pso.SetRasterizerState(Graphics::RasterizerFullScreen);
			pso.SetBlendState(blend);
			pso.SetDepthStencilState(Graphics::DepthStateDisabled);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(0, nullptr);
			pso.SetRenderTargetFormat(rtvFmt, DXGI_FORMAT_UNKNOWN);
			pso.SetVertexShader(FullScreenVS_cso, sizeof(FullScreenVS_cso));
			pso.SetPixelShader(ps, psSize);
			pso.Finalize();
		}

		bool BuildRawCompute(ID3D12RootSignature* rs, const wchar_t* name, const std::vector<uint8_t>& cs,
			Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso, std::string& outErr)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = rs;
			d.CS             = { cs.data(), cs.size() };
			const HRESULT hr = Graphics::g_Device->CreateComputePipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "PostProcess CreateComputePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		}

		bool BuildRawFs(ID3D12RootSignature* rs, const wchar_t* name, const std::vector<uint8_t>& ps,
			DXGI_FORMAT rtvFmt, const D3D12_BLEND_DESC& blend,
			Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso, std::string& outErr)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { FullScreenVS_cso, sizeof(FullScreenVS_cso) };
			d.PS                    = { ps.data(), ps.size() };
			d.BlendState            = blend;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerFullScreen;
			d.DepthStencilState     = Graphics::DepthStateDisabled;
			d.InputLayout           = { nullptr, 0 };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets      = 1;
			d.RTVFormats[0]         = rtvFmt;
			d.DSVFormat             = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count      = 1;
			const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "PostProcess CreateGraphicsPSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		}
	}

	bool GFL2PostProcessPass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M6] GFL2PostProcessPass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_SSAOCS.assign(GFL2SSAOCS_cso, GFL2SSAOCS_cso + sizeof(GFL2SSAOCS_cso));
		m_SSAOCompPS.assign(GFL2SSAOCompositePS_cso, GFL2SSAOCompositePS_cso + sizeof(GFL2SSAOCompositePS_cso));
		m_TAACS.assign(GFL2TAACS_cso, GFL2TAACS_cso + sizeof(GFL2TAACS_cso));
		m_BloomPS.assign(GFL2BloomPS_cso, GFL2BloomPS_cso + sizeof(GFL2BloomPS_cso));
		m_UberPostPS.assign(GFL2UberPostLUTPS_cso, GFL2UberPostLUTPS_cso + sizeof(GFL2UberPostLUTPS_cso));

		m_PsoSSAOCompute = ComputePSO(L"GFL2 SSAO Compute PSO");
		m_PsoSSAOCompute.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_PsoSSAOCompute.SetComputeShader(m_SSAOCS.data(), m_SSAOCS.size());
		m_PsoSSAOCompute.Finalize();

		m_PsoTAACompute = ComputePSO(L"GFL2 TAA Compute PSO");
		m_PsoTAACompute.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_PsoTAACompute.SetComputeShader(m_TAACS.data(), m_TAACS.size());
		m_PsoTAACompute.Finalize();

		const D3D12_BLEND_DESC blendMul = MakeSSAOCompositeMultiplyBlend();
		InitFsPSO(m_PsoSSAOComposite,   L"GFL2 SSAO Composite PSO",   services, m_SSAOCompPS.data(), m_SSAOCompPS.size(), Deferred::LinearHDRBufferFormat, blendMul);
		InitFsPSO(m_PsoBloomOpaque,     L"GFL2 Bloom Opaque PSO",     services, m_BloomPS.data(),    m_BloomPS.size(),    DXGI_FORMAT_R11G11B10_FLOAT,    Graphics::BlendDisable);
		InitFsPSO(m_PsoBloomUpsampleAdd,L"GFL2 Bloom Upsample PSO",   services, m_BloomPS.data(),    m_BloomPS.size(),    DXGI_FORMAT_R11G11B10_FLOAT,    Graphics::BlendAdditive);
		InitFsPSO(m_PsoUberPostLUT,     L"GFL2 UberPost LUT PSO",     services, m_UberPostPS.data(), m_UberPostPS.size(), Deferred::LinearHDRBufferFormat, Graphics::BlendDisable);

		ResetHistory();
		m_Initialized = true;
		return true;
	}

	void GFL2PostProcessPass::ResetHistory()
	{
		m_TaaHistoryValid        = false;
		m_TaaReadSlot            = 0;
		m_TaaJitterPhase         = 0;
		m_PrevUnjitteredViewProj = Math::Matrix4(Math::kIdentity);
	}

	void GFL2PostProcessPass::Shutdown()
	{
		m_HotSSAOCompute.Reset();
		m_HotTAACompute.Reset();
		m_HotSSAOComposite.Reset();
		m_HotBloomOpaque.Reset();
		m_HotBloomUpsampleAdd.Reset();
		m_HotUberPostLUT.Reset();

		m_SSAOCS.clear();
		m_SSAOCompPS.clear();
		m_TAACS.clear();
		m_BloomPS.clear();
		m_UberPostPS.clear();
		ResetHistory();
		m_Initialized = false;
	}

	bool GFL2PostProcessPass::RebuildPSOs(Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr)
		{
			outErr = "GFL2PostProcessPass not initialized";
			return false;
		}
		ID3D12RootSignature* gRs = services.GetBindlessGraphicsRootSignature().GetSignature();
		ID3D12RootSignature* cRs = services.GetBindlessComputeRootSignature().GetSignature();

		const D3D12_BLEND_DESC blendMul = MakeSSAOCompositeMultiplyBlend();
		Microsoft::WRL::ComPtr<ID3D12PipelineState> cSsao, cTaa, cComp, cBlO, cBlA, cLut;
		if (!BuildRawCompute(cRs, L"GFL2 SSAO Hot", m_SSAOCS, cSsao, outErr) ||
			!BuildRawCompute(cRs, L"GFL2 TAA Hot",  m_TAACS,  cTaa,  outErr) ||
			!BuildRawFs(gRs, L"GFL2 SSAOComp Hot",  m_SSAOCompPS, Deferred::LinearHDRBufferFormat, blendMul, cComp, outErr) ||
			!BuildRawFs(gRs, L"GFL2 BloomOpq Hot",  m_BloomPS,    DXGI_FORMAT_R11G11B10_FLOAT, Graphics::BlendDisable, cBlO, outErr) ||
			!BuildRawFs(gRs, L"GFL2 BloomAdd Hot",  m_BloomPS,    DXGI_FORMAT_R11G11B10_FLOAT, Graphics::BlendAdditive, cBlA, outErr) ||
			!BuildRawFs(gRs, L"GFL2 UberLUT Hot",   m_UberPostPS, Deferred::LinearHDRBufferFormat, Graphics::BlendDisable, cLut, outErr))
		{
			return false;
		}

		auto RetireSwap = [&](Microsoft::WRL::ComPtr<ID3D12PipelineState>& slot, Microsoft::WRL::ComPtr<ID3D12PipelineState>&& cand)
		{
			if (slot != nullptr)
			{
				Microsoft::WRL::ComPtr<IUnknown> unk;
				slot.As(&unk);
				services.GetReleaseQueue().Enqueue(std::move(unk));
			}
			slot = std::move(cand);
		};

		RetireSwap(m_HotSSAOCompute,      std::move(cSsao));
		RetireSwap(m_HotTAACompute,       std::move(cTaa));
		RetireSwap(m_HotSSAOComposite,    std::move(cComp));
		RetireSwap(m_HotBloomOpaque,      std::move(cBlO));
		RetireSwap(m_HotBloomUpsampleAdd, std::move(cBlA));
		RetireSwap(m_HotUberPostLUT,      std::move(cLut));
		return true;
	}

	void GFL2PostProcessPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		auto Reg = [&](const char* id, const wchar_t* path, const wchar_t* entry, const wchar_t* prof, std::vector<uint8_t>* target)
		{
			Graphics::ShaderHotReloadDesc d;
			d.Id            = id;
			d.SourceRelPath = path;
			d.EntryPoint    = entry;
			d.TargetProfile = prof;
			d.RebuildPipeline = [this, &services, target](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
			{
				const std::vector<uint8_t> backup = *target;
				*target = dxil;
				if (!RebuildPSOs(services, outErr))
				{
					*target = backup;
					return false;
				}
				return true;
			};
			reloader.Register(std::move(d));
		};

		Reg("GFL2SSAOCS",          L"Engine/Shader/08_Character/GFL2SSAOCS.hlsl",          L"MainCS", L"cs_6_6", &m_SSAOCS);
		Reg("GFL2SSAOCompositePS", L"Engine/Shader/08_Character/GFL2SSAOCompositePS.hlsl", L"MainPS", L"ps_6_6", &m_SSAOCompPS);
		Reg("GFL2TAACS",           L"Engine/Shader/08_Character/GFL2TAACS.hlsl",           L"MainCS", L"cs_6_6", &m_TAACS);
		Reg("GFL2BloomPS",         L"Engine/Shader/08_Character/GFL2BloomPS.hlsl",         L"MainPS", L"ps_6_6", &m_BloomPS);
		Reg("GFL2UberPostLUTPS",   L"Engine/Shader/08_Character/GFL2UberPostLUTPS.hlsl",   L"MainPS", L"ps_6_6", &m_UberPostPS);
	}

	void GFL2PostProcessPass::ExecuteSSAO(
		GraphicsContext& context,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Renderer::RenderParams& params,
		GFL2PostTargets& targets,
		ColorBuffer& linearHDR,
		ColorBuffer& auxMask,
		DepthBuffer& sceneDepth,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services)
	{
		if (!m_Initialized || !targets.SSAORaw)
			return;

		context.PIXBeginEvent(L"8. GFL2SSAOPass (ReverseZ + CharExempt)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		// 8.1 Compute Half-Res SSAO
		context.PIXBeginEvent(L"SSAO_Compute");
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(auxMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*targets.SSAORaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		context.FlushResourceBarriers();

		const D3D12_CPU_DESCRIPTOR_HANDLE handles[3] = {
			sceneDepth.GetDepthSRV(),
			auxMask.GetSRV(),
			targets.SSAORaw->GetUAV(),
		};
		const Renderer::BindlessAllocation staged = services.GetDescriptorHeap().StageTransientDescriptors(handles, 3);

		ComputeContext& cc = context.GetComputeContext();
		cc.SetRootSignature(services.GetBindlessComputeRootSignature());
		cc.SetPipelineStateRaw(m_HotSSAOCompute ? m_HotSSAOCompute.Get() : m_PsoSSAOCompute.GetPipelineStateObject());
		cc.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);

		::GFL2::GFL2RootConstants rc = {};
		rc.AuxTex0SrvIdx = staged.HeapIndex + 0u;
		rc.AuxTex1SrvIdx = staged.HeapIndex + 1u;
		rc.AuxUavIdx     = staged.HeapIndex + 2u;
		rc.PassSubMode   = params.GFL2BypassSSAOExempt ? 1u : 0u;
		rc.PassParam0    = 1.2f;
		rc.PassParam1    = 0.04f;
		cc.SetConstantArray(0, 16, &rc);
		cc.Dispatch2D(targets.SSAORaw->GetWidth(), targets.SSAORaw->GetHeight(), 8, 8);
		context.InsertUAVBarrier(*targets.SSAORaw);
		context.PIXEndEvent();

		// 8.2 Bilateral Blur & Multiply Composite (AuxMask.a < 0.495)
		context.PIXBeginEvent(L"SSAO_BilateralComposite");
		context.TransitionResource(*targets.SSAORaw, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(auxMask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.FlushResourceBarriers();

		const D3D12_CPU_DESCRIPTOR_HANDLE psHandles[3] = {
			targets.SSAORaw->GetSRV(),
			sceneDepth.GetDepthSRV(),
			auxMask.GetSRV(),
		};
		const Renderer::BindlessAllocation psStaged = services.GetDescriptorHeap().StageTransientDescriptors(psHandles, 3);

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetPipelineStateRaw(m_HotSSAOComposite ? m_HotSSAOComposite.Get() : m_PsoSSAOComposite.GetPipelineStateObject());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTarget(linearHDR.GetRTV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		::GFL2::GFL2RootConstants psRc = {};
		psRc.MainShadowMapSrvIdx = psStaged.HeapIndex + 0u;
		psRc.AuxTex0SrvIdx       = psStaged.HeapIndex + 1u;
		psRc.AuxTex1SrvIdx       = psStaged.HeapIndex + 2u;
		psRc.PassSubMode         = params.GFL2BypassSSAOExempt ? 1u : 0u;
		psRc.PassParam0          = params.GFL2SSAOIntensity;
		context.SetConstantArray(0, 16, &psRc);
		context.Draw(3, 0);
		context.PIXEndEvent();

		context.PIXEndEvent();
	}

	void GFL2PostProcessPass::PrepareTaaFrame(
		const Scene::Camera::Camera* camera,
		uint32_t width,
		uint32_t height,
		bool taaEnabled,
		::GFL2::GFL2FrameConstants& inOutFrameCB,
		Math::Matrix4& outUnjitteredViewProj)
	{
		outUnjitteredViewProj = inOutFrameCB.ViewProj;

		// TAA 开关任意方向切换时立刻清空历史，防止启用瞬间或重新开启时混入陈旧帧造成拖糊
		if (taaEnabled != m_PrevTaaEnabled)
		{
			ResetHistory();
			m_PrevTaaEnabled = taaEnabled;
		}

		if (!taaEnabled || camera == nullptr || width == 0 || height == 0)
			return;

		const DirectX::XMFLOAT2 jitterPx = TAAMath::ComputeHaltonJitterPixels(m_TaaJitterPhase, 1.0f);
		const Math::Matrix4 jitteredProj = TAAMath::ApplyJitterToProjMatrixLH(
			camera->GetProjMatrix(), width, height, jitterPx.x, jitterPx.y);
		inOutFrameCB.Proj        = jitteredProj;
		inOutFrameCB.ViewProj    = inOutFrameCB.View * jitteredProj;
		inOutFrameCB.InvViewProj = Math::Invert(inOutFrameCB.ViewProj);
	}

	void GFL2PostProcessPass::ExecuteTAA(
		GraphicsContext& context,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Math::Matrix4& unjitteredViewProj,
		const Renderer::RenderParams& params,
		GFL2PostTargets& targets,
		ColorBuffer& linearHDR,
		DepthBuffer& sceneDepth,
		Renderer::RenderServices& services)
	{
		(void)params;
		if (!m_Initialized || !targets.TAAHistory[0] || !targets.TAAHistory[1])
			return;

		// 若相机切镜/大幅跳变，自动清空 TAA 历史防拖影
		const float dx = frameCB.CameraPosAndNear.x - m_PrevCamPos[0];
		const float dy = frameCB.CameraPosAndNear.y - m_PrevCamPos[1];
		const float dz = frameCB.CameraPosAndNear.z - m_PrevCamPos[2];
		if ((dx * dx + dy * dy + dz * dz) > 0.25f)
			m_TaaHistoryValid = false;

		const uint32_t writeSlot = 1u - m_TaaReadSlot;
		ColorBuffer& histRead  = *targets.TAAHistory[m_TaaReadSlot];
		ColorBuffer& histWrite = *targets.TAAHistory[writeSlot];

		context.PIXBeginEvent(L"9. GFL2TAAPass (ReverseZ ClosestDepth)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(histRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(histWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		context.FlushResourceBarriers();

		const D3D12_CPU_DESCRIPTOR_HANDLE handles[4] = {
			linearHDR.GetSRV(),
			histRead.GetSRV(),
			sceneDepth.GetDepthSRV(),
			histWrite.GetUAV(),
		};
		const Renderer::BindlessAllocation staged = services.GetDescriptorHeap().StageTransientDescriptors(handles, 4);

		ComputeContext& cc = context.GetComputeContext();
		cc.SetRootSignature(services.GetBindlessComputeRootSignature());
		cc.SetPipelineStateRaw(m_HotTAACompute ? m_HotTAACompute.Get() : m_PsoTAACompute.GetPipelineStateObject());
		cc.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);

		struct alignas(16) TAACB
		{
			Math::Matrix4     CurUnjitteredViewProj;
			Math::Matrix4     PrevUnjitteredViewProj;
			DirectX::XMFLOAT4 Params;
		} taaCB;
		taaCB.CurUnjitteredViewProj  = unjitteredViewProj;
		taaCB.PrevUnjitteredViewProj = m_PrevUnjitteredViewProj;
		taaCB.Params                 = { m_TaaHistoryValid ? 1.0f : 0.0f, 0.08f, 0.35f, 1.15f };
		cc.SetDynamicConstantBufferView(2, sizeof(taaCB), &taaCB);

		::GFL2::GFL2RootConstants rc = {};
		rc.AuxTex0SrvIdx       = staged.HeapIndex + 0u;
		rc.AuxTex1SrvIdx       = staged.HeapIndex + 1u;
		rc.MainShadowMapSrvIdx = staged.HeapIndex + 2u;
		rc.AuxUavIdx           = staged.HeapIndex + 3u;
		cc.SetConstantArray(0, 16, &rc);
		cc.Dispatch2D(linearHDR.GetWidth(), linearHDR.GetHeight(), 8, 8);
		context.InsertUAVBarrier(histWrite);

		context.CopyBuffer(linearHDR, histWrite);
		context.PIXEndEvent();

		m_TaaReadSlot            = writeSlot;
		m_TaaHistoryValid        = true;
		++m_TaaJitterPhase;
		m_PrevUnjitteredViewProj = unjitteredViewProj;
		m_PrevCamPos[0]   = frameCB.CameraPosAndNear.x;
		m_PrevCamPos[1]   = frameCB.CameraPosAndNear.y;
		m_PrevCamPos[2]   = frameCB.CameraPosAndNear.z;
	}

	void GFL2PostProcessPass::ExecuteBloom(
		GraphicsContext& context,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Renderer::RenderParams& params,
		GFL2PostTargets& targets,
		ColorBuffer& linearHDR,
		Renderer::RenderServices& services)
	{
		if (!m_Initialized || !targets.BloomMips[0])
			return;

		context.PIXBeginEvent(L"10. BloomPass (Karis Prefilter + Pyramid)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		auto RunBloomMip = [&](ColorBuffer& src, ColorBuffer& dst, uint32_t subMode, float p0, float p1, bool additive)
		{
			context.TransitionResource(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context.TransitionResource(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.FlushResourceBarriers();

			const uint32_t srcSrv = services.GetDescriptorHeap().StageTransientDescriptor(src.GetSRV()).HeapIndex;
			const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(dst.GetWidth()), static_cast<float>(dst.GetHeight()), 0.0f, 1.0f };
			const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(dst.GetWidth()), static_cast<LONG>(dst.GetHeight()) };

			context.SetViewportAndScissor(vp, sc);
			context.SetRenderTarget(dst.GetRTV());
			context.SetPipelineStateRaw(additive
				? (m_HotBloomUpsampleAdd ? m_HotBloomUpsampleAdd.Get() : m_PsoBloomUpsampleAdd.GetPipelineStateObject())
				: (m_HotBloomOpaque      ? m_HotBloomOpaque.Get()      : m_PsoBloomOpaque.GetPipelineStateObject()));

			::GFL2::GFL2RootConstants rc = {};
			rc.AuxTex0SrvIdx = srcSrv;
			rc.PassSubMode   = subMode;
			rc.PassParam0    = p0;
			rc.PassParam1    = p1;
			rc.PassParam2    = 1.0f / static_cast<float>((std::max)(dst.GetWidth(), 1u));
			rc.PassParam3    = 1.0f / static_cast<float>((std::max)(dst.GetHeight(), 1u));
			context.SetConstantArray(0, 16, &rc);
			context.Draw(3, 0);
		};

		// 10.1 Prefilter (EID 4601)
		RunBloomMip(linearHDR, *targets.BloomMips[0], 0u, params.GFL2BloomThreshold, 1.8f, false);

		// 10.2 Downsample (Mip0 -> Mip1 -> Mip2 -> Mip3)
		for (uint32_t m = 1; m < GFL2PostTargets::kBloomMipCount; ++m)
			RunBloomMip(*targets.BloomMips[m - 1], *targets.BloomMips[m], 1u, 0.0f, 0.0f, false);

		// 10.3 Additive Upsample (Mip3 -> Mip2 -> Mip1 -> Mip0)
		static constexpr float kScatter[3] = { 0.55f, 0.65f, 0.75f };
		for (int m = static_cast<int>(GFL2PostTargets::kBloomMipCount) - 1; m >= 1; --m)
			RunBloomMip(*targets.BloomMips[m], *targets.BloomMips[m - 1], 2u, kScatter[m - 1], 0.0f, true);

		context.PIXEndEvent();
	}

	void GFL2PostProcessPass::ExecuteUberPostLUT(
		GraphicsContext& context,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Renderer::RenderParams& params,
		GFL2PostTargets& targets,
		ColorBuffer& linearHDR,
		uint32_t lut3DSrvIdx,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services)
	{
		if (!m_Initialized || !targets.PostWorkHDR)
			return;

		context.PIXBeginEvent(L"11. UberPostLUTPass (Bloom + Vignette + 7281 3D LUT)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (params.GFL2EnableBloom && targets.BloomMips[0])
			context.TransitionResource(*targets.BloomMips[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*targets.PostWorkHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.FlushResourceBarriers();

		const uint32_t hdrSrv   = services.GetDescriptorHeap().StageTransientDescriptor(linearHDR.GetSRV()).HeapIndex;
		const uint32_t bloomSrv = (params.GFL2EnableBloom && targets.BloomMips[0])
			? services.GetDescriptorHeap().StageTransientDescriptor(targets.BloomMips[0]->GetSRV()).HeapIndex
			: ::GFL2::kInvalidBindlessIdx;

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetPipelineStateRaw(m_HotUberPostLUT ? m_HotUberPostLUT.Get() : m_PsoUberPostLUT.GetPipelineStateObject());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTarget(targets.PostWorkHDR->GetRTV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		::GFL2::GFL2RootConstants rc = {};
		rc.AuxTex0SrvIdx       = hdrSrv;
		rc.AuxTex1SrvIdx       = bloomSrv;
		rc.MainShadowMapSrvIdx = params.GFL2Enable3DLUT ? lut3DSrvIdx : ::GFL2::kInvalidBindlessIdx;
		rc.PassParam0          = params.GFL2EnableBloom ? (params.GFL2BloomIntensity * 0.14f) : 0.0f;
		rc.PassParam1          = params.GFL2VignetteIntensity;
		rc.PassParam2          = params.GFL2Enable3DLUT ? params.GFL2LUTIntensity : 0.0f;
		context.SetConstantArray(0, 16, &rc);
		context.Draw(3, 0);

		context.CopyBuffer(linearHDR, *targets.PostWorkHDR);
		context.PIXEndEvent();
	}
}
