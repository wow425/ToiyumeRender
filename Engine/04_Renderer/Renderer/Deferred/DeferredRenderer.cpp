// ============================================================================
// 模块：DeferredRenderer（延迟渲染管线编排器 · WP-3C 实现）
// 架构层：Engine/04_Renderer/Renderer/Deferred
// ============================================================================
#include "00_Core/PCH.h"
#include "DeferredRenderer.h"

#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"

using namespace Graphics;
using namespace Renderer;

namespace Renderer::Deferred
{
	namespace
	{
		constexpr wchar_t kHDRTextureRelative[] = L"Skybox/grasslands_sunset_2k.dds";
	}

	AlgorithmCapsReport DeferredRenderer::CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const
	{
		(void)caps;
		AlgorithmCapsReport report;
		report.Supported = true;
		// 粗估 4×GBuffer + LinearHDR(8B) + LDR(4B) + Depth(4B) + Velocity(4B) + 2×SSAO(2B) + 2×TAAHistory(16B) ≈ 54 B/px
		report.EstimatedVramBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 54ull;
		return report;
	}

	void DeferredRenderer::RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		m_LightingPass.RegisterHotReload(reloader, services);
		m_SkyboxPass.RegisterHotReload(reloader, services);
		m_SSAOPass.RegisterHotReload(reloader, services);
		m_TAAPass.RegisterHotReload(reloader, services);
		m_TonemapPass.RegisterHotReload(reloader, services);
	}

	void DeferredRenderer::ResetHistory()
	{
		m_TAAPass.ResetHistory();
	}

	bool DeferredRenderer::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)
			return true;

		if (desc.simulateInitFailure)
		{
			LOG_ERROR(L"[WP-1B][TD-08a] DeferredRenderer::Initialize 模拟失败返回 false\n");
			return false;
		}

		if (!AttachOrCreateServices(desc))
		{
			LOG_ERROR(L"[DeferredRenderer] RenderServices 初始化失败\n");
			return false;
		}

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };
		m_CreateDesc   = desc;
		m_FrameCounter = 0;

		CreateDeferredBufferTargets();
		m_ShadowPass.Initialize(*m_Services);
		m_GBufferPass.Initialize(*m_Services);
		m_SSAOPass.Initialize(*m_Services);
		// [WP-2C / WP-3C] LightingPass 在 SM 6.6 硬件上自动激活 Bindless 路径以消费 ShadowMap / SSAOMap
		m_LightingPass.Initialize(LinearHDRBufferFormat, SceneDepthBufferFormat, m_Services);
		m_SkyboxPass.Initialize(LinearHDRBufferFormat, SceneDepthBufferFormat);
		m_TAAPass.Initialize(*m_Services);
		m_TonemapPass.Initialize(SceneColorBufferFormat, m_Services);
		m_IBLBakePass.Initialize(LinearHDRBufferFormat, SceneDepthBufferFormat, kHDRTextureRelative);

		m_Initialized = true;
		return true;
	}

	void DeferredRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		// [TD-03a / 所有权反序] 先关闭所有 Pass（含 ShadowPass 经延迟队列退场阴影图与 IBLBakePass 释放 HDR TextureRef），
		// 再销毁本管线自持有 RT，最后按需释放独占 RenderServices。
		m_TonemapPass.Shutdown();
		m_TAAPass.Shutdown();
		m_SkyboxPass.Shutdown();
		m_LightingPass.Shutdown();
		m_SSAOPass.Shutdown();
		m_GBufferPass.Shutdown(m_Services);
		m_ShadowPass.Shutdown(m_Services);
		m_IBLBakePass.Shutdown();

		DestroyDeferredBufferTargets();
		ReleaseServicesIfOwned();

		m_Initialized = false;
	}

	void DeferredRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width  = width;
		m_CreateDesc.height = height;

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

		if (!m_Initialized)
			return;

		// 等 GPU 完成对旧 RT 的全部读取/写入后再销毁重建，同时清空 TAA 历史防止旧尺寸重投影错乱
		g_CommandManager.IdleGPU();
		m_TAAPass.ResetHistory();
		DestroyDeferredBufferTargets();
		CreateDeferredBufferTargets();
	}

	void DeferredRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;
	}

	void DeferredRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		AdvanceOwnedServicesFrame();
		m_DrawHelper.Reset();

		for (auto* model : frame.Models)
		{
			if (model == nullptr)
				continue;

			if (model->IsDirty())
				model->Update(gfxContext, frame.delatT);

			m_DrawHelper.AddModel(*model);
		}
		m_DrawHelper.Sort();

		// 先执行一次性 IBL 烘焙并回填 PrefilterMaxMip，再同步 RenderParams 常量缓冲
		m_IBLBakePass.ExecuteBake(gfxContext);

		if (frame.Params != nullptr)
			Scene::LightingSystem::ApplyParams(gfxContext, *frame.Params);
	}

	void DeferredRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		(void)batchType;
		if (!m_Initialized || m_GBuffers == nullptr || frame.Camera == nullptr)
			return;

		const RenderParams activeParams =
			frame.Params != nullptr ? *frame.Params : *Scene::LightingSystem::GetAppliedParams();

		const bool drawSkybox    = (activeParams.Debug == DebugView::Final);
		const bool bypassTonemap = (activeParams.Debug != DebugView::Final);

		// [WP-3C] Feature Toggles 实控开关判定：关闭态零额外 Pass、零抖动，保证 WP-3B Golden 严格 0 像素差
		const bool shadowActive =
			activeParams.EnableShadow &&
			activeParams.EnableSunLight &&
			(activeParams.Debug == DebugView::Final) &&
			m_ShadowPass.IsInitialized() &&
			m_LightingPass.IsUsingBindlessSM66() &&
			m_ShadowPass.EnsureAlgorithm(*m_Services, activeParams);

		const bool ssaoActive =
			activeParams.EnableSSAO &&
			(activeParams.Debug == DebugView::Final || activeParams.Debug == DebugView::AO) &&
			m_SSAOPass.IsInitialized() &&
			m_LightingPass.IsUsingBindlessSM66();
		const bool ssaoBlurActive = ssaoActive && activeParams.SSAOEnableBlur;

		const bool taaActive =
			activeParams.EnableTAA &&
			(activeParams.Debug == DebugView::Final) &&
			m_TAAPass.IsInitialized();

		// TAA 关闭或切入调试视图时重置历史，确保下次重新开启首帧纯净无残影
		TAAPass::FrameJitterState jitterState = {};
		if (taaActive)
		{
			jitterState = m_TAAPass.PrepareFrame(*frame.Camera, m_CreateDesc.width, m_CreateDesc.height, activeParams.TAAJitterScale);
		}
		else
		{
			m_TAAPass.ResetHistory();
		}

		// 1. 填充本帧 FrameBlackboard（供所有独立 Pass 零耦合消费）
		ColorBuffer* gbuf0    = m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor].get();
		ColorBuffer* gbuf1    = m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal].get();
		ColorBuffer* gbuf2    = m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material].get();
		ColorBuffer* gbuf3    = m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission].get();
		DepthBuffer* depth    = m_GBuffers->SceneDepthBuffer.get();
		ColorBuffer* hdr      = m_GBuffers->LinearHDRBuffer.get();
		ColorBuffer* ldr      = m_GBuffers->SceneColorBuffer.get();
		ColorBuffer* ssaoRaw  = m_GBuffers->SSAORawBuffer.get();
		ColorBuffer* ssaoBlur = m_GBuffers->SSAOBlurBuffer.get();
		ColorBuffer* taaPrev  = m_GBuffers->TAAHistoryBuffer[m_TAAPass.GetReadHistorySlot()].get();
		ColorBuffer* taaCurr  = m_GBuffers->TAAHistoryBuffer[m_TAAPass.GetWriteHistorySlot()].get();
		DepthBuffer* shadowDepth = shadowActive ? m_ShadowPass.GetShadowDepthBuffer() : nullptr;

		m_Blackboard.Reset();
		m_Blackboard.Core.SceneDepth     = depth;
		m_Blackboard.Core.LinearHDRColor = hdr;
		m_Blackboard.Core.SceneColorLDR  = ldr;
		// [契约 4] 零拷贝复用既有 GBuffer + SceneDepth 组装统一 PrimarySurfaceDesc，
		// 并在 Zone B 一次性暂存全部 Bindless SRV 下标，供下游 SSAO / Lighting 及未来通道同构消费。
		m_Blackboard.PrimarySurface = BuildPrimarySurfaceFromDeferred(
			m_CreateDesc.width, m_CreateDesc.height,
			depth, gbuf0, gbuf1, gbuf2, gbuf3, m_GBuffers->VelocityBuffer.get());
		StagePrimarySurfaceSRVs(m_Blackboard.PrimarySurface, m_Services->GetDescriptorHeap());

		m_Blackboard.Set(FrameResourceKey::GBuffer_BaseColor, gbuf0, m_Blackboard.PrimarySurface.AlbedoAO.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Normal,    gbuf1, m_Blackboard.PrimarySurface.Normal.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Material,  gbuf2, m_Blackboard.PrimarySurface.Material.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Emission,  gbuf3, m_Blackboard.PrimarySurface.Emission.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::SceneDepth,        depth, m_Blackboard.PrimarySurface.Depth.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::Velocity,          m_GBuffers->VelocityBuffer.get(), m_Blackboard.PrimarySurface.Velocity.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::LinearHDRColor,    hdr);
		m_Blackboard.Set(FrameResourceKey::SceneColorLDR,     ldr);
		m_Blackboard.Set(FrameResourceKey::SSAORaw,           ssaoRaw);
		m_Blackboard.Set(FrameResourceKey::SSAOMap,           ssaoBlur);
		m_Blackboard.Set(FrameResourceKey::TAAHistoryPrev,    taaPrev);
		m_Blackboard.Set(FrameResourceKey::TAAOutput,         taaCurr);
		if (shadowDepth != nullptr)
			m_Blackboard.Set(FrameResourceKey::ShadowMap, shadowDepth);
		m_IBLBakePass.PublishToBlackboard(m_Blackboard);

		// 2. [WP-3A / WP-3C] 经三阶段 LinearPassList 编排全部真实 Pass（自动 PIX 标记 + GpuTimer + Barrier）
		ShadowOutput frameShadowOut = {};
		m_PassList.Reset();

		// ---- [Feature: Shadow] ShadowPass (SimpleShadowMap / CSM 2..4 级联) ----
		if (shadowActive && shadowDepth != nullptr)
		{
			std::vector<ResourceAccess> shadowAccesses;
			const uint32_t sliceCount = shadowDepth->GetSliceCount();
			shadowAccesses.reserve(sliceCount);
			for (uint32_t s = 0; s < sliceCount; ++s)
				shadowAccesses.push_back({ shadowDepth, PassAccess::WriteDepth, s });

			m_PassList.AddPass("ShadowPass", std::move(shadowAccesses),
				[this, &frame, activeParams, &frameShadowOut](GraphicsContext& ctx)
				{
					frameShadowOut = m_ShadowPass.Execute(ctx, frame, m_DrawHelper, activeParams, *m_Services);
				});
		}

		// ---- [Core] GBufferPass ----
		m_PassList.AddPass("GBufferPass",
			{
				{ gbuf0, PassAccess::WriteRTV },
				{ gbuf1, PassAccess::WriteRTV },
				{ gbuf2, PassAccess::WriteRTV },
				{ gbuf3, PassAccess::WriteRTV },
				{ hdr,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			},
			[this, &frame, pass, taaActive, jitterState](GraphicsContext& ctx)
			{
				const Math::Matrix4* vpOverride = taaActive ? &jitterState.JitteredViewProj : nullptr;
				m_GBufferPass.Execute(ctx, frame, m_DrawHelper, m_Blackboard, pass, m_MainViewport, m_MainScissor, *m_Services, vpOverride);
			});

		// ---- [Feature: SSAO] SSAO_Compute + SSAO_Blur ----
		ColorBuffer* ssaoForLighting = nullptr;
		if (ssaoActive && ssaoRaw != nullptr)
		{
			const uint32_t frameIdx = m_FrameCounter;
			m_PassList.AddPass("SSAO_Compute",
				{
					{ depth,   PassAccess::ReadSRV_NonPixel },
					{ gbuf1,   PassAccess::ReadSRV_NonPixel },
					{ ssaoRaw, PassAccess::WriteUAV },
				},
				[this, &frame, activeParams, frameIdx, taaActive, jitterState](GraphicsContext& ctx)
				{
					const Math::Matrix4* invVpOverride = taaActive ? &jitterState.InvJitteredViewProj : nullptr;
					const Math::Matrix4* projOverride  = taaActive ? &jitterState.JitteredProj : nullptr;
					m_SSAOPass.ExecuteCompute(ctx, m_Blackboard, *frame.Camera, activeParams,
						m_CreateDesc.width, m_CreateDesc.height, frameIdx, invVpOverride, projOverride, *m_Services);
				});

			if (ssaoBlurActive && ssaoBlur != nullptr)
			{
				m_PassList.AddPass("SSAO_Blur",
					{
						{ ssaoRaw,  PassAccess::ReadSRV_NonPixel },
						{ depth,    PassAccess::ReadSRV_NonPixel },
						{ gbuf1,    PassAccess::ReadSRV_NonPixel },
						{ ssaoBlur, PassAccess::WriteUAV },
					},
					[this, &frame](GraphicsContext& ctx)
					{
						m_SSAOPass.ExecuteBlur(ctx, m_Blackboard, *frame.Camera,
							m_CreateDesc.width, m_CreateDesc.height, *m_Services);
					});
				ssaoForLighting = ssaoBlur;
			}
			else
			{
				ssaoForLighting = ssaoRaw;
			}
		}

		// ---- [Core] LightingPass（消费可选 ShadowMap + SSAOMap） ----
		{
			std::vector<ResourceAccess> lightAccesses = {
				{ gbuf0, PassAccess::ReadSRV_Pixel },
				{ gbuf1, PassAccess::ReadSRV_Pixel },
				{ gbuf2, PassAccess::ReadSRV_Pixel },
				{ gbuf3, PassAccess::ReadSRV_Pixel },
				{ depth, PassAccess::ReadDepthAndSRV },
				{ hdr,   PassAccess::WriteRTV },
			};
			if (shadowActive && shadowDepth != nullptr)
			{
				// 全资源收敛：将 CSM 逐切片 DEPTH_WRITE 统一转换回 PIXEL_SHADER_RESOURCE
				lightAccesses.push_back({ shadowDepth, PassAccess::ReadSRV_Pixel });
			}
			if (ssaoActive && ssaoForLighting != nullptr)
			{
				lightAccesses.push_back({ ssaoForLighting, PassAccess::ReadSRV_Pixel });
			}

			m_PassList.AddPass("LightingPass", std::move(lightAccesses),
				[this, &frame, shadowActive, &frameShadowOut, ssaoActive, ssaoForLighting, taaActive, jitterState](GraphicsContext& ctx)
				{
					LightingPass::ExecutionExtras extras = {};
					extras.Shadow              = shadowActive ? &frameShadowOut : nullptr;
					extras.SSAOMap             = ssaoForLighting;
					extras.EnableSSAO          = ssaoActive && (ssaoForLighting != nullptr);
					extras.InvViewProjOverride = taaActive ? &jitterState.InvJitteredViewProj : nullptr;
					m_LightingPass.Execute(ctx, m_Blackboard, *frame.Camera, m_CreateDesc.width, m_CreateDesc.height, *m_Services, &extras);
				});
		}

		// ---- [Core] SkyboxPass ----
		if (drawSkybox)
		{
			ColorBuffer* envCube = m_Blackboard.GetColorBuffer(FrameResourceKey::EnvCube);
			m_PassList.AddPass("SkyboxPass",
				{
					{ envCube, PassAccess::ReadSRV_Pixel },
					{ depth,   PassAccess::ReadDepthAndSRV },
					{ hdr,     PassAccess::WriteRTV },
				},
				[this, &frame, activeParams, taaActive, jitterState](GraphicsContext& ctx)
				{
					const Math::Matrix4* projOverride = taaActive ? &jitterState.JitteredProj : nullptr;
					m_SkyboxPass.Execute(ctx, m_Blackboard, *frame.Camera, activeParams, *m_Services, projOverride);
				});
		}

		// ---- [Feature: TAA] TAAPass（Linear HDR 空间时域抗锯齿 Resolve） ----
		ColorBuffer* tonemapInput = hdr;
		if (taaActive && taaPrev != nullptr && taaCurr != nullptr)
		{
			m_PassList.AddPass("TAAPass",
				{
					{ hdr,     PassAccess::ReadSRV_NonPixel },
					{ taaPrev, PassAccess::ReadSRV_NonPixel },
					{ depth,   PassAccess::ReadSRV_NonPixel },
					{ taaCurr, PassAccess::WriteUAV },
				},
				[this, jitterState, activeParams](GraphicsContext& ctx)
				{
					m_TAAPass.Execute(ctx, m_Blackboard, jitterState, activeParams,
						m_CreateDesc.width, m_CreateDesc.height, *m_Services);
				});
			tonemapInput = taaCurr;
		}

		// ---- [Core: PostProcess] TonemapPass（唯一共享 HDR -> LDR 编码） ----
		m_PassList.AddPass("TonemapPass",
			{
				{ tonemapInput, PassAccess::ReadSRV_Pixel },
				{ ldr,          PassAccess::WriteRTV },
			},
			[this, activeParams, bypassTonemap, tonemapInput](GraphicsContext& ctx)
			{
				m_Blackboard.Core.LinearHDRColor = tonemapInput;
				m_TonemapPass.Execute(ctx, m_Blackboard, activeParams, bypassTonemap, m_MainViewport, m_MainScissor, *m_Services);
			});

		context.PIXBeginEvent(L"DeferredRenderer");
		m_PassList.Execute(context, m_Services, frame.Timer);
		context.PIXEndEvent();

		++m_FrameCounter;
	}

	void DeferredRenderer::EndFrame(GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		for (auto* model : frame.Models)
		{
			if (model != nullptr)
				model->ClearDirty();
		}
	}

	void DeferredRenderer::CreateDeferredBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0)
			return;

		const uint32_t width  = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_GBuffers = std::make_shared<DeferredBuffer>();
		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor] =
			BufferManager::CreateColorBuffer(L"GBuffer_BaseColor", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_BaseColor]);
		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal] =
			BufferManager::CreateColorBuffer(L"GBuffer_Normal", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Normal]);
		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material] =
			BufferManager::CreateColorBuffer(L"GBuffer_Material", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Material]);
		m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission] =
			BufferManager::CreateColorBuffer(L"GBuffer_Emission", width, height, GBufferFormats[(uint32_t)GBufferSlot::GBuffer_Emission]);

		m_GBuffers->LinearHDRBuffer  = BufferManager::CreateColorBuffer(L"Deferred LinearHDRColor",   width, height, LinearHDRBufferFormat);
		m_GBuffers->SceneColorBuffer = BufferManager::CreateColorBuffer(L"Deferred SceneColorBuffer", width, height, SceneColorBufferFormat);
		m_GBuffers->SceneDepthBuffer = BufferManager::CreateDepthBuffer(L"Deferred SceneDepthBuffer", width, height, SceneDepthBufferFormat);
		m_GBuffers->VelocityBuffer   = BufferManager::CreateColorBuffer(L"Deferred VelocityBuffer",   width, height, VelocityBufferFormat);

		// [WP-3C] SSAO 原始/模糊目标 + TAA Ping-Pong 历史目标（与主缓冲统一托管于 BufferManager，Resize 零泄漏）
		m_GBuffers->SSAORawBuffer       = BufferManager::CreateColorBuffer(L"Deferred SSAORaw",       width, height, SSAOBufferFormat);
		m_GBuffers->SSAOBlurBuffer      = BufferManager::CreateColorBuffer(L"Deferred SSAOBlur",      width, height, SSAOBufferFormat);
		m_GBuffers->TAAHistoryBuffer[0] = BufferManager::CreateColorBuffer(L"Deferred TAAHistory_0",  width, height, LinearHDRBufferFormat);
		m_GBuffers->TAAHistoryBuffer[1] = BufferManager::CreateColorBuffer(L"Deferred TAAHistory_1",  width, height, LinearHDRBufferFormat);
	}

	void DeferredRenderer::DestroyDeferredBufferTargets()
	{
		if (m_GBuffers)
		{
			BufferManager::ReleaseColorBuffer(L"GBuffer_BaseColor",         m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_BaseColor]);
			BufferManager::ReleaseColorBuffer(L"GBuffer_Normal",            m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Normal]);
			BufferManager::ReleaseColorBuffer(L"GBuffer_Material",          m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Material]);
			BufferManager::ReleaseColorBuffer(L"GBuffer_Emission",          m_GBuffers->GBuffers[(uint32_t)GBufferSlot::GBuffer_Emission]);
			BufferManager::ReleaseColorBuffer(L"Deferred LinearHDRColor",   m_GBuffers->LinearHDRBuffer);
			BufferManager::ReleaseColorBuffer(L"Deferred SceneColorBuffer", m_GBuffers->SceneColorBuffer);
			BufferManager::ReleaseDepthBuffer(L"Deferred SceneDepthBuffer", m_GBuffers->SceneDepthBuffer);
			BufferManager::ReleaseColorBuffer(L"Deferred VelocityBuffer",   m_GBuffers->VelocityBuffer);
			BufferManager::ReleaseColorBuffer(L"Deferred SSAORaw",          m_GBuffers->SSAORawBuffer);
			BufferManager::ReleaseColorBuffer(L"Deferred SSAOBlur",         m_GBuffers->SSAOBlurBuffer);
			BufferManager::ReleaseColorBuffer(L"Deferred TAAHistory_0",     m_GBuffers->TAAHistoryBuffer[0]);
			BufferManager::ReleaseColorBuffer(L"Deferred TAAHistory_1",     m_GBuffers->TAAHistoryBuffer[1]);
		}
		m_GBuffers.reset();
	}
} // namespace Renderer::Deferred
