#include "00_Core/PCH.h"
#include "04_Renderer/Passes/Common/GFL2EnvScenePass.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"
#include "03_AssetSystem/Loaders/GFL2/EnvSceneLoader.h"
#include "04_Renderer/Passes/Character/CharacterPassCommon.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "04_Renderer/Renderer/RendererConfig.h"

#include "08_Character/CharacterOpaqueVS.h"
#include "08_Character/EnvScenePS.h"
#include "08_Character/EnvSkyboxPS.h"
#include "08_Character/CharacterBodyPS.h"
#include "08_Character/FloorPlanarReflectionPS.h"

#include <cstdio>

namespace Renderer::GFL2
{
	namespace
	{
		struct EnvPsoSpec
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

		void InitStaticEnvPso(GraphicsPSO& pso, Renderer::RenderServices& services, const EnvPsoSpec& s)
		{
			const DXGI_FORMAT rtvs[2] = { Deferred::LinearHDRBufferFormat, DXGI_FORMAT_R16G16B16A16_FLOAT };
			pso = GraphicsPSO(s.Name);
			pso.SetRootSignature(services.GetBindlessGraphicsRootSignature());
			D3D12_RASTERIZER_DESC rast = Graphics::RasterizerDefault;
			rast.CullMode = s.Cull;
			pso.SetRasterizerState(rast);
			pso.SetBlendState(s.Blend);
			pso.SetDepthStencilState(s.DepthStencil);
			pso.SetSampleMask(UINT_MAX);
			pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
			pso.SetInputLayout(kCharacterInputLayoutCount, kCharacterInputLayout);
			pso.SetRenderTargetFormats(s.NumRtvs, rtvs, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			pso.SetVertexShader(s.VS, s.VSSize);
			pso.SetPixelShader(s.PS, s.PSSize);
			pso.Finalize();
		}

		bool BuildRawEnvPso(ID3D12RootSignature* rs, const EnvPsoSpec& s,
			Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPso, std::string& outErr)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature        = rs;
			d.VS                    = { s.VS, s.VSSize };
			d.PS                    = { s.PS, s.PSSize };
			d.BlendState            = s.Blend;
			d.SampleMask            = UINT_MAX;
			d.RasterizerState       = Graphics::RasterizerDefault;
			d.RasterizerState.CullMode = s.Cull;
			d.DepthStencilState     = s.DepthStencil;
			d.InputLayout           = { kCharacterInputLayout, kCharacterInputLayoutCount };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets      = s.NumRtvs;
			if (s.NumRtvs >= 1u) d.RTVFormats[0] = Deferred::LinearHDRBufferFormat;
			if (s.NumRtvs >= 2u) d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			d.DSVFormat             = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.SampleDesc.Count      = 1;

			const HRESULT hr = Graphics::g_Device->CreateGraphicsPipelineState(&d, TY_IID_PPV_ARGS(&outPso));
			if (FAILED(hr) || outPso == nullptr)
			{
				char buf[128] = {};
				std::snprintf(buf, sizeof(buf), "GFL2EnvScenePass CreatePSO failed (hr=0x%08X)", static_cast<unsigned>(hr));
				outErr = buf;
				return false;
			}
			outPso->SetName(s.Name);
			return true;
		}
	}

	bool GFL2EnvScenePass::Initialize(Renderer::RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!services.IsBindlessSM66Ready())
		{
			LOG_ERROR(L"[GFL2][M6] GFL2EnvScenePass requires SM 6.6 Bindless RootSignature\n");
			return false;
		}

