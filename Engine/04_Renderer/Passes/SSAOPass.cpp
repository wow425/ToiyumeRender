// ============================================================================
// 模块：SSAOPass（SM 6.6 Bindless SSAO + 双边模糊 · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/SSAOPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 SSAOComputeCS_cso / SSAOBlurCS_cso 的唯一包含点
#include "04_PostProcess/SSAOComputeCS.h"
#include "04_PostProcess/SSAOBlurCS.h"

#include <algorithm>
#include <cstdio>

namespace Renderer
{
	bool SSAOPass::Initialize(RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[WP-3C][SSAOPass] 设备不支持 SM 6.6 Bindless，SSAOPass 不可用\n");
			return false;
		}

		m_ComputePSO = ComputePSO(L"SSAOPass : Compute SM6.6 Bindless PSO");
		m_ComputePSO.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_ComputePSO.SetComputeShader(SSAOComputeCS_cso, sizeof(SSAOComputeCS_cso));
		m_ComputePSO.Finalize();

		m_BlurPSO = ComputePSO(L"SSAOPass : Bilateral Blur SM6.6 Bindless PSO");
		m_BlurPSO.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_BlurPSO.SetComputeShader(SSAOBlurCS_cso, sizeof(SSAOBlurCS_cso));
		m_BlurPSO.Finalize();

