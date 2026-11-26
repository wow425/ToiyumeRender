#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Character/CharacterFaceHairEyePass.h"

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
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"

#include "08_Character/CharacterOpaqueVS.h"
#include "08_Character/CharacterBangsShadowVS.h"
#include "08_Character/CharacterFacePS.h"
#include "08_Character/CharacterHairPS.h"
#include "08_Character/CharacterEyePS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	namespace
	{
		D3D12_BLEND_DESC MakeMultiplyBlendDesc()
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

		struct PsoBuildSpec
		{
			const wchar_t*           Name;
			const void*              VS;
			size_t                   VSSize;
			const void*              PS;
			size_t                   PSSize;
			D3D12_CULL_MODE          Cull;
			D3D12_BLEND_DESC         Blend;
			D3D12_DEPTH_STENCIL_DESC DepthStencil;
			uint32_t                 NumRtvs;
		};

		void InitStaticPSO(
			GraphicsPSO& pso,
			Renderer::RenderServices& services,
			const PsoBuildSpec& spec)
		{
			const DXGI_FORMAT rtvs[2] = {
				Deferred::LinearHDRBufferFormat,
				DXGI_FORMAT_R16G16B16A16_FLOAT,
			};
			pso = GraphicsPSO(spec.Name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());

			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode = spec.Cull;
			pso.SetRasterizerState(rast);
			pso.SetBlendState(spec.Blend);
			pso.SetDepthStencilState(spec.DepthStencil);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(kCharacterInputLayoutCount, kCharacterInputLayout);
			pso.SetRenderTargetFormats(spec.NumRtvs, (spec.NumRtvs > 0u) ? rtvs : nullptr, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			pso.SetVertexShader(spec.VS, spec.VSSize);
			if (spec.PS != nullptr && spec.PSSize > 0)
				pso.SetPixelShader(spec.PS, spec.PSSize);
			pso.Finalize();
		}

		bool BuildRawPSO(
			ID3D12RootSignature* rs,
			const PsoBuildSpec& spec,
			Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso,
			std::string& outErr)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { spec.VS, spec.VSSize };
			if (spec.PS != nullptr && spec.PSSize > 0)
				d.PS = { spec.PS, spec.PSSize };
			d.BlendState            = spec.Blend;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode = spec.Cull;
			d.DepthStencilState     = spec.DepthStencil;
			d.InputLayout           = { kCharacterInputLayout, kCharacterInputLayoutCount };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets      = spec.NumRtvs;
			if (spec.NumRtvs >= 1u) d.RTVFormats[0] = Deferred::LinearHDRBufferFormat;
			if (spec.NumRtvs >= 2u) d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			d.DSVFormat             = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.SampleDesc.Count      = 1;

			const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "CharacterFaceHairEyePass CreatePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(spec.Name);
			return true;
		}
	}

	bool CharacterFaceHairEyePass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M4] CharacterFaceHairEyePass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_OpaqueVS.assign(CharacterOpaqueVS_cso, CharacterOpaqueVS_cso + sizeof(CharacterOpaqueVS_cso));
		m_BangsShadowVS.assign(CharacterBangsShadowVS_cso, CharacterBangsShadowVS_cso + sizeof(CharacterBangsShadowVS_cso));
		m_FacePS.assign(CharacterFacePS_cso, CharacterFacePS_cso + sizeof(CharacterFacePS_cso));
		m_HairPS.assign(CharacterHairPS_cso, CharacterHairPS_cso + sizeof(CharacterHairPS_cso));
		m_EyePS.assign(CharacterEyePS_cso, CharacterEyePS_cso + sizeof(CharacterEyePS_cso));

		const D3D12_BLEND_DESC blendMul = MakeMultiplyBlendDesc();

		const D3D12_DEPTH_STENCIL_DESC dsWriteReplace = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE,
			D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE);
		const D3D12_DEPTH_STENCIL_DESC dsNotEqualReplace = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_STENCIL_OP_REPLACE);
		const D3D12_DEPTH_STENCIL_DESC dsEqualInc = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_INCR_SAT);
		const D3D12_DEPTH_STENCIL_DESC dsEqualDec = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_DECR_SAT);
		const D3D12_DEPTH_STENCIL_DESC dsEqualKeep = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsDepthRONoStencil = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			false, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);

		InitStaticPSO(m_PsoFaceOpaque, services, {
			L"GFL2 FaceOpaque PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_FacePS.data(), m_FacePS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsWriteReplace, 2u });
		InitStaticPSO(m_PsoFaceShadowRedraw, services, {
			L"GFL2 FaceShadowRedraw PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_FacePS.data(), m_FacePS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsEqualDec, 2u });
		InitStaticPSO(m_PsoHairBack, services, {
			L"GFL2 HairBack PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(),
			D3D12_CULL_MODE_NONE, Graphics::BlendDisable, dsWriteReplace, 2u });
		InitStaticPSO(m_PsoHairBangsOpaque, services, {
			L"GFL2 HairBangsOpaque PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsNotEqualReplace, 2u });
		InitStaticPSO(m_PsoHairBangsAlpha, services, {
			L"GFL2 HairBangsAlpha PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendTraditional, dsEqualKeep, 1u });
		InitStaticPSO(m_PsoBangsShadowStencil, services, {
			L"GFL2 BangsShadowStencil PSO", m_BangsShadowVS.data(), m_BangsShadowVS.size(), nullptr, 0,
			D3D12_CULL_MODE_BACK, Graphics::BlendNoColorWrite, dsEqualInc, 0u });
		InitStaticPSO(m_PsoEyeBase, services, {
			L"GFL2 EyeBase PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsWriteReplace, 2u });
		InitStaticPSO(m_PsoEyeAdd, services, {
			L"GFL2 EyeAdd PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(),
			D3D12_CULL_MODE_BACK, Graphics::BlendAdditive, dsDepthRONoStencil, 1u });
		InitStaticPSO(m_PsoEyeMul, services, {
			L"GFL2 EyeMul PSO", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(),
			D3D12_CULL_MODE_BACK, blendMul, dsDepthRONoStencil, 1u });

		m_Initialized = true;
		return true;
	}

	void CharacterFaceHairEyePass::Shutdown()
	{
		m_HotFaceOpaque.Reset();
		m_HotFaceRedraw.Reset();
		m_HotHairBack.Reset();
		m_HotHairBangsOpaque.Reset();
		m_HotHairBangsAlpha.Reset();
		m_HotBangsShadowStencil.Reset();
		m_HotEyeBase.Reset();
		m_HotEyeAdd.Reset();
		m_HotEyeMul.Reset();

		m_OpaqueVS.clear();
		m_BangsShadowVS.clear();
		m_FacePS.clear();
		m_HairPS.clear();
		m_EyePS.clear();
		m_Initialized = false;
	}

	bool CharacterFaceHairEyePass::RebuildAllPSOs(Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr)
		{
			outErr = "CharacterFaceHairEyePass not initialized";
			return false;
		}
		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		const D3D12_BLEND_DESC blendMul = MakeMultiplyBlendDesc();
		const D3D12_DEPTH_STENCIL_DESC dsWriteReplace = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE,
			D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE);
		const D3D12_DEPTH_STENCIL_DESC dsNotEqualReplace = MakeStencilDepthDesc(
			true, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_STENCIL_OP_REPLACE);
		const D3D12_DEPTH_STENCIL_DESC dsEqualInc = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_INCR_SAT);
		const D3D12_DEPTH_STENCIL_DESC dsEqualDec = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_DECR_SAT);
		const D3D12_DEPTH_STENCIL_DESC dsEqualKeep = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			true, D3D12_COMPARISON_FUNC_EQUAL, D3D12_STENCIL_OP_KEEP);
		const D3D12_DEPTH_STENCIL_DESC dsDepthRONoStencil = MakeStencilDepthDesc(
			false, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
			false, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_KEEP);

		Microsoft::WRL::ComPtr<ID3D12PipelineState> cFaceOpaque, cFaceRedraw, cHairBack, cHairOpaque, cHairAlpha, cBangsSt, cEyeBase, cEyeAdd, cEyeMul;
		if (!BuildRawPSO(rs, { L"GFL2 FaceOpaque Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_FacePS.data(), m_FacePS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsWriteReplace, 2u }, cFaceOpaque, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 FaceRedraw Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_FacePS.data(), m_FacePS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsEqualDec, 2u }, cFaceRedraw, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 HairBack Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(), D3D12_CULL_MODE_NONE, Graphics::BlendDisable, dsWriteReplace, 2u }, cHairBack, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 HairOpaque Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsNotEqualReplace, 2u }, cHairOpaque, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 HairAlpha Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_HairPS.data(), m_HairPS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendTraditional, dsEqualKeep, 1u }, cHairAlpha, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 BangsStencil Hot", m_BangsShadowVS.data(), m_BangsShadowVS.size(), nullptr, 0, D3D12_CULL_MODE_BACK, Graphics::BlendNoColorWrite, dsEqualInc, 0u }, cBangsSt, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 EyeBase Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendDisable, dsWriteReplace, 2u }, cEyeBase, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 EyeAdd Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendAdditive, dsDepthRONoStencil, 1u }, cEyeAdd, outErr) ||
			!BuildRawPSO(rs, { L"GFL2 EyeMul Hot", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EyePS.data(), m_EyePS.size(), D3D12_CULL_MODE_BACK, blendMul, dsDepthRONoStencil, 1u }, cEyeMul, outErr))
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

		RetireSwap(m_HotFaceOpaque,         std::move(cFaceOpaque));
		RetireSwap(m_HotFaceRedraw,         std::move(cFaceRedraw));
		RetireSwap(m_HotHairBack,           std::move(cHairBack));
		RetireSwap(m_HotHairBangsOpaque,    std::move(cHairOpaque));
		RetireSwap(m_HotHairBangsAlpha,     std::move(cHairAlpha));
		RetireSwap(m_HotBangsShadowStencil, std::move(cBangsSt));
		RetireSwap(m_HotEyeBase,            std::move(cEyeBase));
		RetireSwap(m_HotEyeAdd,             std::move(cEyeAdd));
		RetireSwap(m_HotEyeMul,             std::move(cEyeMul));
		return true;
	}

	void CharacterFaceHairEyePass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		auto Reg = [&](const char* id, const wchar_t* path, const wchar_t* prof, std::vector<uint8_t>* target)
		{
			Graphics::ShaderHotReloadDesc d;
			d.Id            = id;
			d.SourceRelPath = path;
			d.EntryPoint    = (std::wstring(prof).rfind(L"vs_", 0) == 0) ? L"MainVS" : L"MainPS";
			d.TargetProfile = prof;
			d.RebuildPipeline = [this, &services, target](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
			{
				const std::vector<uint8_t> backup = *target;
				*target = dxil;
				if (!RebuildAllPSOs(services, outErr))
				{
					*target = backup;
					return false;
				}
				return true;
			};
			reloader.Register(std::move(d));
		};

		Reg("CharacterFacePS",        L"Engine/Shader/08_Character/CharacterFacePS.hlsl",        L"ps_6_6", &m_FacePS);
		Reg("CharacterHairPS",        L"Engine/Shader/08_Character/CharacterHairPS.hlsl",        L"ps_6_6", &m_HairPS);
		Reg("CharacterEyePS",         L"Engine/Shader/08_Character/CharacterEyePS.hlsl",         L"ps_6_6", &m_EyePS);
		Reg("CharacterBangsShadowVS", L"Engine/Shader/08_Character/CharacterBangsShadowVS.hlsl", L"vs_6_6", &m_BangsShadowVS);
	}

	void CharacterFaceHairEyePass::Execute(
		GraphicsContext& context,
		const ::GFL2::CharacterManifestLoader& assets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Renderer::RenderParams& params,
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

		auto FindDrawIndex = [&](uint32_t eid) -> int
		{
			for (size_t i = 0; i < metas.size(); ++i)
				if (metas[i].EventId == eid)
					return static_cast<int>(i);
			return -1;
		};

		context.PIXBeginEvent(L"5. CharacterFaceHairEyePass (4-State Stencil)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(characterAuxMask, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetViewportAndScissor(viewport, scissor);
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetVertexBuffer(0, assets.GetVertexBufferView());
		context.SetIndexBuffer(assets.GetIndexBufferView());

		const D3D12_CPU_DESCRIPTOR_HANDLE rtv2[2] = { linearHDR.GetRTV(), characterAuxMask.GetRTV() };
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv1[1] = { linearHDR.GetRTV() };

		auto DrawOne = [&](
			uint32_t eid,
			const wchar_t* pixName,
			ID3D12PipelineState* pso,
			uint32_t stencilRef,
			uint32_t rtvCount,
			bool readOnlyDepth,
			uint32_t subMode,
			float param0)
		{
			const int di = FindDrawIndex(eid);
			if (di < 0)
				return;
			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size() || draw.MaterialIndex >= materials.size())
				return;
			const auto& sub = submeshes[draw.SubmeshIndex];
			const auto& mat = materials[draw.MaterialIndex];

			context.PIXBeginEvent(pixName);
			if (rtvCount == 2u)
				context.SetRenderTargets(2, rtv2, readOnlyDepth ? sceneDepth.GetDSV_DepthReadOnly() : sceneDepth.GetDSV());
			else if (rtvCount == 1u)
				context.SetRenderTargets(1, rtv1, readOnlyDepth ? sceneDepth.GetDSV_DepthReadOnly() : sceneDepth.GetDSV());
			else
				context.SetDepthStencilTarget(sceneDepth.GetDSV_DepthReadOnly());

			context.SetPipelineStateRaw(pso);
			context.SetStencilRef(stencilRef);

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex             = static_cast<uint32_t>(di);
			rc.MaterialIndex         = draw.MaterialIndex;
			rc.DrawDataSrvIdx        = st.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx    = st.MaterialDataSrvIdx;
			rc.MainShadowMapSrvIdx   = mainShadowSrvIdx;
			rc.ReflectionProbeSrvIdx = mat.ReflectionProbeIdx;
			rc.PassSubMode           = subMode;
			rc.PassParam0            = param0;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
			context.PIXEndEvent();
		};

		auto PickPso = [](const Microsoft::WRL::ComPtr<ID3D12PipelineState>& hot, const GraphicsPSO& stat) -> ID3D12PipelineState*
		{
			return hot ? hot.Get() : stat.GetPipelineStateObject();
		};

		// 5.1 面部皮肤 (EID 3031, Write Stencil=200)
		DrawOne(3031u, L"5.1 Face_Skin_EID3031 (Write Stencil=200)",
			PickPso(m_HotFaceOpaque, m_PsoFaceOpaque), 200u, 2u, false, 0u,
			params.GFL2EnableFaceSDF ? 1.0f : 0.0f);

		// 5.2 眉毛 (EID 3062, Write 204) & 后发 (EID 3100, CullNone, Write 203)
		DrawOne(3062u, L"5.2a Eyebrow_EID3062 (Write Stencil=204)",
			PickPso(m_HotFaceOpaque, m_PsoFaceOpaque), 204u, 2u, false, 0u,
			params.GFL2EnableFaceSDF ? 1.0f : 0.0f);
		DrawOne(3100u, L"5.2b HairBack_EID3100 (CullNone, Write Stencil=203)",
			PickPso(m_HotHairBack, m_PsoHairBack), 203u, 2u, false, 0u,
			params.GFL2EnableHairAniso ? 1.0f : 0.0f);

		// 5.3 眼球底层 (EID 3137, Write Stencil=204)
		if (params.GFL2EnableEye)
		{
			DrawOne(3137u, L"5.3 EyeBase_EID3137 (Parallax, Write Stencil=204)",
				PickPso(m_HotEyeBase, m_PsoEyeBase), 204u, 2u, false, 0u, 1.0f);
		}

		// 5.4 前刘海不透明层 (EID 3178, NotEqual 204 -> Replace 204)
		DrawOne(3178u, L"5.4 HairBangs_Opaque_EID3178 (NotEqual 204 -> Replace 204)",
			PickPso(m_HotHairBangsOpaque, m_PsoHairBangsOpaque), 204u, 2u, false, 0u,
			params.GFL2EnableHairAniso ? 1.0f : 0.0f);

		// 5.5/5.6/5.7/5.8 进入深度只读+模板读写阶段（仅允许修改 Stencil，深度只读测试）
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_READ);
		// 注意：5.5/5.7 仍需写 Stencil！DX12 中 DepthRead 不可写 Stencil，需保持 DEPTH_WRITE 但 PSO.DepthWriteMask=ZERO
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();

		// 5.5 前刘海投影写模板 (EID 3198, Equal 200 -> IncSat 201)
		if (params.GFL2EnableBangsStencilShadow)
		{
			DrawOne(3198u, L"5.5 BangsShadowStencil_EID3198 (Equal 200 -> IncSat 201)",
				PickPso(m_HotBangsShadowStencil, m_PsoBangsShadowStencil), 200u, 0u, false, 0u, 0.0f);
		}

		// 5.6 眼球高光 Add (EID 3229) & 眼窝阴影 Mul (EID 3252)
		if (params.GFL2EnableEye)
		{
			DrawOne(3229u, L"5.6a EyeHighlightAdd_EID3229",
				PickPso(m_HotEyeAdd, m_PsoEyeAdd), 0u, 1u, false, 1u, 1.0f);
			DrawOne(3252u, L"5.6b EyeSocketMul_EID3252",
				PickPso(m_HotEyeMul, m_PsoEyeMul), 0u, 1u, false, 2u, 1.0f);
		}

		// 5.7 面部刘海影重绘 (EID 3295, Equal 201 -> DecSat 200)
		if (params.GFL2EnableBangsStencilShadow)
		{
			DrawOne(3295u, L"5.7 FaceShadowRedraw_EID3295 (Equal 201 -> DecSat 200)",
				PickPso(m_HotFaceRedraw, m_PsoFaceShadowRedraw), 201u, 2u, false, 1u,
				params.GFL2EnableFaceSDF ? 1.0f : 0.0f);
		}

		// 5.8 眉眼透刘海覆盖 (EID 3335, Equal 204 -> Keep, Alpha=0.49)
		if (params.GFL2EnableBangsOverEye)
		{
			DrawOne(3335u, L"5.8 BangsOverEye_EID3335 (Equal 204, Alpha=0.49)",
				PickPso(m_HotHairBangsAlpha, m_PsoHairBangsAlpha), 204u, 1u, false, 1u,
				params.GFL2EnableHairAniso ? 1.0f : 0.0f);
		}

		context.PIXEndEvent();
	}
}
