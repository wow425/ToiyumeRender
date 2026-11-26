#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Character/CharacterOpaquePass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"

#include "08_Character/CharacterOpaqueVS.h"
#include "08_Character/CharacterBodyPS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	namespace
	{
		const D3D12_INPUT_ELEMENT_DESC kCharacterInputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
	}

	bool CharacterOpaquePass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M1] CharacterOpaquePass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_CurrentVSBytecode.assign(CharacterOpaqueVS_cso, CharacterOpaqueVS_cso + sizeof(CharacterOpaqueVS_cso));
		m_CurrentPSBytecode.assign(CharacterBodyPS_cso, CharacterBodyPS_cso + sizeof(CharacterBodyPS_cso));

		const DXGI_FORMAT kOpaqueRtvFormats[2] = {
			Deferred::LinearHDRBufferFormat,
			DXGI_FORMAT_R16G16B16A16_FLOAT, // CharacterAuxMask
		};

		auto BuildOpaqueDepthStencil = []() -> D3D12_DEPTH_STENCIL_DESC
		{
			D3D12_DEPTH_STENCIL_DESC ds = Graphics::DepthStateReadWriteReversed;
			ds.StencilEnable            = TRUE;
			ds.StencilReadMask          = 0xFF;
			ds.StencilWriteMask         = 0xFF;
			ds.FrontFace.StencilFunc    = D3D12_COMPARISON_FUNC_ALWAYS;
			ds.FrontFace.StencilPassOp  = D3D12_STENCIL_OP_REPLACE;
			ds.FrontFace.StencilFailOp  = D3D12_STENCIL_OP_KEEP;
			ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			ds.BackFace                 = ds.FrontFace;
			return ds;
		};
		const D3D12_DEPTH_STENCIL_DESC kOpaqueDepthStencil = BuildOpaqueDepthStencil();

		auto ConfigureBase = [&](GraphicsPSO& pso, const wchar_t* name, D3D12_CULL_MODE cull)
		{
			pso = GraphicsPSO(name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());

			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode = cull;
			pso.SetRasterizerState(rast);

			pso.SetBlendState(Graphics::BlendDisable);
			pso.SetDepthStencilState(kOpaqueDepthStencil);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(_countof(kCharacterInputLayout), kCharacterInputLayout);
			pso.SetRenderTargetFormats(2, kOpaqueRtvFormats, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			pso.SetVertexShader(m_CurrentVSBytecode.data(), m_CurrentVSBytecode.size());
			pso.SetPixelShader(m_CurrentPSBytecode.data(), m_CurrentPSBytecode.size());
			pso.Finalize();
		};

		ConfigureBase(m_PSO_CullBack,  L"GFL2 CharacterOpaque CullBack PSO",  D3D12_CULL_MODE_BACK);
		ConfigureBase(m_PSO_CullFront, L"GFL2 CharacterOpaque CullFront PSO", D3D12_CULL_MODE_FRONT);
		ConfigureBase(m_PSO_CullNone,  L"GFL2 CharacterOpaque CullNone PSO",  D3D12_CULL_MODE_NONE);

		m_Initialized = true;
		return true;
	}

	void CharacterOpaquePass::Shutdown()
	{
		m_HotPSO_Back.Reset();
		m_HotPSO_Front.Reset();
		m_HotPSO_None.Reset();
		m_CurrentVSBytecode.clear();
		m_CurrentPSBytecode.clear();
		m_Initialized = false;
	}

	bool CharacterOpaquePass::RebuildPSOs(
		const void* vsBytecode, size_t vsSize,
		const void* psBytecode, size_t psSize,
		Renderer::RenderServices& services,
		std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || vsBytecode == nullptr || psBytecode == nullptr || vsSize == 0 || psSize == 0)
		{
			outErr = "CharacterOpaquePass not initialized or empty bytecode";
			return false;
		}

		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		D3D12_DEPTH_STENCIL_DESC ds = Graphics::DepthStateReadWriteReversed;
		ds.StencilEnable            = TRUE;
		ds.StencilReadMask          = 0xFF;
		ds.StencilWriteMask         = 0xFF;
		ds.FrontFace.StencilFunc    = D3D12_COMPARISON_FUNC_ALWAYS;
		ds.FrontFace.StencilPassOp  = D3D12_STENCIL_OP_REPLACE;
		ds.FrontFace.StencilFailOp  = D3D12_STENCIL_OP_KEEP;
		ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		ds.BackFace                 = ds.FrontFace;

		auto BuildOne = [&](D3D12_CULL_MODE cull, const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso) -> bool
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { vsBytecode, vsSize };
			d.PS                    = { psBytecode, psSize };
			d.BlendState            = Graphics::BlendDisable;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode = cull;
			d.DepthStencilState     = ds;
			d.InputLayout           = { kCharacterInputLayout, _countof(kCharacterInputLayout) };
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
				std::snprintf(buf, sizeof(buf), "CreateGraphicsPipelineState failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		};

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candBack, candFront, candNone;
		if (!BuildOne(D3D12_CULL_MODE_BACK,  L"GFL2 CharacterOpaque Hot Back",  candBack) ||
			!BuildOne(D3D12_CULL_MODE_FRONT, L"GFL2 CharacterOpaque Hot Front", candFront) ||
			!BuildOne(D3D12_CULL_MODE_NONE,  L"GFL2 CharacterOpaque Hot None",  candNone))
		{
			return false;
		}

		auto RetireAndSwap = [&](Microsoft::WRL::ComPtr<ID3D12PipelineState>& slot, Microsoft::WRL::ComPtr<ID3D12PipelineState>&& cand)
		{
			if (slot != nullptr)
			{
				Microsoft::WRL::ComPtr<IUnknown> unk;
				slot.As(&unk);
				services.GetReleaseQueue().Enqueue(std::move(unk));
			}
			slot = std::move(cand);
		};

		RetireAndSwap(m_HotPSO_Back,  std::move(candBack));
		RetireAndSwap(m_HotPSO_Front, std::move(candFront));
		RetireAndSwap(m_HotPSO_None,  std::move(candNone));
		return true;
	}

	void CharacterOpaquePass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "CharacterOpaqueVS";
			desc.SourceRelPath = L"Engine/Shader/08_Character/CharacterOpaqueVS.hlsl";
			desc.EntryPoint    = L"MainVS";
			desc.TargetProfile = L"vs_6_6";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
			{
				if (!RebuildPSOs(dxil.data(), dxil.size(), m_CurrentPSBytecode.data(), m_CurrentPSBytecode.size(), services, outErr))
					return false;
				m_CurrentVSBytecode = dxil;
				return true;
			};
			reloader.Register(std::move(desc));
		}

		{
			Graphics::ShaderHotReloadDesc desc;
			desc.Id            = "CharacterBodyPS";
			desc.SourceRelPath = L"Engine/Shader/08_Character/CharacterBodyPS.hlsl";
			desc.EntryPoint    = L"MainPS";
			desc.TargetProfile = L"ps_6_6";
			desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
			{
				if (!RebuildPSOs(m_CurrentVSBytecode.data(), m_CurrentVSBytecode.size(), dxil.data(), dxil.size(), services, outErr))
					return false;
				m_CurrentPSBytecode = dxil;
				return true;
			};
			reloader.Register(std::move(desc));
		}
	}

	void CharacterOpaquePass::Execute(
		GraphicsContext& context,
		const ::GFL2::CharacterManifestLoader& assets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		ColorBuffer& linearHDR,
		ColorBuffer& characterAuxMask,
		DepthBuffer& sceneDepth,
		uint32_t mainShadowSrvIdx,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services,
		bool includePreM4FaceHairEye)
	{
		const auto& st = assets.GetStats();
		if (!m_Initialized || !st.Loaded || !st.MeshLoaded)
			return;

		const auto& metas     = assets.GetDrawMeta();
		const auto& draws     = assets.GetCpuDrawData();
		const auto& materials = assets.GetCpuMaterialData();
		const auto& submeshes = assets.GetSubmeshes();
		if (metas.empty() || submeshes.empty())
			return;

		context.PIXBeginEvent(L"4. CharacterOpaquePass (PBR+NPR Ramp)");

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

		auto BindCullPSO = [&](::GFL2::CullModeId cull, int& lastCull)
		{
			const int target = static_cast<int>(cull);
			if (target == lastCull)
				return;
			lastCull = target;

			ID3D12PipelineState* rawPso = nullptr;
			if (cull == ::GFL2::CullModeId::Front)
				rawPso = m_HotPSO_Front ? m_HotPSO_Front.Get() : m_PSO_CullFront.GetPipelineStateObject();
			else if (cull == ::GFL2::CullModeId::None)
				rawPso = m_HotPSO_None ? m_HotPSO_None.Get() : m_PSO_CullNone.GetPipelineStateObject();
			else
				rawPso = m_HotPSO_Back ? m_HotPSO_Back.Get() : m_PSO_CullBack.GetPipelineStateObject();

			context.SetPipelineStateRaw(rawPso);
		};

		int lastCull = -1;
		int lastStencilRef = -1;
		int activePixGroup = -1; // 0=Body/Cloth/Nails, 1=Weapons, 2=PreM4_FaceHairEye

		auto SwitchPixGroup = [&](int nextGroup)
		{
			if (activePixGroup == nextGroup)
				return;
			if (activePixGroup >= 0)
				context.PIXEndEvent();
			activePixGroup = nextGroup;
			if (activePixGroup == 0)      context.PIXBeginEvent(L"Body_Nails_Swimsuit_Coat (Stencil=200)");
			else if (activePixGroup == 1) context.PIXBeginEvent(L"Weapons_AR_HG (Stencil=200)");
			else if (activePixGroup == 2) context.PIXBeginEvent(L"PreM4_FaceHairEye_Opaque (Stencil=200/203/204)");
		};

		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			bool isOpaqueGroup = false;
			int group = 0;

			if (meta.ShadingModelId == ::GFL2::ShadingModel::Body ||
				meta.ShadingModelId == ::GFL2::ShadingModel::Cloth ||
				meta.ShadingModelId == ::GFL2::ShadingModel::Nails)
			{
				isOpaqueGroup = true;
				group = 0;
			}
			else if (meta.ShadingModelId == ::GFL2::ShadingModel::Weapon)
			{
				isOpaqueGroup = true;
				group = 1;
			}
			else if (includePreM4FaceHairEye &&
				(meta.EventId == 3031u || meta.EventId == 3062u || meta.EventId == 3100u ||
				 meta.EventId == 3137u || meta.EventId == 3178u))
			{
				isOpaqueGroup = true;
				group = 2;
			}

			if (!isOpaqueGroup)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size() || draw.MaterialIndex >= materials.size())
				continue;

			const auto& sub = submeshes[draw.SubmeshIndex];
			const auto& mat = materials[draw.MaterialIndex];

			// 写入 M2 硬件 8-bit 模板标识（与 AK12-frame7206.rdc 200/203/204 语义一致）
			uint32_t stencilVal = 200u;
			if (meta.EventId == 3100u)
				stencilVal = 203u;
			else if (meta.EventId == 3062u || meta.EventId == 3137u || meta.EventId == 3178u)
				stencilVal = 204u;

			SwitchPixGroup(group);
			BindCullPSO(meta.CullMode, lastCull);
			if (static_cast<int>(stencilVal) != lastStencilRef)
			{
				lastStencilRef = static_cast<int>(stencilVal);
				context.SetStencilRef(stencilVal);
			}

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

		if (activePixGroup >= 0)
			context.PIXEndEvent();

		context.PIXEndEvent();
	}
}
