#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Common/GFL2ShadowPass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

#include "08_Character/GFL2ShadowDepthVS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	namespace
	{
		const D3D12_INPUT_ELEMENT_DESC kShadowCharacterInputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
	}

	bool GFL2ShadowPass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M2] GFL2ShadowPass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_CurrentVSBytecode.assign(GFL2ShadowDepthVS_cso, GFL2ShadowDepthVS_cso + sizeof(GFL2ShadowDepthVS_cso));

		auto ConfigureShadowPSO = [&](GraphicsPSO& pso, const wchar_t* name, D3D12_CULL_MODE cull)
		{
			pso = GraphicsPSO(name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());

			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode              = cull;
			// Reverse-Z 下由接收面 Normal+Depth Bias 主导，保持 0 硬件斜率偏置避免阴影脱离
			rast.DepthBias             = 0;
			rast.DepthBiasClamp        = 0.0f;
			rast.SlopeScaledDepthBias  = 0.0f;
			rast.DepthClipEnable       = FALSE;
			pso.SetRasterizerState(rast);

			pso.SetBlendState(Graphics::BlendNoColorWrite);
			pso.SetDepthStencilState(Graphics::DepthStateReadWriteReversed);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(_countof(kShadowCharacterInputLayout), kShadowCharacterInputLayout);
			pso.SetDepthTargetFormat(DXGI_FORMAT_D32_FLOAT);
			pso.SetVertexShader(m_CurrentVSBytecode.data(), m_CurrentVSBytecode.size());
			pso.Finalize();
		};

		ConfigureShadowPSO(m_PSO_CullBack, L"GFL2ShadowPass CullBack ReverseZ PSO", D3D12_CULL_MODE_BACK);
		ConfigureShadowPSO(m_PSO_CullNone, L"GFL2ShadowPass CullNone ReverseZ PSO", D3D12_CULL_MODE_NONE);

		m_Initialized = true;
		return true;
	}

	void GFL2ShadowPass::Shutdown()
	{
		m_HotPSO_Back.Reset();
		m_HotPSO_None.Reset();
		m_CurrentVSBytecode.clear();
		m_Initialized = false;
	}

	bool GFL2ShadowPass::RebuildPSOs(const void* vsBytecode, size_t vsSize, Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || vsBytecode == nullptr || vsSize == 0)
		{
			outErr = "GFL2ShadowPass not initialized or empty VS bytecode";
			return false;
		}
		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		auto BuildOne = [&](D3D12_CULL_MODE cull, const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso) -> bool
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { vsBytecode, vsSize };
			d.BlendState            = Graphics::BlendNoColorWrite;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode             = cull;
			d.RasterizerState.DepthBias            = 0;
			d.RasterizerState.DepthBiasClamp       = 0.0f;
			d.RasterizerState.SlopeScaledDepthBias = 0.0f;
			d.RasterizerState.DepthClipEnable      = FALSE;
			d.DepthStencilState     = Graphics::DepthStateReadWriteReversed;
			d.InputLayout           = { kShadowCharacterInputLayout, _countof(kShadowCharacterInputLayout) };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets      = 0;
			d.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
			d.SampleDesc.Count      = 1;

			const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "Shadow CreateGraphicsPipelineState failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(name);
			return true;
		};

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candBack, candNone;
		if (!BuildOne(D3D12_CULL_MODE_BACK, L"GFL2Shadow Hot Back", candBack) ||
			!BuildOne(D3D12_CULL_MODE_NONE, L"GFL2Shadow Hot None", candNone))
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
		RetireAndSwap(m_HotPSO_Back, std::move(candBack));
		RetireAndSwap(m_HotPSO_None, std::move(candNone));
		return true;
	}

	void GFL2ShadowPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		Graphics::ShaderHotReloadDesc desc;
		desc.Id            = "GFL2ShadowDepthVS";
		desc.SourceRelPath = L"Engine/Shader/08_Character/GFL2ShadowDepthVS.hlsl";
		desc.EntryPoint    = L"MainVS";
		desc.TargetProfile = L"vs_6_6";
		desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) -> bool
		{
			if (!RebuildPSOs(dxil.data(), dxil.size(), services, outErr))
				return false;
			m_CurrentVSBytecode = dxil;
			return true;
		};
		reloader.Register(std::move(desc));
	}

	void GFL2ShadowPass::Execute(
		GraphicsContext& context,
		const ::GFL2::CharacterManifestLoader& assets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		DepthBuffer& shadowMap,
		Renderer::RenderServices& services,
		const wchar_t* pixSubLabel)
	{
		const auto& st = assets.GetStats();
		if (!m_Initialized || !st.Loaded || !st.MeshLoaded)
			return;

		const auto& metas     = assets.GetDrawMeta();
		const auto& draws     = assets.GetCpuDrawData();
		const auto& submeshes = assets.GetSubmeshes();
		if (metas.empty() || submeshes.empty())
			return;

		context.PIXBeginEvent(L"1. GFL2ShadowPass");
		context.PIXBeginEvent(pixSubLabel ? pixSubLabel : L"ShadowMap");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(shadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();
		context.ClearDepth(shadowMap);

		const uint32_t res = std::max(shadowMap.GetWidth(), 1u);
		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f };
		const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);

		context.SetViewportAndScissor(vp, sc);
		context.SetDepthStencilTarget(shadowMap.GetDSV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetVertexBuffer(0, assets.GetVertexBufferView());
		context.SetIndexBuffer(assets.GetIndexBufferView());

		int lastCull = -1;
		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			// 与 EID 2788..3178 实体 Caster 对齐，跳过描边 / Stencil 阴影重绘 / 后置半透明叠加
			const bool isCaster =
				(meta.ShadingModelId == ::GFL2::ShadingModel::Body) ||
				(meta.ShadingModelId == ::GFL2::ShadingModel::Cloth) ||
				(meta.ShadingModelId == ::GFL2::ShadingModel::Weapon) ||
				(meta.ShadingModelId == ::GFL2::ShadingModel::Nails) ||
				(meta.EventId == 3031u || meta.EventId == 3062u || meta.EventId == 3100u ||
				 meta.EventId == 3137u || meta.EventId == 3178u);

			if (!isCaster)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size())
				continue;
			const auto& sub = submeshes[draw.SubmeshIndex];

			const bool wantNone = (meta.CullMode == ::GFL2::CullModeId::None);
			const int targetMode = wantNone ? 1 : 0;
			if (targetMode != lastCull)
			{
				lastCull = targetMode;
				ID3D12PipelineState* raw = wantNone
					? (m_HotPSO_None ? m_HotPSO_None.Get() : m_PSO_CullNone.GetPipelineStateObject())
					: (m_HotPSO_Back ? m_HotPSO_Back.Get() : m_PSO_CullBack.GetPipelineStateObject());
				context.SetPipelineStateRaw(raw);
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
		context.PIXEndEvent();
	}
}