		m_OpaqueVS.assign(CharacterOpaqueVS_cso, CharacterOpaqueVS_cso + sizeof(CharacterOpaqueVS_cso));
		m_EnvPS.assign(EnvScenePS_cso, EnvScenePS_cso + sizeof(EnvScenePS_cso));
		m_SkyboxPS.assign(EnvSkyboxPS_cso, EnvSkyboxPS_cso + sizeof(EnvSkyboxPS_cso));
		m_CharBodyPS.assign(CharacterBodyPS_cso, CharacterBodyPS_cso + sizeof(CharacterBodyPS_cso));
		m_FloorReflPS.assign(FloorPlanarReflectionPS_cso, FloorPlanarReflectionPS_cso + sizeof(FloorPlanarReflectionPS_cso));

		const D3D12_DEPTH_STENCIL_DESC dsEnv = Graphics::DepthStateReadWriteReversed;
		const D3D12_DEPTH_STENCIL_DESC dsRO  = Graphics::DepthStateReadOnlyReversed;

		InitStaticEnvPso(m_PsoEnvCullBack,  services, { L"GFL2 Env CullBack",  m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_BACK,  Graphics::BlendDisable, dsEnv, 2u });
		InitStaticEnvPso(m_PsoEnvCullFront, services, { L"GFL2 Env CullFront", m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_FRONT, Graphics::BlendDisable, dsEnv, 2u });
		InitStaticEnvPso(m_PsoEnvCullNone,  services, { L"GFL2 Env CullNone",  m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_NONE,  Graphics::BlendDisable, dsEnv, 2u });
		InitStaticEnvPso(m_PsoSkybox,       services, { L"GFL2 Skybox",        m_OpaqueVS.data(), m_OpaqueVS.size(), m_SkyboxPS.data(), m_SkyboxPS.size(), D3D12_CULL_MODE_NONE, Graphics::BlendDisable, dsRO, 2u });

		InitStaticEnvPso(m_PsoMirrorCharBack,  services, { L"GFL2 MirrorChar Back",  m_OpaqueVS.data(), m_OpaqueVS.size(), m_CharBodyPS.data(), m_CharBodyPS.size(), D3D12_CULL_MODE_BACK,  Graphics::BlendDisable, dsEnv, 2u });
		InitStaticEnvPso(m_PsoMirrorCharFront, services, { L"GFL2 MirrorChar Front", m_OpaqueVS.data(), m_OpaqueVS.size(), m_CharBodyPS.data(), m_CharBodyPS.size(), D3D12_CULL_MODE_FRONT, Graphics::BlendDisable, dsEnv, 2u });
		InitStaticEnvPso(m_PsoFloorComposite,  services, { L"GFL2 FloorRefl Comp",   m_OpaqueVS.data(), m_OpaqueVS.size(), m_FloorReflPS.data(), m_FloorReflPS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendTraditional, dsRO, 1u });

