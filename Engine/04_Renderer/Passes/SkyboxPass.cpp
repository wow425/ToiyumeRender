// ============================================================================
// 模块：SkyboxPass（环境天空盒全屏背景 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/SkyboxPass.h"

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
#include "05_Scene/Camera/Camera.h"

#include "06_Environment/SkyboxVS.h"
#include "06_Environment/SkyboxPS.h"

namespace Renderer
{
	bool SkyboxPass::Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
	{
		using namespace Graphics;

		SamplerDesc linearSamplerDesc;
		linearSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		linearSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linearSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linearSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linearSamplerDesc.MaxAnisotropy = 1;

		m_RootSig.Reset(2, 1);
		m_RootSig.InitStaticSampler(0, linearSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		m_RootSig[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
		m_RootSig[1].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
		m_RootSig.Finalize(L"SkyboxPass RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		m_PSO = GraphicsPSO(L"SkyboxPass PSO");
		m_PSO.SetRootSignature(m_RootSig);
		m_PSO.SetRasterizerState(RasterizerFullScreen);
		m_PSO.SetDepthStencilState(DepthStateReadOnly);
		m_PSO.SetBlendState(BlendDisable);
		m_PSO.SetInputLayout(0, nullptr);
		m_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		m_PSO.SetRenderTargetFormat(colorFormat, depthFormat);
		m_PSO.SetVertexShader(SkyboxVS_cso, sizeof(SkyboxVS_cso));
		m_PSO.SetPixelShader(SkyboxPS_cso, sizeof(SkyboxPS_cso));
		m_PSO.Finalize();
		m_ColorFormat = colorFormat;
		m_DepthFormat = depthFormat;

		m_Initialized = true;
		return true;
	}

	void SkyboxPass::Shutdown()
	{
		m_HotPSO.Reset();
		m_RootSig.Reset(0, 0);
		m_Initialized = false;
	}

	void SkyboxPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized)
			return;
		Graphics::ShaderHotReloadDesc desc;
		desc.Id            = "SkyboxPS";
		desc.SourceRelPath = L"Engine/Shader/06_Environment/SkyboxPS.hlsl";
		desc.EntryPoint    = L"MainPS";
		desc.TargetProfile = L"ps_6_4";
		desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
			return HotReloadPixelShader(dxil, services, outErr);
		};
		reloader.Register(std::move(desc));
	}

	bool SkyboxPass::HotReloadPixelShader(const std::vector<uint8_t>& dxil, RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "SkyboxPass not initialized or empty DXIL";
			return false;
		}
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature        = m_RootSig.GetSignature();
		psoDesc.VS                    = { SkyboxVS_cso, sizeof(SkyboxVS_cso) };
		psoDesc.PS                    = { dxil.data(), dxil.size() };
		psoDesc.BlendState            = Graphics::BlendDisable;
		psoDesc.SampleMask            = UINT_MAX;
		psoDesc.RasterizerState       = Graphics::RasterizerFullScreen;
		psoDesc.DepthStencilState     = Graphics::DepthStateReadOnly;
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
			std::snprintf(buf, sizeof(buf), "SkyboxPass CreateGraphicsPipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(L"SkyboxPass HotReload PSO");
		if (m_HotPSO != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> oldUnk;
			m_HotPSO.As(&oldUnk);
			services.GetReleaseQueue().Enqueue(std::move(oldUnk));
		}
		m_HotPSO = std::move(candidate);
		return true;
	}

	void SkyboxPass::Execute(GraphicsContext& context,
		const FrameBlackboard& blackboard,
		const Scene::Camera::Camera& camera,
		const RenderParams& params,
		RenderServices& services,
		const Math::Matrix4* projOverride) const
	{
		if (!m_Initialized)
			return;

		// [TD-06d] 仅在 DebugView == Final 时绘制天空盒，防止覆盖 GBuffer/Depth 调试通道背景。
		if (params.Debug != DebugView::Final)
			return;

		ColorBuffer* envCube     = blackboard.GetColorBuffer(FrameResourceKey::EnvCube);
		DepthBuffer* sceneDepth  = blackboard.Core.SceneDepth;
		ColorBuffer* outputColor = blackboard.Core.LinearHDRColor != nullptr
			? blackboard.Core.LinearHDRColor
			: blackboard.Core.SceneColorLDR;

		if (envCube == nullptr || sceneDepth == nullptr || outputColor == nullptr)
			return;

		context.PIXBeginEvent(L"SkyPass");

		context.TransitionResource(*outputColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
		// [TD-04c / DX12 只读深度不变量] 此时 SceneDepth 处于 DEPTH_READ | PIXEL_SHADER_RESOURCE 只读态，
		// OMSetRenderTargets 必须绑定 GetDSV_DepthReadOnly()，绑定可写 GetDSV() 会触发 Debug Layer 写冲突 ERROR。
		context.TransitionResource(*sceneDepth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(*envCube, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.FlushResourceBarriers();

		context.SetRenderTarget(outputColor->GetRTV(), sceneDepth->GetDSV_DepthReadOnly());

		// 与 SkyboxPS.hlsl 的 cbuffer CameraCB 逐字节对齐（InvView @0, InvProj @64, DisplayParams @128，共 144B）
		struct alignas(16) SkyboxCameraCB
		{
			Math::Matrix4     InvView;
			Math::Matrix4     InvProj;
			DirectX::XMFLOAT4 DisplayParams; // x=Exposure, y=EnableTonemap, z=EnableSRGBEncode, w=unused
		};
		static_assert(sizeof(SkyboxCameraCB) == 144 && sizeof(SkyboxCameraCB) % 16 == 0,
			"SkyboxCameraCB must match SkyboxPS.hlsl cbuffer layout and 16B alignment");

		SkyboxCameraCB cameraCB = {};
		cameraCB.InvView = Math::Invert(camera.GetViewMatrix());
		cameraCB.InvProj = Math::Invert((projOverride != nullptr) ? *projOverride : camera.GetProjMatrix());
		cameraCB.DisplayParams = DirectX::XMFLOAT4(
			params.Exposure,
			params.EnableTonemap ? 1.0f : 0.0f,
			params.EnableSRGBEncode ? 1.0f : 0.0f,
			0.0f);

		context.SetRootSignature(m_RootSig);
		context.SetPipelineStateRaw(m_HotPSO ? m_HotPSO.Get() : m_PSO.GetPipelineStateObject());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// [契约 1] 经统一单可见堆 Zone B 暂存环境贴图 SRV，保持整帧 SetDescriptorHeaps <= 1
		const BindlessAllocation stagedEnv =
			services.GetDescriptorHeap().StageTransientDescriptor(envCube->GetSRV());
		context.SetDescriptorTable(0, stagedEnv.Gpu);
		context.SetDynamicConstantBufferView(1, sizeof(cameraCB), &cameraCB);

		context.Draw(3, 0);

		context.PIXEndEvent();
	}
}