		m_Initialized =
			(m_ComputePSO.GetPipelineStateObject() != nullptr) &&
			(m_BlurPSO.GetPipelineStateObject() != nullptr);
		return m_Initialized;
	}

	void SSAOPass::Shutdown()
	{
		m_HotComputePSO.Reset();
		m_HotBlurPSO.Reset();
		m_Initialized = false;
	}

	void SSAOPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "SSAOComputeCS";
			desc.SourceRelPath = L"Engine/Shader/04_PostProcess/SSAOComputeCS.hlsl";
			desc.EntryPoint    = L"MainCS";
			desc.TargetProfile = L"cs_6_6";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
				return HotReloadComputeShader(dxil, /*isBlur=*/false, services, outErr);
			};
			reloader.Register(std::move(desc));
		}
		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "SSAOBlurCS";
			desc.SourceRelPath = L"Engine/Shader/04_PostProcess/SSAOBlurCS.hlsl";
			desc.EntryPoint    = L"MainCS";
			desc.TargetProfile = L"cs_6_6";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
				return HotReloadComputeShader(dxil, /*isBlur=*/true, services, outErr);
			};
			reloader.Register(std::move(desc));
		}
	}

	bool SSAOPass::HotReloadComputeShader(const std::vector<uint8_t>& dxil, bool isBlur,
		RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "SSAOPass not initialized or empty DXIL";
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
			std::snprintf(buf, sizeof(buf), "SSAOPass CreateComputePipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(isBlur ? L"SSAOPass HotReload Blur PSO" : L"SSAOPass HotReload Compute PSO");

		// [契约 7.2 事务化替换] 旧 PSO 可能仍被在途命令列表引用，必须入帧尾延迟释放队列
		auto& slot = isBlur ? m_HotBlurPSO : m_HotComputePSO;
		if (slot != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> oldUnk;
			slot.As(&oldUnk);
			services.GetReleaseQueue().Enqueue(std::move(oldUnk));
		}
		slot = std::move(candidate);
		return true;
	}

	void SSAOPass::ExecuteCompute(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const Scene::Camera::Camera& camera,
		const RenderParams& params,
		uint32_t width,
		uint32_t height,
		uint32_t frameIndex,
		const Math::Matrix4* invViewProjOverride,
		const Math::Matrix4* projOverride,
		RenderServices& services) const
	{
		if (!m_Initialized || width == 0 || height == 0)
			return;

		DepthBuffer* sceneDepth  = blackboard.Core.SceneDepth;
		ColorBuffer* gbufNormal  = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Normal);
		ColorBuffer* ssaoRaw     = blackboard.GetColorBuffer(FrameResourceKey::SSAORaw);
		if (sceneDepth == nullptr || gbufNormal == nullptr || ssaoRaw == nullptr)
			return;

		context.PIXBeginEvent(L"SSAOPass::Compute");

		// [契约 1 / 契约 4] 优先直接消费 FrameBlackboard::PrimarySurface 已暂存的 Bindless SRV 下标
		// （Deferred 与 DXR 同构），未就绪时回退为按需暂存；仅需为本 Pass 输出 SSAORaw UAV 暂存 1 槽。
		uint32_t depthSrvIdx  = kInvalidBindlessIndex;
		uint32_t normalSrvIdx = kInvalidBindlessIndex;
		uint32_t outUavIdx    = kInvalidBindlessIndex;
		if (blackboard.PrimarySurface.IsBindlessReady())
		{
			depthSrvIdx  = blackboard.PrimarySurface.Depth.SrvBindlessIdx;
			normalSrvIdx = blackboard.PrimarySurface.Normal.SrvBindlessIdx;
			outUavIdx    = services.GetDescriptorHeap().StageTransientDescriptor(ssaoRaw->GetUAV()).HeapIndex;
		}
		else
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE handles[3] = {
				sceneDepth->GetDepthSRV(),
				gbufNormal->GetSRV(),
				ssaoRaw->GetUAV(),
			};
			const BindlessAllocation staged = services.GetDescriptorHeap().StageTransientDescriptors(handles, 3);
			depthSrvIdx  = staged.HeapIndex + 0u;
			normalSrvIdx = staged.HeapIndex + 1u;
			outUavIdx    = staged.HeapIndex + 2u;
		}
		const uint32_t rootConsts[4] = {
			depthSrvIdx,
			normalSrvIdx,
			outUavIdx,
			(std::clamp)(params.SSAOSampleCount, 4u, 32u),
		};

		ComputeConstants cb = {};
		cb.InvViewProj   = (invViewProjOverride != nullptr) ? *invViewProjOverride : camera.GetInViewProjMatrix();
		cb.ViewMatrix    = camera.GetViewMatrix();
		cb.ProjMatrix    = (projOverride != nullptr) ? *projOverride : camera.GetProjMatrix();
		cb.ScreenSize    = DirectX::XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
		cb.InvScreenSize = DirectX::XMFLOAT2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
		cb.Radius        = (std::max)(params.SSAORadius, 0.01f);
		cb.Bias          = (std::max)(params.SSAOBias, 0.0f);
		cb.Power         = (std::max)(params.SSAOPower, 0.1f);
		cb.FrameIndex    = frameIndex;

		ComputeContext& computeCtx = context.GetComputeContext();
		computeCtx.SetRootSignature(services.GetBindlessComputeRootSignature());
		computeCtx.SetPipelineStateRaw(m_HotComputePSO ? m_HotComputePSO.Get() : m_ComputePSO.GetPipelineStateObject());
		computeCtx.SetConstantArray(0, 4, rootConsts);
		computeCtx.SetDynamicConstantBufferView(2, sizeof(cb), &cb);
		computeCtx.Dispatch2D(width, height, 8, 8);

		context.PIXEndEvent();
	}

	void SSAOPass::ExecuteBlur(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const Scene::Camera::Camera& camera,
		uint32_t width,
		uint32_t height,
		RenderServices& services) const
	{
		if (!m_Initialized || width == 0 || height == 0)
			return;

		ColorBuffer* ssaoRaw    = blackboard.GetColorBuffer(FrameResourceKey::SSAORaw);
		ColorBuffer* ssaoBlur   = blackboard.GetColorBuffer(FrameResourceKey::SSAOMap);
		DepthBuffer* sceneDepth = blackboard.Core.SceneDepth;
		ColorBuffer* gbufNormal = blackboard.GetColorBuffer(FrameResourceKey::GBuffer_Normal);
		if (ssaoRaw == nullptr || ssaoBlur == nullptr || sceneDepth == nullptr || gbufNormal == nullptr)
			return;

		context.PIXBeginEvent(L"SSAOPass::BilateralBlur");

		const D3D12_CPU_DESCRIPTOR_HANDLE handles[4] = {
			ssaoRaw->GetSRV(),
			sceneDepth->GetDepthSRV(),
			gbufNormal->GetSRV(),
			ssaoBlur->GetUAV(),
		};
		const BindlessAllocation staged = services.GetDescriptorHeap().StageTransientDescriptors(handles, 4);
		const uint32_t rootConsts[4] = {
			staged.HeapIndex + 0u,
			staged.HeapIndex + 1u,
			staged.HeapIndex + 2u,
			staged.HeapIndex + 3u,
		};

		BlurConstants cb = {};
		cb.ScreenSize     = DirectX::XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
		cb.InvScreenSize  = DirectX::XMFLOAT2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
		cb.NearClip       = (std::max)(camera.GetNearClip(), 0.01f);
		cb.FarClip        = (std::max)(camera.GetFarClip(), cb.NearClip + 1.0f);
		cb.DepthSharpness = 12.0f;

		ComputeContext& computeCtx = context.GetComputeContext();
		computeCtx.SetRootSignature(services.GetBindlessComputeRootSignature());
		computeCtx.SetPipelineStateRaw(m_HotBlurPSO ? m_HotBlurPSO.Get() : m_BlurPSO.GetPipelineStateObject());
		computeCtx.SetConstantArray(0, 4, rootConsts);
		computeCtx.SetDynamicConstantBufferView(2, sizeof(cb), &cb);
		computeCtx.Dispatch2D(width, height, 8, 8);

		context.PIXEndEvent();
	}
}