		m_Initialized = true;
		return true;
	}

	void GFL2EnvScenePass::Shutdown()
	{
		m_HotEnvBack.Reset();
		m_HotEnvFront.Reset();
		m_HotEnvNone.Reset();
		m_HotSkybox.Reset();
		m_HotMirrorBack.Reset();
		m_HotMirrorFront.Reset();
		m_HotFloorComposite.Reset();

		m_OpaqueVS.clear();
		m_EnvPS.clear();
		m_SkyboxPS.clear();
		m_CharBodyPS.clear();
		m_FloorReflPS.clear();
		m_Initialized = false;
	}

	bool GFL2EnvScenePass::RebuildPSOs(Renderer::RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr)
		{
			outErr = "GFL2EnvScenePass not initialized";
			return false;
		}
		ID3D12RootSignature* rs = services.GetBindlessGraphicsRootSignature().GetSignature();
		if (rs == nullptr)
		{
			outErr = "BindlessGraphicsRootSignature is null";
			return false;
		}

		const D3D12_DEPTH_STENCIL_DESC dsEnv = Graphics::DepthStateReadWriteReversed;
		const D3D12_DEPTH_STENCIL_DESC dsRO  = Graphics::DepthStateReadOnlyReversed;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> cEB, cEF, cEN, cSky, cMB, cMF, cFC;
		if (!BuildRawEnvPso(rs, { L"GFL2 Env Hot Back",   m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_BACK,  Graphics::BlendDisable, dsEnv, 2u }, cEB, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 Env Hot Front",  m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_FRONT, Graphics::BlendDisable, dsEnv, 2u }, cEF, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 Env Hot None",   m_OpaqueVS.data(), m_OpaqueVS.size(), m_EnvPS.data(), m_EnvPS.size(), D3D12_CULL_MODE_NONE,  Graphics::BlendDisable, dsEnv, 2u }, cEN, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 Skybox Hot",     m_OpaqueVS.data(), m_OpaqueVS.size(), m_SkyboxPS.data(), m_SkyboxPS.size(), D3D12_CULL_MODE_NONE, Graphics::BlendDisable, dsRO, 2u }, cSky, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 MirrorB Hot",    m_OpaqueVS.data(), m_OpaqueVS.size(), m_CharBodyPS.data(), m_CharBodyPS.size(), D3D12_CULL_MODE_BACK,  Graphics::BlendDisable, dsEnv, 2u }, cMB, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 MirrorF Hot",    m_OpaqueVS.data(), m_OpaqueVS.size(), m_CharBodyPS.data(), m_CharBodyPS.size(), D3D12_CULL_MODE_FRONT, Graphics::BlendDisable, dsEnv, 2u }, cMF, outErr) ||
			!BuildRawEnvPso(rs, { L"GFL2 FloorRefl Hot",  m_OpaqueVS.data(), m_OpaqueVS.size(), m_FloorReflPS.data(), m_FloorReflPS.size(), D3D12_CULL_MODE_BACK, Graphics::BlendTraditional, dsRO, 1u }, cFC, outErr))
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

		RetireSwap(m_HotEnvBack,        std::move(cEB));
		RetireSwap(m_HotEnvFront,       std::move(cEF));
		RetireSwap(m_HotEnvNone,        std::move(cEN));
		RetireSwap(m_HotSkybox,         std::move(cSky));
		RetireSwap(m_HotMirrorBack,     std::move(cMB));
		RetireSwap(m_HotMirrorFront,    std::move(cMF));
		RetireSwap(m_HotFloorComposite, std::move(cFC));
		return true;
	}

	void GFL2EnvScenePass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		auto Reg = [&](const char* id, const wchar_t* path, std::vector<uint8_t>* target)
		{
			Graphics::ShaderHotReloadDesc d;
			d.Id            = id;
			d.SourceRelPath = path;
			d.EntryPoint    = L"MainPS";
			d.TargetProfile = L"ps_6_6";
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

		Reg("EnvScenePS",              L"Engine/Shader/08_Character/EnvScenePS.hlsl",              &m_EnvPS);
		Reg("EnvSkyboxPS",             L"Engine/Shader/08_Character/EnvSkyboxPS.hlsl",             &m_SkyboxPS);
		Reg("FloorPlanarReflectionPS", L"Engine/Shader/08_Character/FloorPlanarReflectionPS.hlsl", &m_FloorReflPS);
	}

	void GFL2EnvScenePass::ExecuteOpaqueAndSkybox(
		GraphicsContext& context,
		const ::GFL2::EnvSceneLoader& envAssets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		const Renderer::RenderParams& params,
		ColorBuffer& linearHDR,
		ColorBuffer& characterAuxMask,
		DepthBuffer& sceneDepth,
		uint32_t mainShadowSrvIdx,
		uint32_t addLight1ShadowSrvIdx,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services)
	{
		const auto& st = envAssets.GetStats();
		if (!m_Initialized || !st.Loaded || !st.MeshLoaded)
			return;

		const auto& metas     = envAssets.GetDrawMeta();
		const auto& draws     = envAssets.GetCpuDrawData();
		const auto& materials = envAssets.GetCpuMaterialData();
		const auto& submeshes = envAssets.GetSubmeshes();

		context.PIXBeginEvent(L"2. EnvOpaqueAndSkyboxPass (186 Static + Skybox)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(characterAuxMask, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);

		::GFL2::GFL2AdditionalLightsCB addCB = envAssets.GetAdditionalLightsCB();
		addCB.ControlParams.x = params.GFL2EnableAdditionalLights ? 1.0f : 0.0f;
		addCB.ControlParams.y = params.GFL2AdditionalLightScale;
		context.SetDynamicConstantBufferView(2, sizeof(addCB), &addCB);

		context.SetViewportAndScissor(viewport, scissor);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv2[2] = { linearHDR.GetRTV(), characterAuxMask.GetRTV() };
		context.SetRenderTargets(2, rtv2, sceneDepth.GetDSV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetVertexBuffer(0, envAssets.GetVertexBufferView());
		context.SetIndexBuffer(envAssets.GetIndexBufferView());

		// 2.1 静态场景不透明建筑/道具 (186 Draws)
		context.PIXBeginEvent(L"Env_StaticArchitecture_Lightmap");
		int lastCull = -1;
		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			if (meta.Category != ::GFL2::EnvDrawCategory::Opaque)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size() || draw.MaterialIndex >= materials.size())
				continue;
			const auto& sub = submeshes[draw.SubmeshIndex];
			const auto& mat = materials[draw.MaterialIndex];

			const int target = static_cast<int>(meta.CullMode);
			if (target != lastCull)
			{
				lastCull = target;
				ID3D12PipelineState* raw = nullptr;
				if (meta.CullMode == ::GFL2::CullModeId::Front)
					raw = m_HotEnvFront ? m_HotEnvFront.Get() : m_PsoEnvCullFront.GetPipelineStateObject();
				else if (meta.CullMode == ::GFL2::CullModeId::None)
					raw = m_HotEnvNone ? m_HotEnvNone.Get() : m_PsoEnvCullNone.GetPipelineStateObject();
				else
					raw = m_HotEnvBack ? m_HotEnvBack.Get() : m_PsoEnvCullBack.GetPipelineStateObject();
				context.SetPipelineStateRaw(raw);
			}

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex             = static_cast<uint32_t>(di);
			rc.MaterialIndex         = draw.MaterialIndex;
			rc.DrawDataSrvIdx        = st.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx    = st.MaterialDataSrvIdx;
			rc.MainShadowMapSrvIdx   = mainShadowSrvIdx;
			rc.ReflectionProbeSrvIdx = mat.ReflectionProbeIdx;
			rc.AuxTex0SrvIdx         = addLight1ShadowSrvIdx;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
		}
		context.PIXEndEvent();

		// 2.2 天空盒 (EID 3604, 只读深度 GREATER_EQUAL)
		context.PIXBeginEvent(L"Env_Skybox_5687");
		for (size_t di = 0; di < metas.size(); ++di)
		{
			const auto& meta = metas[di];
			if (meta.Category != ::GFL2::EnvDrawCategory::Skybox)
				continue;

			const auto& draw = draws[di];
			if (draw.SubmeshIndex >= submeshes.size())
				continue;
			const auto& sub = submeshes[draw.SubmeshIndex];

			context.SetRenderTargets(2, rtv2, sceneDepth.GetDSV_DepthReadOnly());
			context.SetPipelineStateRaw(m_HotSkybox ? m_HotSkybox.Get() : m_PsoSkybox.GetPipelineStateObject());

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex          = static_cast<uint32_t>(di);
			rc.MaterialIndex      = draw.MaterialIndex;
			rc.DrawDataSrvIdx     = st.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx = st.MaterialDataSrvIdx;
			rc.AuxTex0SrvIdx      = st.SkyboxTexSrvIdx;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
		}
		context.PIXEndEvent();

		context.PIXEndEvent();
	}

	void GFL2EnvScenePass::ExecuteFloorReflection(
		GraphicsContext& context,
		const ::GFL2::EnvSceneLoader& envAssets,
		const ::GFL2::CharacterManifestLoader& charAssets,
		const ::GFL2::GFL2FrameConstants& frameCB,
		ColorBuffer& floorMirrorRT,
		DepthBuffer& floorMirrorDepth,
		ColorBuffer& linearHDR,
		DepthBuffer& sceneDepth,
		uint32_t mainShadowSrvIdx,
		const D3D12_VIEWPORT& viewport,
		const D3D12_RECT& scissor,
		Renderer::RenderServices& services,
		float reflectionStrength)
	{
		const auto& est = envAssets.GetStats();
		const auto& cst = charAssets.GetStats();
		if (!m_Initialized || !est.Loaded || !est.MeshLoaded || !cst.Loaded || !cst.MeshLoaded)
			return;

		// 寻找 FloorPlanarReflection (EID 3843)
		int floorIdx = -1;
		const auto& envMetas = envAssets.GetDrawMeta();
		for (size_t i = 0; i < envMetas.size(); ++i)
		{
			if (envMetas[i].Category == ::GFL2::EnvDrawCategory::FloorMirror)
			{
				floorIdx = static_cast<int>(i);
				break;
			}
		}
		if (floorIdx < 0)
			return;

		context.PIXBeginEvent(L"3. PlanarFloorReflection (MirrorChar + EID3843)");

		if (context.GetSetDescriptorHeapsCount() == 0)
			services.GetDescriptorHeap().BindToContext(context);

		// 3.1 镜像平面相机渲染角色倒影至 FloorMirrorRT
		context.PIXBeginEvent(L"MirrorCamera_RenderAK12");
		context.TransitionResource(floorMirrorRT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(floorMirrorDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context.FlushResourceBarriers();

		context.ClearColor(floorMirrorRT);
		context.ClearDepth(floorMirrorDepth);

		const float floorZ = est.FloorPlaneZ;
		const DirectX::XMMATRIX reflectMat(
			DirectX::XMVectorSet(1.0f, 0.0f,  0.0f, 0.0f),
			DirectX::XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),
			DirectX::XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
			DirectX::XMVectorSet(0.0f, 0.0f, 2.0f * floorZ, 1.0f));
		::GFL2::GFL2FrameConstants mirrorCB = frameCB;
		mirrorCB.ViewProj = Math::Matrix4(DirectX::XMMatrixMultiply(reflectMat, static_cast<DirectX::XMMATRIX>(frameCB.ViewProj)));
		mirrorCB.CameraPosAndNear.z = 2.0f * floorZ - frameCB.CameraPosAndNear.z;

		const D3D12_VIEWPORT mirrorVp = {
			0.0f, 0.0f,
			static_cast<float>(floorMirrorRT.GetWidth()),
			static_cast<float>(floorMirrorRT.GetHeight()),
			0.0f, 1.0f
		};
		const D3D12_RECT mirrorSc = {
			0, 0,
			static_cast<LONG>(floorMirrorRT.GetWidth()),
			static_cast<LONG>(floorMirrorRT.GetHeight())
		};

		context.SetRootSignature(services.GetBindlessGraphicsRootSignature());
		context.SetDynamicConstantBufferView(1, sizeof(mirrorCB), &mirrorCB);
		context.SetViewportAndScissor(mirrorVp, mirrorSc);
		context.SetRenderTarget(floorMirrorRT.GetRTV(), floorMirrorDepth.GetDSV());
		context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context.SetVertexBuffer(0, charAssets.GetVertexBufferView());
		context.SetIndexBuffer(charAssets.GetIndexBufferView());

		const auto& cMetas = charAssets.GetDrawMeta();
		const auto& cDraws = charAssets.GetCpuDrawData();
		const auto& cMats  = charAssets.GetCpuMaterialData();
		const auto& cSubs  = charAssets.GetSubmeshes();

		for (size_t di = 0; di < cMetas.size(); ++di)
		{
			const auto& cm = cMetas[di];
			if (cm.ShadingModelId == ::GFL2::ShadingModel::Outline ||
				cm.ShadingModelId == ::GFL2::ShadingModel::Transparent ||
				cm.ShadingModelId == ::GFL2::ShadingModel::EyeAdd ||
				cm.ShadingModelId == ::GFL2::ShadingModel::EyeMul ||
				cm.EventId == 3198u || cm.EventId == 3295u || cm.EventId == 3335u)
			{
				continue;
			}
			const auto& cd = cDraws[di];
			if (cd.SubmeshIndex >= cSubs.size())
				continue;
			const auto& sub = cSubs[cd.SubmeshIndex];
			const auto& mat = cMats[cd.MaterialIndex];

			// 反射矩阵翻转绕序：原 CullBack -> 用 CullFront，原 CullFront -> 用 CullBack
			const bool useBackPso = (cm.CullMode == ::GFL2::CullModeId::Front);
			context.SetPipelineStateRaw(useBackPso
				? (m_HotMirrorBack  ? m_HotMirrorBack.Get()  : m_PsoMirrorCharBack.GetPipelineStateObject())
				: (m_HotMirrorFront ? m_HotMirrorFront.Get() : m_PsoMirrorCharFront.GetPipelineStateObject()));

			::GFL2::GFL2RootConstants rc = {};
			rc.DrawIndex             = static_cast<uint32_t>(di);
			rc.MaterialIndex         = cd.MaterialIndex;
			rc.DrawDataSrvIdx        = cst.DrawDataSrvIdx;
			rc.MaterialDataSrvIdx    = cst.MaterialDataSrvIdx;
			rc.MainShadowMapSrvIdx   = mainShadowSrvIdx;
			rc.ReflectionProbeSrvIdx = mat.ReflectionProbeIdx;
			context.SetConstantArray(0, 16, &rc);

			context.DrawIndexed(sub.IndexCount, sub.StartIndex, static_cast<INT>(sub.BaseVertex));
		}
		context.PIXEndEvent();

		// 3.2 地板混合合成 (EID 3843)
		context.PIXBeginEvent(L"FloorPlanarReflection_Composite_EID3843");
		context.TransitionResource(floorMirrorRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.TransitionResource(linearHDR, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.TransitionResource(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_READ);
		context.FlushResourceBarriers();

		const uint32_t mirrorSrvIdx = services.GetDescriptorHeap().StageTransientDescriptor(floorMirrorRT.GetSRV()).HeapIndex;
		const auto& fDraw = envAssets.GetCpuDrawData()[floorIdx];
		const auto& fSub  = envAssets.GetSubmeshes()[fDraw.SubmeshIndex];

		context.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);
		context.SetViewportAndScissor(viewport, scissor);
		context.SetRenderTarget(linearHDR.GetRTV(), sceneDepth.GetDSV_DepthReadOnly());
		context.SetPipelineStateRaw(m_HotFloorComposite ? m_HotFloorComposite.Get() : m_PsoFloorComposite.GetPipelineStateObject());
		context.SetVertexBuffer(0, envAssets.GetVertexBufferView());
		context.SetIndexBuffer(envAssets.GetIndexBufferView());

		::GFL2::GFL2RootConstants rc = {};
		rc.DrawIndex          = static_cast<uint32_t>(floorIdx);
		rc.MaterialIndex      = fDraw.MaterialIndex;
		rc.DrawDataSrvIdx     = est.DrawDataSrvIdx;
		rc.MaterialDataSrvIdx = est.MaterialDataSrvIdx;
		rc.AuxTex0SrvIdx      = mirrorSrvIdx;
		rc.PassParam0         = reflectionStrength;
		context.SetConstantArray(0, 16, &rc);

		context.DrawIndexed(fSub.IndexCount, fSub.StartIndex, static_cast<INT>(fSub.BaseVertex));
		context.PIXEndEvent();

		context.PIXEndEvent();
	}
}
