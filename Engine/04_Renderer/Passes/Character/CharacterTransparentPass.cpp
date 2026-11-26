#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Character/CharacterTransparentPass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"
#include "04_Renderer/Passes/Character/CharacterPassCommon.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"

#include "08_Character/CharacterOpaqueVS.h"
#include "08_Character/CharacterTransparentPS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	namespace
	{
		D3D12_BLEND_DESC MakeTransparentMrtBlendDesc()
		{
			D3D12_BLEND_DESC bd = Graphics::BlendPreMultiplied;
			bd.IndependentBlendEnable = TRUE;
			// RT0: LinearHDR PreMultiplied (One, InvSrcAlpha) 保留高光全强度
			bd.RenderTarget[0] = Graphics::BlendPreMultiplied.RenderTarget[0];
			// RT1: CharacterAuxMask 保持不透明写入 0.498
			bd.RenderTarget[1] = Graphics::BlendDisable.RenderTarget[0];
			return bd;
		}
	}

	bool CharacterTransparentPass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M5] CharacterTransparentPass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_VS.assign(CharacterOpaqueVS_cso, CharacterOpaqueVS_cso + sizeof(CharacterOpaqueVS_cso));
		m_PS.assign(CharacterTransparentPS_cso, CharacterTransparentPS_cso + sizeof(CharacterTransparentPS_cso));

		const DXGI_FORMAT rtvs[2] = {
			Deferred::LinearHDRBufferFormat,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
		};
		const D3D12_BLEND_DESC bd = MakeTransparentMrtBlendDesc();

		const D3D12_DEPTH_STENCIL_DESC dsFront = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsBack = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE);

		auto InitPso = [&](GraphicsPSO& pso, const wchar_t* name, D3D12_CULL_MODE cull, const D3D12_DEPTH_STENCIL_DESC& ds)
		{
			pso = GraphicsPSO(name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode = cull;
			pso.SetRasterizerState(rast);
			pso.SetBlendState(bd);
			pso.SetDepthStencilState(ds);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(kCharacterInputLayoutCount, kCharacterInputLayout);
			pso.SetRenderTargetFormats(2, rtvs, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			pso.SetVertexShader(m_VS.data(), m_VS.size());
			pso.SetPixelShader(m_PS.data(), m_PS.size());
			pso.Finalize();
		};

		InitPso(m_PsoCullFront, L"GFL2 CharacterTransparent CullFront PSO", D3D12_CULL_MODE_FRONT, dsFront);
		InitPso(m_PsoCullBack,  L"GFL2 CharacterTransparent CullBack PSO",  D3D12_CULL_MODE_BACK,  dsBack);

		m_Initialized = true;
		return true;
	}

	void CharacterTransparentPass::Shutdown()
	{
		m_HotFront.Reset();
		m_HotBack.Reset();
		m_VS.clear();
		m_PS.clear();
		m_Initialized = false;
	}

	bool CharacterTransparentPass::RebuildPSOs(Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr)
		{
			outErr = "CharacterTransparentPass not initialized";
			return false;
		}
		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		const D3D12_BLEND_DESC bd = MakeTransparentMrtBlendDesc();
		const D3D12_DEPTH_STENCIL_DESC dsFront = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsBack = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE);

		auto BuildOne = [&](const wchar_t* name, D3D12_CULL_MODE cull, const D3D12_DEPTH_STENCIL_DESC& ds, Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso) -> bool
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { m_VS.data(), m_VS.size() };
			d.PS                    = { m_PS.data(), m_PS.size() };
			d.BlendState            = bd;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode = cull;
			d.DepthStencilState     = ds;
			d.InputLayout           = { kCharacterInputLayout, kCharacterInputLayoutCount };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets      = 2;
			d.RTVFormats[0]         = Deferred::LinearHDRBufferFormat;
			d.RTVFormats[1]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
			d.DSVFormat             = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.SampleDesc.Count      = 1;

			const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "CharacterTransparentPass CreatePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		};

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candF, candB;
		if (!BuildOne(L"GFL2 Transparent Hot Front", D3D12_CULL_MODE_FRONT, dsFront, candF) ||
			!BuildOne(L"GFL2 Transparent Hot Back",  D3D12_CULL_MODE_BACK,  dsBack,  candB))
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

		RetireSwap(m_HotFront, std::move(candF));
		RetireSwap(m_HotBack,  std::move(candB));
		return true;
	}

	void CharacterTransparentPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		Graphics::ShaderHotReloadDesc d;
		d.Id            = "CharacterTransparentPS";
		d.SourceRelPath = L"Engine/Shader/08_Character/CharacterTransparentPS.hlsl";
		d.EntryPoint    = L"MainPS";
		d.TargetProfile = L"ps_6_6";
		d.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
		{
			const std::vector<uint8_t> backup = m_PS;
			m_PS = dxil;
			if (!RebuildPSOs(services, outErr))
			{
				m_PS = backup;
				return false;
			}
			return true;
		};
		reloader.Register(std::move(d));
	}

	void CharacterTransparentPass::Execute(
		GraphicsContext& context,
		const ::GFL2::CharacterManifestLoader& assets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		ColorBuffer& linearHDR,
		ColorBuffer& characterAuxMask,
		DepthBuffer& sceneDepth,
		uint32_t mainShadowSrvIdx,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services)
	{
		const auto& st = assets.GetStats();
		if (!m_Initialized || !st.Loaded || !st.MeshLoaded)
			return;

		const auto& metas     = assets.GetDrawMeta();
		const auto& draws     = assets.GetCpuDrawData();
		const auto& materials = assets.GetCpuMaterialData();
		const auto& submeshes = assets.GetSubmeshes();

		context.PIXBeginEvent(L"7. CharacterTransparentPass (5 Pairs Front->Back)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(characterAuxMask, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetViewportAndScissor(viewport, scissor);

		const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { linearHDR.GetRTV(), characterAuxMask.GetRTV() };
		context.SetRenderTargets(2, rtvs, sceneDepth.GetDSV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetVertexBuffer(0, assets.GetVertexBufferView());
		context.SetIndexBuffer(assets.GetIndexBufferView());
		context.SetStencilRef(200u);

		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			if (meta.ShadingModelId != ::GFL2::ShadingModel::Transparent)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size() || draw.MaterialIndex >= materials.size())
				continue;
			const auto& sub = submeshes[draw.SubmeshIndex];
			const auto& mat = materials[draw.MaterialIndex];

			if (meta.CullMode == ::GFL2::CullModeId::Front)
				context.SetPipelineStateRaw(m_HotFront ? m_HotFront.Get() : m_PsoCullFront.GetPipelineStateObject());
			else
				context.SetPipelineStateRaw(m_HotBack ? m_HotBack.Get() : m_PsoCullBack.GetPipelineStateObject());

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex             = static_cast<uint32_t>(di);
			rc.MaterialIndex         = draw.MaterialIndex;
			rc.DrawDataSrvIdx        = st.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx    = st.MaterialDataSrvIdx;
			rc.MainShadowMapSrvIdx   = mainShadowSrvIdx;
			rc.ReflectionProbeSrvIdx = mat.ReflectionProbeIdx;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
		}

		context.PIXEndEvent();
	}
}
