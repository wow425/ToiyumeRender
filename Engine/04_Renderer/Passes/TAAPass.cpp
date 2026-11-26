// ============================================================================
// 模块：TAAPass（SM 6.6 Bindless 时域抗锯齿 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/TAAPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 TAABindlessCS_cso 的唯一包含点
#include "04_PostProcess/TAABindlessCS.h"

#include <algorithm>
#include <cstdio>

namespace Renderer
{
	bool TAAPass::Initialize(RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[WP-3C][TAAPass] 设备不支持 SM 6.6 Bindless，TAAPass 不可用\n");
			return false;
		}

		m_ComputePSO = ComputePSO(L"TAAPass : Resolve SM6.6 Bindless PSO");
		m_ComputePSO.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_ComputePSO.SetComputeShader(TAABindlessCS_cso, sizeof(TAABindlessCS_cso));
		m_ComputePSO.Finalize();

		ResetHistory();
		m_Initialized = (m_ComputePSO.GetPipelineStateObject() != nullptr);
		return m_Initialized;
	}

	void TAAPass::Shutdown()
	{
		m_HotComputePSO.Reset();
		ResetHistory();
		m_Initialized = false;
	}

	void TAAPass::ResetHistory()
	{
		m_HistoryValid     = false;
		m_JitterPhaseIndex = 0;
		m_HistoryWriteSlot = 0;
		m_PrevUnjitteredViewProj = Math::Matrix4(Math::kIdentity);
	}

	TAAPass::FrameJitterState TAAPass::PrepareFrame(const Scene::Camera::Camera& camera,
		uint32_t width, uint32_t height, float jitterScale)
	{
		FrameJitterState st;
		const float clampedScale = (std::clamp)(jitterScale, 0.0f, 2.0f);
		st.JitterPixels        = TAAMath::ComputeHaltonJitterPixels(m_JitterPhaseIndex, clampedScale);
		st.JitteredProj        = TAAMath::ApplyJitterToProjMatrixRH(
			camera.GetProjMatrix(), width, height, st.JitterPixels.x, st.JitterPixels.y);
		st.UnjitteredViewProj  = camera.GetViewProjMatrix();
		st.JitteredViewProj    = camera.GetViewMatrix() * st.JitteredProj;
		st.InvJitteredViewProj = Math::Invert(st.JitteredViewProj);
		return st;
	}

	void TAAPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		Graphics::ShaderHotReloadDesc desc;
		desc.Id            = "TAABindlessCS";
		desc.SourceRelPath = L"Engine/Shader/04_PostProcess/TAABindlessCS.hlsl";
		desc.EntryPoint    = L"MainCS";
		desc.TargetProfile = L"cs_6_6";
		desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
			return HotReloadComputeShader(dxil, services, outErr);
		};
		reloader.Register(std::move(desc));
	}

	bool TAAPass::HotReloadComputeShader(const std::vector<uint8_t>& dxil, RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "TAAPass not initialized or empty DXIL";
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = services.GetBindlessComputeRootSignature().GetSignature();
		desc.CS             = { dxil.data(), dxil.size() };

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate;
		const HRESULT hr = Graphics::g_Device->CreateComputePipelineState(&desc, TY_IID_PPV_ARGS(&candidate));
		if (FAILED(hr) || candidate == nullptr)
		{
			char buf[128] = {};
			std::snprintf(buf, sizeof(buf), "TAAPass CreateComputePipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(L"TAAPass HotReload Compute PSO");

		if (m_HotComputePSO != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> oldUnk;
			m_HotComputePSO.As(&oldUnk);
			services.GetReleaseQueue().Enqueue(std::move(oldUnk));
		}
		m_HotComputePSO = std::move(candidate);
		return true;
	}

	void TAAPass::Execute(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const FrameJitterState& jitterState,
		const RenderParams& params,
		uint32_t width,
		uint32_t height,
		RenderServices& services)
	{
		if (!m_Initialized || width == 0 || height == 0)
			return;

		ColorBuffer* currentHdr  = blackboard.GetColorBuffer(FrameResourceKey::LinearHDRColor);
		ColorBuffer* historyPrev = blackboard.GetColorBuffer(FrameResourceKey::TAAHistoryPrev);
		ColorBuffer* historyOut  = blackboard.GetColorBuffer(FrameResourceKey::TAAOutput);
		DepthBuffer* sceneDepth  = blackboard.Core.SceneDepth;
		if (currentHdr == nullptr || historyPrev == nullptr || historyOut == nullptr || sceneDepth == nullptr)
			return;

		context.PIXBeginEvent(L"TAAPass::Resolve");

		// [契约 1] 经 4 分区单堆 Zone B 瞬态暂存 4 个描述符（CurrentHDR / HistoryPrev / Depth / OutputUAV），零切堆
		const D3D12_CPU_DESCRIPTOR_HANDLE handles[4] = {
			currentHdr->GetSRV(),
			historyPrev->GetSRV(),
			sceneDepth->GetDepthSRV(),
			historyOut->GetUAV(),
		};
		const BindlessAllocation staged = services.GetDescriptorHeap().StageTransientDescriptors(handles, 4);
		const uint32_t rootConsts[4] = {
			staged.HeapIndex + 0u,
			staged.HeapIndex + 1u,
			staged.HeapIndex + 2u,
			staged.HeapIndex + 3u,
		};

		Constants cb = {};
		cb.InvJitteredViewProj    = jitterState.InvJitteredViewProj;
		cb.CurUnjitteredViewProj  = jitterState.UnjitteredViewProj;
		cb.PrevUnjitteredViewProj = m_HistoryValid ? m_PrevUnjitteredViewProj : jitterState.UnjitteredViewProj;
		cb.ScreenSize             = DirectX::XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
		cb.InvScreenSize          = DirectX::XMFLOAT2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
		cb.BlendFactor            = (std::clamp)(params.TAABlendFactor, 0.01f, 1.0f);
		cb.VarianceGamma          = (std::clamp)(params.TAAVarianceGamma, 0.25f, 4.0f);
		cb.HistoryValid           = m_HistoryValid ? 1u : 0u;

		ComputeContext& computeCtx = context.GetComputeContext();
		computeCtx.SetRootSignature(services.GetBindlessComputeRootSignature());
		computeCtx.SetPipelineStateRaw(m_HotComputePSO ? m_HotComputePSO.Get() : m_ComputePSO.GetPipelineStateObject());
		computeCtx.SetConstantArray(0, 4, rootConsts);
		computeCtx.SetDynamicConstantBufferView(2, sizeof(cb), &cb);
		computeCtx.Dispatch2D(width, height, 8, 8);

		context.PIXEndEvent();

		// 推进历史状态：下帧读取本次写入的 Ping-Pong 槽并对比本次无抖动视图投影矩阵
		m_PrevUnjitteredViewProj = jitterState.UnjitteredViewProj;
		m_HistoryValid           = true;
		m_HistoryWriteSlot      ^= 1u;
		++m_JitterPhaseIndex;
	}
}
