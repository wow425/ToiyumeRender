#include "00_Core/PCH.h"
#include "04_Renderer/Renderer/GFL2/GFL2CharacterPipeline.h"

#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/GFL2MathConvert.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "05_Scene/Camera/Camera.h"

#include <algorithm>

namespace Renderer::GFL2
{
	AlgorithmCapsReport GFL2CharacterPipeline::CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const
	{
		AlgorithmCapsReport report;
		if (!caps.SupportsBindlessSM66)
		{
			report.Supported = false;
			report.UnsupportedReason = "GFL2CharacterPipeline requires SM 6.6 + ResourceBinding Tier 3 Bindless";
			return report;
		}
		report.Supported = true;
		report.EstimatedVramBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 48ull;
		return report;
	}

	bool GFL2CharacterPipeline::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)
			return true;

		if (desc.simulateInitFailure)
			return false;

		if (!AttachOrCreateServices(desc))
			return false;

		if (!m_Services->IsBindlessSM66Ready())
		{
			ReleaseServicesIfOwned();
			return false;
		}

		m_CreateDesc   = desc;
		m_FrameCounter = 0;
		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };

		CreatePipelineTargets();

		if (!m_TonemapPass.Initialize(Deferred::SceneColorBufferFormat, m_Services) ||
			!m_Assets.LoadAK12(*m_Services) ||
			!m_ShadowPass.Initialize(*m_Services) ||
			!m_OpaquePass.Initialize(*m_Services) ||
			!m_FaceHairEyePass.Initialize(*m_Services) ||
			!m_OutlinePass.Initialize(*m_Services) ||
			!m_TransparentPass.Initialize(*m_Services) ||
			!m_EnvPass.Initialize(*m_Services) ||
			!m_PostPass.Initialize(*m_Services) ||
			!m_DebugOverlayPass.Initialize(*m_Services))
		{
			LOG_ERROR(L"[GFL2][M4-M6] One or more GFL2CharacterPipeline sub-passes failed to initialize\n");
			Shutdown();
			return false;
		}

		// M6：加载整备室环境、附加光与 7281 3D LUT（共享 5682 ReflectionProbe6）
		const uint32_t probe5682Idx = m_Assets.FindTextureBindlessIdx(5682u);
		if (!m_EnvAssets.LoadEnv(*m_Services, probe5682Idx))
			LOG_INFO(L"[GFL2][M6] EnvSceneLoader non-fatal warning: environment assets unavailable\n");

		m_ShadowMapMain = std::make_shared<DepthBuffer>(0.0f, 0);
		m_ShadowMapMain->Create(
			L"GFL2 ShadowMapMain 4096",
			GFL2ShadowPass::kMainShadowResolution,
			GFL2ShadowPass::kMainShadowResolution,
			DXGI_FORMAT_D32_FLOAT);

		m_ShadowMapAdd1 = std::make_shared<DepthBuffer>(0.0f, 0);
		m_ShadowMapAdd1->Create(L"GFL2 ShadowMapAdd1 512", 512u, 512u, DXGI_FORMAT_D32_FLOAT);

		m_Initialized = true;
		LOG_INFO(L"[GFL2][M4-M6] GFL2CharacterPipeline initialized (%ux%u, M0-M6 Full Pass Pipeline Ready)\n",
			desc.width, desc.height);
		return true;
	}

	void GFL2CharacterPipeline::Shutdown()
	{
		m_DebugOverlayPass.Shutdown();
		m_PostPass.Shutdown();
		m_EnvPass.Shutdown();
		m_TransparentPass.Shutdown();
		m_OutlinePass.Shutdown();
		m_FaceHairEyePass.Shutdown();
		m_OpaquePass.Shutdown();
		m_ShadowPass.Shutdown();
		m_EnvAssets.Shutdown(m_Services);
		m_Assets.Shutdown(m_Services);
		m_TonemapPass.Shutdown();

		if (m_ShadowMapMain) { m_ShadowMapMain->Destroy(); m_ShadowMapMain.reset(); }
		if (m_ShadowMapAdd1) { m_ShadowMapAdd1->Destroy(); m_ShadowMapAdd1.reset(); }
		DestroyPipelineTargets();
		ReleaseServicesIfOwned();
		m_Initialized = false;
	}

	void GFL2CharacterPipeline::OnResize(uint32_t width, uint32_t height)
	{
		if (!m_Initialized || width == 0 || height == 0)
			return;

		m_CreateDesc.width  = width;
		m_CreateDesc.height = height;
		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

		DestroyPipelineTargets();
		CreatePipelineTargets();
		m_PostPass.ResetHistory();
	}

	void GFL2CharacterPipeline::CreatePipelineTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0)
			return;

		const uint32_t w = m_CreateDesc.width;
		const uint32_t h = m_CreateDesc.height;

		m_Buffers = std::make_shared<GFL2PipelineBuffers>();

		const Color hdrClear(0.03f, 0.045f, 0.06f, 1.0f);
		m_Buffers->LinearHDRBuffer = Graphics::BufferManager::CreateColorBuffer(
			L"GFL2 LinearHDRColor", w, h, Deferred::LinearHDRBufferFormat, 1, &hdrClear);

		const Color auxClear(0.0f, 0.0f, 0.0f, 0.0f);
		m_Buffers->CharacterAuxMask = Graphics::BufferManager::CreateColorBuffer(
			L"GFL2 CharacterAuxMask", w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, &auxClear);

		const uint32_t mw = (std::max)(w / 2u, 1u);
		const uint32_t mh = (std::max)(h / 2u, 1u);
		const Color mirrorClear(0.0f, 0.0f, 0.0f, 0.0f);
		m_Buffers->FloorMirrorRT = Graphics::BufferManager::CreateColorBuffer(
			L"GFL2 FloorMirrorRT", mw, mh, Deferred::LinearHDRBufferFormat, 1, &mirrorClear);
		m_Buffers->FloorMirrorDepth = std::make_shared<DepthBuffer>(0.0f, 0);
		m_Buffers->FloorMirrorDepth->Create(L"GFL2 FloorMirrorDepth", mw, mh, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);

		m_Buffers->SceneColorBuffer = Graphics::BufferManager::CreateColorBuffer(
			L"GFL2 SceneColorLDR", w, h, Deferred::SceneColorBufferFormat);

		m_Buffers->SceneDepthBuffer = std::make_shared<DepthBuffer>(0.0f, 0);
		m_Buffers->SceneDepthBuffer->Create(L"GFL2 SceneDepthReverseZ", w, h, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);

		m_Buffers->PostTargets.Create(w, h);
	}

	void GFL2CharacterPipeline::DestroyPipelineTargets()
	{
		if (m_Buffers)
		{
			m_Buffers->PostTargets.Destroy();
			Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 LinearHDRColor", m_Buffers->LinearHDRBuffer);
			Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 CharacterAuxMask", m_Buffers->CharacterAuxMask);
			Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 FloorMirrorRT", m_Buffers->FloorMirrorRT);
			if (m_Buffers->FloorMirrorDepth)
				m_Buffers->FloorMirrorDepth->Destroy();
			Graphics::BufferManager::ReleaseColorBuffer(L"GFL2 SceneColorLDR", m_Buffers->SceneColorBuffer);
			if (m_Buffers->SceneDepthBuffer)
				m_Buffers->SceneDepthBuffer->Destroy();
		}
		m_Buffers.reset();
	}

	void GFL2CharacterPipeline::RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		m_ShadowPass.RegisterHotReload(reloader, services);
		m_EnvPass.RegisterHotReload(reloader, services);
		m_OpaquePass.RegisterHotReload(reloader, services);
		m_FaceHairEyePass.RegisterHotReload(reloader, services);
		m_OutlinePass.RegisterHotReload(reloader, services);
		m_TransparentPass.RegisterHotReload(reloader, services);
		m_PostPass.RegisterHotReload(reloader, services);
		m_DebugOverlayPass.RegisterHotReload(reloader, services);
		m_TonemapPass.RegisterHotReload(reloader, services);
	}

	void GFL2CharacterPipeline::ResetHistory()
	{
		m_PostPass.ResetHistory();
	}

	void GFL2CharacterPipeline::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;
		if (!m_Initialized)
			return;
		AdvanceOwnedServicesFrame();
	}

	void GFL2CharacterPipeline::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		(void)frame;
		(void)gfxContext;
	}

	void GFL2CharacterPipeline::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		(void)pass;
		(void)batchType;
		if (!m_Initialized || !m_Buffers)
			return;

		const RenderParams activeParams = (frame.Params != nullptr) ? *frame.Params : RenderParams{};
		ColorBuffer* hdr        = m_Buffers->LinearHDRBuffer.get();
		ColorBuffer* aux        = m_Buffers->CharacterAuxMask.get();
		ColorBuffer* mirrorRT   = m_Buffers->FloorMirrorRT.get();
		DepthBuffer* mirrorDep  = m_Buffers->FloorMirrorDepth.get();
		ColorBuffer* ldr        = m_Buffers->SceneColorBuffer.get();
		DepthBuffer* depth      = m_Buffers->SceneDepthBuffer.get();
		DepthBuffer* shadowMap  = m_ShadowMapMain.get();
		DepthBuffer* addShadow1 = m_ShadowMapAdd1.get();
		if (!hdr || !aux || !ldr || !depth)
			return;

		::GFL2::GFL2FrameConstants frameCB = {};
		if (frame.Camera != nullptr)
		{
			frameCB.ViewProj    = frame.Camera->GetViewProjMatrix();
			frameCB.InvViewProj = frame.Camera->GetInViewProjMatrix();
			frameCB.View        = frame.Camera->GetViewMatrix();
			frameCB.Proj        = frame.Camera->GetProjMatrix();
			const Math::Vector3 pos  = frame.Camera->GetPosition();
			const Math::Vector3 look = frame.Camera->GetLook();
			frameCB.CameraPosAndNear = { static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()), 0.10f };
			frameCB.CameraFwdAndFar  = { static_cast<float>(look.GetX()), static_cast<float>(look.GetY()), static_cast<float>(look.GetZ()), 1000.0f };
		}
		else
		{
			frameCB.ViewProj    = Math::Matrix4(Math::kIdentity);
			frameCB.InvViewProj = Math::Matrix4(Math::kIdentity);
			frameCB.View        = Math::Matrix4(Math::kIdentity);
			frameCB.Proj        = Math::Matrix4(Math::kIdentity);
		}

		DirectX::XMFLOAT3 dirToLightUE5 = { -0.738529f, 0.560575f, 0.374607f };
		{
			Math::Vector3 sunProp(activeParams.SunDirection[0], activeParams.SunDirection[1], activeParams.SunDirection[2]);
			const float lenSq = static_cast<float>(Math::LengthSquare(sunProp));
			if (lenSq > 1e-8f)
				sunProp = sunProp * Math::RecipSqrt(Math::Scalar(lenSq));
			else
				sunProp = Math::Vector3(0.738529f, -0.560575f, -0.374607f);
			dirToLightUE5 = {
				-static_cast<float>(sunProp.GetX()),
				-static_cast<float>(sunProp.GetY()),
				-static_cast<float>(sunProp.GetZ())
			};
			frameCB.MainLightDirToLightAndInt = { dirToLightUE5.x, dirToLightUE5.y, dirToLightUE5.z, activeParams.SunIntensity };
			frameCB.MainLightColorAndShadow   = { activeParams.SunColor[0], activeParams.SunColor[1], activeParams.SunColor[2], activeParams.GFL2ShadowStrength };
		}

		const auto& st = m_Assets.GetStats();
		frameCB.MainLightViewProj = ::GFL2::MathConv::ComputeUE5DirLightShadowMatrix(
			st.WorldBoundsMin, st.WorldBoundsMax, dirToLightUE5, GFL2ShadowPass::kMainShadowResolution);

		const float w = (m_MainViewport.Width > 0.0f) ? m_MainViewport.Width : 1280.0f;
		const float h = (m_MainViewport.Height > 0.0f) ? m_MainViewport.Height : 720.0f;
		frameCB.ScreenSizeAndInv = { w, h, 1.0f / w, 1.0f / h };
		frameCB.ToggleFlags0     = {
			static_cast<float>(static_cast<uint32_t>(activeParams.Debug)),
			activeParams.GFL2EnableRamp ? 1.0f : 0.0f,
			activeParams.GFL2EnableSkinSHDCNorm ? 1.0f : 0.0f,
			activeParams.GFL2EnableIBL ? activeParams.IBLIntensity : 0.0f
		};
		frameCB.ToggleFlags1 = {
			activeParams.GFL2OutlineWidthScale,
			activeParams.GFL2OutlineZBias,
			activeParams.GFL2OutlineMinPx,
			activeParams.GFL2OutlineMaxPx
		};
		frameCB.ToggleFlags2 = {
			activeParams.GFL2FaceSdfSmooth,
			activeParams.GFL2HairSpecIntensity,
			activeParams.GFL2HairViewZShift,
			activeParams.GFL2BangsOverEyeAlpha
		};
		frameCB.ShadowBiasParams = {
			activeParams.GFL2ShadowNormalBias,
			activeParams.GFL2ShadowDepthBias,
			activeParams.GFL2ShadowPCFRadius,
			(activeParams.GFL2EnableShadow && shadowMap != nullptr) ? 1.0f : 0.0f
		};

		m_Blackboard.Reset();
		m_Blackboard.Core.LinearHDRColor = hdr;
		m_Blackboard.Core.SceneColorLDR  = ldr;
		m_Blackboard.Core.SceneDepth     = depth;
		m_Blackboard.Set(FrameResourceKey::LinearHDRColor, hdr);
		m_Blackboard.Set(FrameResourceKey::SceneColorLDR, ldr);
		m_Blackboard.Set(FrameResourceKey::SceneDepth, depth);

		m_PassList.Reset();

		// Pass 0：Clear Reverse-Z 深度模板 (Depth=0.0f, Stencil=0) + LinearHDR + CharacterAuxMask
		m_PassList.AddPass("ClearTargets_ReverseZ",
			{
				{ hdr,   PassAccess::WriteRTV },
				{ aux,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			},
			[hdr, aux, depth](GraphicsContext& ctx)
			{
				ctx.ClearColor(*hdr);
				ctx.ClearColor(*aux);
				ctx.ClearDepthAndStencil(*depth);
			});

		const bool pipeOn     = activeParams.EnableGFL2CharacterPipeline;
		const bool doShadow   = pipeOn && activeParams.GFL2EnableShadow && m_ShadowPass.IsInitialized() && (shadowMap != nullptr);
		const bool envReady   = m_EnvAssets.GetStats().Loaded && m_EnvPass.IsInitialized();
		const bool doEnv      = pipeOn && activeParams.GFL2EnableEnvScene && envReady;
		const bool doAddSh    = doShadow && doEnv && activeParams.GFL2EnableAdditionalLights && (addShadow1 != nullptr);
		const bool isFinalDbg = (activeParams.Debug == DebugView::Final);

		// Pass 1a：主光 4096x4096 Reverse-Z Shadow
		if (doShadow)
		{
			m_PassList.AddPass("GFL2ShadowPass_Main",
				{ { shadowMap, PassAccess::WriteDepth } },
				[this, shadowMap, frameCB](GraphicsContext& ctx)
				{
					m_ShadowPass.Execute(ctx, m_Assets, frameCB, *shadowMap, *m_Services, L"MainLight_Shadow_4096 (ReverseZ)");
				});
		}

		// Pass 1b：附加顶灯 Light[1] 512x512 Shadow
		if (doAddSh)
		{
			::GFL2::GFL2FrameConstants addCB = frameCB;
			addCB.MainLightViewProj = m_EnvAssets.GetLight1ShadowViewProj();
			m_PassList.AddPass("GFL2ShadowPass_AddLight1",
				{ { addShadow1, PassAccess::WriteDepth } },
				[this, addShadow1, addCB](GraphicsContext& ctx)
				{
					m_ShadowPass.Execute(ctx, m_Assets, addCB, *addShadow1, *m_Services, L"AdditionalLight1_Shadow_512");
				});
		}

		// Pass 2：整备室静态场景 + 天空盒 (AuxMask.a = 0.0 -> 接受 SSAO)
		if (doEnv)
		{
			std::vector<ResourceAccess> resUses = {
				{ hdr,   PassAccess::WriteRTV },
				{ aux,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			};
			if (doShadow) resUses.push_back({ shadowMap, PassAccess::ReadSRV_Pixel });
			if (doAddSh)  resUses.push_back({ addShadow1, PassAccess::ReadSRV_Pixel });

			m_PassList.AddPass("EnvOpaqueAndSkyboxPass",
				std::move(resUses),
				[this, hdr, aux, depth, shadowMap, addShadow1, doShadow, doAddSh, frameCB, activeParams](GraphicsContext& ctx)
				{
					const uint32_t mainSrv = doShadow
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(shadowMap->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					const uint32_t addSrv = doAddSh
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(addShadow1->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					m_EnvPass.ExecuteOpaqueAndSkybox(
						ctx, m_EnvAssets, frameCB, activeParams, *hdr, *aux, *depth,
						mainSrv, addSrv, m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 3：地板平面镜面反射 (先镜像渲染 AK-12，再在场景地板前/后合成)
		if (doEnv && activeParams.GFL2EnableFloorReflection && mirrorRT != nullptr && mirrorDep != nullptr)
		{
			std::vector<ResourceAccess> resUses = {
				{ mirrorRT,  PassAccess::WriteRTV },
				{ mirrorDep, PassAccess::WriteDepth },
				{ hdr,       PassAccess::WriteRTV },
				{ depth,     PassAccess::ReadDepthOnly },
			};
			if (doShadow) resUses.push_back({ shadowMap, PassAccess::ReadSRV_Pixel });

			m_PassList.AddPass("FloorPlanarReflectionPass",
				std::move(resUses),
				[this, mirrorRT, mirrorDep, hdr, depth, shadowMap, doShadow, frameCB](GraphicsContext& ctx)
				{
					const uint32_t mainSrv = doShadow
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(shadowMap->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					m_EnvPass.ExecuteFloorReflection(
						ctx, m_EnvAssets, m_Assets, frameCB, *mirrorRT, *mirrorDep, *hdr, *depth,
						mainSrv, m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 4：M3 角色主体不透明几何 (Body/Cloth/Nails/Weapon, Write Stencil=200)
		if (pipeOn && m_OpaquePass.IsInitialized())
		{
			std::vector<ResourceAccess> resUses = {
				{ hdr,   PassAccess::WriteRTV },
				{ aux,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			};
			if (doShadow) resUses.push_back({ shadowMap, PassAccess::ReadSRV_Pixel });

			m_PassList.AddPass("CharacterOpaquePass",
				std::move(resUses),
				[this, hdr, aux, depth, shadowMap, doShadow, frameCB](GraphicsContext& ctx)
				{
					const uint32_t shadowSrv = doShadow
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(shadowMap->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					m_OpaquePass.Execute(
						ctx, m_Assets, frameCB, *hdr, *aux, *depth, shadowSrv,
						m_MainViewport, m_MainScissor, *m_Services, false);
				});
		}

		// Pass 5：M4 面/发/眼特化 + 4 态 Stencil (200/204/203/201)
		if (pipeOn && m_FaceHairEyePass.IsInitialized())
		{
			std::vector<ResourceAccess> resUses = {
				{ hdr,   PassAccess::WriteRTV },
				{ aux,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			};
			if (doShadow) resUses.push_back({ shadowMap, PassAccess::ReadSRV_Pixel });

			m_PassList.AddPass("CharacterFaceHairEyePass",
				std::move(resUses),
				[this, hdr, aux, depth, shadowMap, doShadow, frameCB, activeParams](GraphicsContext& ctx)
				{
					const uint32_t shadowSrv = doShadow
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(shadowMap->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					m_FaceHairEyePass.Execute(
						ctx, m_Assets, frameCB, activeParams, *hdr, *aux, *depth, shadowSrv,
						m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 6：M5 背面平滑法线挤出描边 (19 Draws, 3577 Back NotEqual 204)
		if (pipeOn && activeParams.GFL2EnableOutline && m_OutlinePass.IsInitialized())
		{
			m_PassList.AddPass("CharacterOutlinePass",
				{
					{ hdr,   PassAccess::WriteRTV },
					{ aux,   PassAccess::WriteRTV },
					{ depth, PassAccess::WriteDepth },
				},
				[this, hdr, aux, depth, frameCB](GraphicsContext& ctx)
				{
					m_OutlinePass.Execute(ctx, m_Assets, frameCB, *hdr, *aux, *depth, m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 7：M5 5 组双面半透明件 (CullFront -> CullBack, 高光不衰减)
		if (pipeOn && activeParams.GFL2EnableTransparent && m_TransparentPass.IsInitialized())
		{
			std::vector<ResourceAccess> resUses = {
				{ hdr,   PassAccess::WriteRTV },
				{ aux,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			};
			if (doShadow) resUses.push_back({ shadowMap, PassAccess::ReadSRV_Pixel });

			m_PassList.AddPass("CharacterTransparentPass",
				std::move(resUses),
				[this, hdr, aux, depth, shadowMap, doShadow, frameCB](GraphicsContext& ctx)
				{
					const uint32_t shadowSrv = doShadow
						? m_Services->GetDescriptorHeap().StageTransientDescriptor(shadowMap->GetDepthSRV()).HeapIndex
						: ::GFL2::kInvalidBindlessIdx;
					m_TransparentPass.Execute(
						ctx, m_Assets, frameCB, *hdr, *aux, *depth, shadowSrv,
						m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 8：M6 Reverse-Z SSAO (角色 AuxMask.a >= 0.495 严格豁免)
		if (pipeOn && isFinalDbg && doEnv && activeParams.GFL2EnableSSAO && m_PostPass.IsInitialized())
		{
			m_PassList.AddPass("GFL2SSAOPass",
				{
					{ depth, PassAccess::ReadSRV_NonPixel },
					{ aux,   PassAccess::ReadSRV_NonPixel },
					{ hdr,   PassAccess::WriteRTV },
				},
				[this, hdr, aux, depth, frameCB, activeParams](GraphicsContext& ctx)
				{
					m_PostPass.ExecuteSSAO(
						ctx, frameCB, activeParams, m_Buffers->PostTargets,
						*hdr, *aux, *depth, m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 9：M6 Reverse-Z TAA
		if (pipeOn && isFinalDbg && activeParams.GFL2EnableTAA && m_PostPass.IsInitialized())
		{
			m_PassList.AddPass("GFL2TAAPass",
				{
					{ hdr,   PassAccess::ReadSRV_NonPixel },
					{ depth, PassAccess::ReadSRV_NonPixel },
				},
				[this, hdr, depth, frameCB, activeParams](GraphicsContext& ctx)
				{
					m_PostPass.ExecuteTAA(
						ctx, frameCB, activeParams, m_Buffers->PostTargets,
						*hdr, *depth, *m_Services);
				});
		}

		// Pass 10：M6 8-Pass Bloom 金字塔
		const bool doBloom = pipeOn && isFinalDbg && activeParams.GFL2EnableBloom && m_PostPass.IsInitialized();
		if (doBloom)
		{
			m_PassList.AddPass("GFL2BloomPass",
				{ { hdr, PassAccess::ReadSRV_Pixel } },
				[this, hdr, frameCB, activeParams](GraphicsContext& ctx)
				{
					m_PostPass.ExecuteBloom(ctx, frameCB, activeParams, m_Buffers->PostTargets, *hdr, *m_Services);
				});
		}

		// Pass 11：M6 UberPost (Bloom + Vignette + 7281 32^3 3D LUT)
		const bool doUberLut = pipeOn && isFinalDbg && m_PostPass.IsInitialized() &&
			(doBloom || (activeParams.GFL2Enable3DLUT && m_EnvAssets.GetStats().Lut3DSrvIdx != ::GFL2::kInvalidBindlessIdx) ||
			 activeParams.GFL2VignetteIntensity > 1e-4f);
		if (doUberLut)
		{
			m_PassList.AddPass("GFL2UberPostLUTPass",
				{ { hdr, PassAccess::ReadSRV_Pixel } },
				[this, hdr, frameCB, activeParams](GraphicsContext& ctx)
				{
					m_PostPass.ExecuteUberPostLUT(
						ctx, frameCB, activeParams, m_Buffers->PostTargets, *hdr,
						m_EnvAssets.GetStats().Lut3DSrvIdx, m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 12：DebugView::StencilMask (9) 硬件 8-bit Stencil X 光覆盖可视化
		if (pipeOn && activeParams.Debug == DebugView::StencilMask && m_DebugOverlayPass.IsInitialized())
		{
			m_PassList.AddPass("GFL2DebugOverlayPass",
				{
					{ hdr,   PassAccess::WriteRTV },
					{ depth, PassAccess::ReadSRV_Pixel },
				},
				[this, hdr, depth, activeParams](GraphicsContext& ctx)
				{
					m_DebugOverlayPass.Execute(
						ctx, *hdr, *depth, static_cast<uint32_t>(activeParams.Debug),
						m_MainViewport, m_MainScissor, *m_Services);
				});
		}

		// Pass 13：唯一全局 TonemapPass (若 7281 3D LUT 已执行 ACEScct 色调映射，此处仅做曝光与 sRGB 收口)
		const bool bypassDisplayEncode = (static_cast<uint32_t>(activeParams.Debug) >= static_cast<uint32_t>(DebugView::Normal));
		RenderParams tonemapParams = activeParams;
		if (doUberLut && activeParams.GFL2Enable3DLUT && m_EnvAssets.GetStats().Lut3DSrvIdx != ::GFL2::kInvalidBindlessIdx)
			tonemapParams.EnableTonemap = false;

		m_PassList.AddPass("TonemapPass",
			{
				{ hdr, PassAccess::ReadSRV_Pixel },
				{ ldr, PassAccess::WriteRTV },
			},
			[this, tonemapParams, bypassDisplayEncode](GraphicsContext& ctx)
			{
				m_TonemapPass.Execute(ctx, m_Blackboard, tonemapParams, bypassDisplayEncode, m_MainViewport, m_MainScissor, *m_Services);
			});

		context.PIXBeginEvent(L"GFL2CharacterPipeline");
		m_PassList.Execute(context, m_Services, frame.Timer);
		context.PIXEndEvent();

		++m_FrameCounter;
	}

	void GFL2CharacterPipeline::EndFrame(GraphicsContext& context, const RenderFrameDesc& frame)
	{
		(void)context;
		(void)frame;
	}
}
