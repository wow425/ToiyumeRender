#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Character/CharacterOutlinePass.h"

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

#include "08_Character/CharacterOutlineVS.h"
#include "08_Character/CharacterOutlinePS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	bool CharacterOutlinePass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M5] CharacterOutlinePass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_VS.assign(CharacterOutlineVS_cso, CharacterOutlineVS_cso + sizeof(CharacterOutlineVS_cso));
		m_PS.assign(CharacterOutlinePS_cso, CharacterOutlinePS_cso + sizeof(CharacterOutlinePS_cso));

		const DXGI_FORMAT rtvs[2] = {
			Deferred::LinearHDRBufferFormat,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
		};

		auto InitPso = [&](GraphicsPSO& pso, const wchar_t* name, const D3D12_DEPTH_STENCIL_DESC& ds)
		{
			pso = GraphicsPSO(name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode = D3D12_CULL_MODE_FRONT;
			pso.SetRasterizerState(rast);
			pso.SetBlendState(Graphics::BlendDisable);
			pso.SetDepthStencilState(ds);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(kCharacterInputLayoutCount, kCharacterInputLayout);
			pso.SetRenderTargetFormats(2, rtvs, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			pso.SetVertexShader(m_VS.data(), m_VS.size());
			pso.SetPixelShader(m_PS.data(), m_PS.size());
			pso.Finalize();
		};

		const D3D12_DEPTH_STENCIL_DESC dsDefault = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			false, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsBackNotEq204 = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP,
			D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_STENCIL_OP_KEEP);

		InitPso(m_PsoOutlineDefault,     L"GFL2 CharacterOutline Default PSO",       dsDefault);
		InitPso(m_PsoOutlineNotEqual204, L"GFL2 CharacterOutline NotEqual204 PSO",   dsBackNotEq204);

		m_Initialized = true;
		return true;
	}

	void CharacterOutlinePass::Shutdown()
	{
		m_HotDefault.Reset();
		m_HotNotEqual204.Reset();
		m_VS.clear();
		m_PS.clear();
		m_Initialized = false;
	}

	bool CharacterOutlinePass::RebuildPSOs(Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr)
		{
			outErr = "CharacterOutlinePass not initialized";
			return false;
		}
		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		auto BuildOne = [&](const wchar_t* name, const D3D12_DEPTH_STENCIL_DESC& ds, Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso) -> bool
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { m_VS.data(), m_VS.size() };
			d.PS                    = { m_PS.data(), m_PS.size() };
			d.BlendState            = Graphics::BlendDisable;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
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
				std::snprintf(buf, sizeof(buf), "CharacterOutlinePass CreatePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		};

		const D3D12_DEPTH_STENCIL_DESC dsDefault = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			false, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsBackNotEq204 = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP,
			D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_STENCIL_OP_KEEP);

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candDef, cand204;
		if (!BuildOne(L"GFL2 Outline Hot Default", dsDefault, candDef) ||
			!BuildOne(L"GFL2 Outline Hot NotEq204", dsBackNotEq204, cand204))
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

		RetireSwap(m_HotDefault, std::move(candDef));
		RetireSwap(m_HotNotEqual204, std::move(cand204));
		return true;
	}

	void CharacterOutlinePass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
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

		Reg("CharacterOutlineVS", L"Engine/Shader/08_Character/CharacterOutlineVS.hlsl", L"MainVS", L"vs_6_6", &m_VS);
		Reg("CharacterOutlinePS", L"Engine/Shader/08_Character/CharacterOutlinePS.hlsl", L"MainPS", L"ps_6_6", &m_PS);
	}

	void CharacterOutlinePass::Execute(
		GraphicsContext& context,
		const ::GFL2::CharacterManifestLoader& assets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		ColorBuffer& linearHDR,
		ColorBuffer& characterAuxMask,
		DepthBuffer& sceneDepth,
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

		context.PIXBeginEvent(L"6. CharacterOutlinePass (19x SmoothNormal CullFront)");

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

		int lastMode = -1;
		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			if (meta.ShadingModelId != ::GFL2::ShadingModel::Outline)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size() || draw.MaterialIndex >= materials.size())
				continue;
			const auto& sub = submeshes[draw.SubmeshIndex];

			const bool isBangsOutline = (meta.EventId == 3577u) ||
				(meta.StencilPreset == ::GFL2::StencilPresetId::OutlineNotEqual204);
			const int mode = isBangsOutline ? 1 : 0;
			if (mode != lastMode)
			{
				lastMode = mode;
				if (isBangsOutline)
				{
					context.SetPipelineStateRaw(m_HotNotEqual204 ? m_HotNotEqual204.Get() : m_PsoOutlineNotEqual204.GetPipelineStateObject());
					context.SetStencilRef(204u);
				}
				else
				{
					context.SetPipelineStateRaw(m_HotDefault ? m_HotDefault.Get() : m_PsoOutlineDefault.GetPipelineStateObject());
					context.SetStencilRef(0u);
				}
			}

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex          = static_cast<uint32_t>(di);
			rc.MaterialIndex      = draw.MaterialIndex;
			rc.DrawDataSrvIdx     = st.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx = st.MaterialDataSrvIdx;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
		}

		context.PIXEndEvent();
	}
}
