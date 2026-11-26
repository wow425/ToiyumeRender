// ============================================================================
// 模块：RayTracingRenderer（DXR 光追管线薄编排器 · WP-2B 实现）
// 架构层：Engine/04_Renderer/Renderer/RayTracing
// ============================================================================
#include "00_Core/PCH.h"
#include "RayTracingRenderer.h"

#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Command/CommandListManager.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"

using namespace Graphics;
using namespace Renderer;

namespace Renderer::RayTracing
{
	AlgorithmCapsReport RayTracingRenderer::CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const
	{
		AlgorithmCapsReport report;
		if (Graphics::IsSimulatedNoDxrForTest() ||
			caps.RaytracingTier < D3D12_RAYTRACING_TIER_1_0 ||
			!g_bRaytracingSupported || g_DeviceDXR == nullptr)
		{
			report.Supported = false;
			report.UnsupportedReason = Graphics::IsSimulatedNoDxrForTest()
				? "DXR disabled by -SimulateNoDXR test override (RaytracingTier=NOT_SUPPORTED)"
				: "Current GPU does not support DirectX Raytracing (RaytracingTier < 1.0)";
			report.EstimatedVramBytes = 0;
			return report;
		}
		report.Supported = true;
		// PrimarySurface(Depth4+Albedo4+Normal4+Material8+Emission4 = 24B/px) + LinearHDR(8B) + LDR(4B) + ~32MB AS/SBT
		report.EstimatedVramBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 36ull + (32ull << 20);
		return report;
	}

	void RayTracingRenderer::RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		m_PrimaryRayPass.RegisterHotReload(reloader, services);
		m_TonemapPass.RegisterHotReload(reloader, services);
	}

	bool RayTracingRenderer::Initialize(const RendererCreateDesc& desc)
	{
		m_CreateDesc = desc;

		if (desc.simulateInitFailure)
		{
			LOG_ERROR(L"[WP-1B][TD-08a] RayTracingRenderer::Initialize 模拟失败返回 false\n");
			return false;
		}

		if (Graphics::IsSimulatedNoDxrForTest() || !g_bRaytracingSupported || g_DeviceDXR == nullptr)
		{
			LOG_INFO(L"[DXR] RayTracingRenderer::Initialize 中止：设备不支持 DXR（supported=%d device=%p）\n",
				g_bRaytracingSupported ? 1 : 0, (void*)g_DeviceDXR.Get());
			return false;
		}

		if (!AttachOrCreateServices(desc))
		{
			LOG_ERROR(L"[DXR] RenderServices 初始化失败\n");
			return false;
		}

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };

		if (!CreateOutputTargets(desc.width, desc.height))
		{
			ReleaseServicesIfOwned();
			return false;
		}

		if (!m_MainPass.Initialize(*m_Services))
		{
			DestroyOutputTargets();
			ReleaseServicesIfOwned();
			return false;
		}

		// [契约 4] 初始化 DXR 1.1 Inline RayQuery 主表面提取 Pass（支持 SM 6.6 + Tier 1.1 时自动启用）
		m_PrimaryRayPass.Initialize(*m_Services);

		// [WP-2C / 契约 5] DXR UAV 写入 R16G16B16A16_FLOAT LinearHDRColor，由共享 TonemapPass 输出 LDR
		m_MainPass.RefreshOutputUav(m_LinearHDRBuffer.get(), *m_Services);
		m_TonemapPass.Initialize(Renderer::Deferred::SceneColorBufferFormat, m_Services);

		m_Initialized = true;
		LOG_INFO(L"[DXR] RayTracingRenderer 初始化完成：%ux%u，tier=%d，outputUavSlot=%u，RTPSO=OK\n",
			desc.width, desc.height, static_cast<int>(g_RaytracingTier), m_MainPass.GetOutputUavHeapIndex());
		return true;
	}

	void RayTracingRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		if (g_Device != nullptr)
			g_CommandManager.IdleGPU();

		const uint32_t dispatched = m_MainPass.GetDispatchedFrameCount();
		m_TonemapPass.Shutdown();
		m_PrimaryRayPass.Shutdown();
		m_ASManager.Clear(/*idleGpu=*/false);
		m_MainPass.Shutdown(m_Services);
		DestroyOutputTargets();
		ReleaseServicesIfOwned();

		m_Stats = {};
		m_Initialized = false;
		LOG_INFO(L"[DXR] RayTracingRenderer 已关闭（累计 DispatchRays 帧数=%u）\n", dispatched);
	}

	void RayTracingRenderer::OnResize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
		m_CreateDesc.width  = width;
		m_CreateDesc.height = height;

		if (!m_Initialized)
			return;

		g_CommandManager.IdleGPU();
		DestroyOutputTargets();
		CreateOutputTargets(width, height);
		m_MainPass.RefreshOutputUav(m_LinearHDRBuffer.get(), *m_Services);
	}

	bool RayTracingRenderer::CreateOutputTargets(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return false;

		// [契约 4] 统一 PrimarySurface 缓冲：ClearColor 严格对齐空场景哨兵值（Depth=1.0），杜绝 Debug Layer #820
		const Color clearDepthOne(1.0f, 1.0f, 1.0f, 1.0f);
		const Color clearAlbedo(0.0f, 0.0f, 0.0f, 1.0f);
		const Color clearNormal(0.5f, 0.5f, 0.0f, 0.0f);
		const Color clearZero(0.0f, 0.0f, 0.0f, 0.0f);

		m_PrimaryDepthBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing PrimarySurface Depth", width, height,
			Renderer::Deferred::PrimarySurfaceDepthFormat, 1, &clearDepthOne);
		m_PrimaryAlbedoBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing PrimarySurface AlbedoAO", width, height,
			Renderer::Deferred::GBufferFormats[0], 1, &clearAlbedo);
		m_PrimaryNormalBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing PrimarySurface Normal", width, height,
			Renderer::Deferred::GBufferFormats[1], 1, &clearNormal);
		m_PrimaryMaterialBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing PrimarySurface Material", width, height,
			Renderer::Deferred::GBufferFormats[2], 1, &clearZero);
		m_PrimaryEmissionBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing PrimarySurface Emission", width, height,
			Renderer::Deferred::GBufferFormats[3], 1, &clearZero);

		m_LinearHDRBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing LinearHDRColor", width, height,
			Renderer::Deferred::LinearHDRBufferFormat, 1, nullptr);
		m_SceneColorBuffer = BufferManager::CreateColorBuffer(
			L"RayTracing Scene Color", width, height,
			Renderer::Deferred::SceneColorBufferFormat, 1, nullptr);
		if (m_PrimaryDepthBuffer == nullptr || m_PrimaryAlbedoBuffer == nullptr ||
			m_PrimaryNormalBuffer == nullptr || m_PrimaryMaterialBuffer == nullptr ||
			m_PrimaryEmissionBuffer == nullptr || m_LinearHDRBuffer == nullptr ||
			m_SceneColorBuffer == nullptr)
		{
			LOG_ERROR(L"[DXR] 创建 LinearHDR/LDR 输出缓冲失败：%ux%u\n", width, height);
			return false;
		}
		return true;
	}

	void RayTracingRenderer::DestroyOutputTargets()
	{
		if (m_PrimaryDepthBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing PrimarySurface Depth", m_PrimaryDepthBuffer);
			m_PrimaryDepthBuffer.reset();
		}
		if (m_PrimaryAlbedoBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing PrimarySurface AlbedoAO", m_PrimaryAlbedoBuffer);
			m_PrimaryAlbedoBuffer.reset();
		}
		if (m_PrimaryNormalBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing PrimarySurface Normal", m_PrimaryNormalBuffer);
			m_PrimaryNormalBuffer.reset();
		}
		if (m_PrimaryMaterialBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing PrimarySurface Material", m_PrimaryMaterialBuffer);
			m_PrimaryMaterialBuffer.reset();
		}
		if (m_PrimaryEmissionBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing PrimarySurface Emission", m_PrimaryEmissionBuffer);
			m_PrimaryEmissionBuffer.reset();
		}
		if (m_LinearHDRBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing LinearHDRColor", m_LinearHDRBuffer);
			m_LinearHDRBuffer.reset();
		}
		if (m_SceneColorBuffer != nullptr)
		{
			BufferManager::ReleaseColorBuffer(L"RayTracing Scene Color", m_SceneColorBuffer);
			m_SceneColorBuffer.reset();
		}
	}

	void RayTracingRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		if (!m_Initialized)
			return;

		AdvanceOwnedServicesFrame();
		m_ASManager.SyncScene(gfxContext, frame.Models, m_MainPass.GetStateObjectProperties());
		if (frame.Models.empty())
			m_Stats = {};
	}

	void RayTracingRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		(void)batchType;
		if (pass != kOpaque || !m_Initialized)
			return;

		m_Blackboard.Reset();
		m_Blackboard.Core.LinearHDRColor = m_LinearHDRBuffer.get();
		m_Blackboard.Core.SceneColorLDR  = m_SceneColorBuffer.get();
		// [契约 4] 发布 DXR 统一 PrimarySurfaceDesc 并于 Zone B 一次性暂存 Bindless SRV 下标，
		// 使未来下游 Pass（间接光 / 去噪 / 阴影）与 Deferred 路径同构零分支读取。
		m_Blackboard.PrimarySurface = BuildPrimarySurfaceFromDxr(
			m_CreateDesc.width, m_CreateDesc.height,
			m_PrimaryDepthBuffer.get(),
			m_PrimaryAlbedoBuffer.get(),
			m_PrimaryNormalBuffer.get(),
			m_PrimaryMaterialBuffer.get(),
			m_PrimaryEmissionBuffer.get(),
			nullptr);
		StagePrimarySurfaceSRVs(m_Blackboard.PrimarySurface, m_Services->GetDescriptorHeap());

		m_Blackboard.Set(FrameResourceKey::SceneDepth,        m_PrimaryDepthBuffer.get(),    m_Blackboard.PrimarySurface.Depth.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_BaseColor, m_PrimaryAlbedoBuffer.get(),   m_Blackboard.PrimarySurface.AlbedoAO.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Normal,    m_PrimaryNormalBuffer.get(),   m_Blackboard.PrimarySurface.Normal.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Material,  m_PrimaryMaterialBuffer.get(), m_Blackboard.PrimarySurface.Material.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::GBuffer_Emission,  m_PrimaryEmissionBuffer.get(), m_Blackboard.PrimarySurface.Emission.SrvBindlessIdx);
		m_Blackboard.Set(FrameResourceKey::LinearHDRColor,    m_LinearHDRBuffer.get());
		m_Blackboard.Set(FrameResourceKey::SceneColorLDR,     m_SceneColorBuffer.get());

		const RenderParams activeParams = frame.Params != nullptr ? *frame.Params : RenderParams{};
		const bool hasActiveScene = m_ASManager.IsReady() && !frame.Models.empty();

		m_PassList.Reset();

		// ---- [契约 4] PrimaryRayPass：DXR 1.1 Inline RayQuery 写入统一 PrimarySurface ----
		{
			PrimaryRayTargets primaryTargets;
			primaryTargets.DepthR32F = m_PrimaryDepthBuffer.get();
			primaryTargets.AlbedoAO  = m_PrimaryAlbedoBuffer.get();
			primaryTargets.NormalOct = m_PrimaryNormalBuffer.get();
			primaryTargets.Material  = m_PrimaryMaterialBuffer.get();
			primaryTargets.Emission  = m_PrimaryEmissionBuffer.get();

			const bool tracePrimary = hasActiveScene && m_PrimaryRayPass.IsInitialized();
			const PassAccess writeMode = tracePrimary ? PassAccess::WriteUAV : PassAccess::WriteRTV;
			std::vector<ResourceAccess> primaryAccesses = {
				{ primaryTargets.DepthR32F, writeMode },
				{ primaryTargets.AlbedoAO,  writeMode },
				{ primaryTargets.NormalOct, writeMode },
				{ primaryTargets.Material,  writeMode },
				{ primaryTargets.Emission,  writeMode },
			};
			if (tracePrimary)
			{
				if (m_ASManager.GetScene().GetTopLevelBuffer() != nullptr)
					primaryAccesses.push_back({ const_cast<GpuBuffer*>(m_ASManager.GetScene().GetTopLevelBuffer()), PassAccess::AccelerationStructure });
				if (m_ASManager.GetScene().GetSceneGeometryBuffer() != nullptr)
					primaryAccesses.push_back({ const_cast<GpuBuffer*>(m_ASManager.GetScene().GetSceneGeometryBuffer()), PassAccess::ReadSRV_NonPixel });
				if (m_ASManager.GetScene().GetInstanceDataBuffer() != nullptr)
					primaryAccesses.push_back({ const_cast<GpuBuffer*>(m_ASManager.GetScene().GetInstanceDataBuffer()), PassAccess::ReadSRV_NonPixel });
			}

			m_PassList.AddPass("PrimaryRayPass",
				std::move(primaryAccesses),
				[this, &frame, primaryTargets](GraphicsContext& ctx)
				{
					m_PrimaryRayPass.Execute(ctx, frame, m_ASManager, primaryTargets,
						m_CreateDesc.width, m_CreateDesc.height, *m_Services);
				});
		}

		// [WP-3A / 契约 3] DXR 主射线 Pass 必须走 AddNativePass 逃逸护栏：
		//   进入前刷屏障并保单可见堆；退出后自动 InvalidateCachedPipelineState() 防 SetPipelineState1 污染后续 TonemapPass！
		std::vector<ResourceAccess> dxrAccesses;
		if (hasActiveScene)
		{
			dxrAccesses.push_back({ m_LinearHDRBuffer.get(), PassAccess::WriteUAV });
			if (m_ASManager.GetScene().GetTopLevelBuffer() != nullptr)
				dxrAccesses.push_back({ const_cast<GpuBuffer*>(m_ASManager.GetScene().GetTopLevelBuffer()), PassAccess::AccelerationStructure });
		}
		else
		{
			dxrAccesses.push_back({ m_LinearHDRBuffer.get(), PassAccess::WriteRTV });
		}

		m_PassList.AddNativePass("RayTracingMainPass",
			std::move(dxrAccesses),
			[this, &frame](GraphicsContext& ctx)
			{
				m_MainPass.Execute(ctx, frame, m_ASManager, m_Blackboard,
					m_CreateDesc.width, m_CreateDesc.height, *m_Services, m_Stats);
			});

		m_PassList.AddPass("TonemapPass",
			{
				{ m_LinearHDRBuffer.get(),  PassAccess::ReadSRV_Pixel },
				{ m_SceneColorBuffer.get(), PassAccess::WriteRTV },
			},
			[this, activeParams](GraphicsContext& ctx)
			{
				// 逐像素调试旁理由 LinearHDRColor.a < 0.5 精确控制（命中调试像素直通、背景仍走显示编码）
				m_TonemapPass.Execute(ctx, m_Blackboard, activeParams, /*bypassDisplayEncode=*/false,
					m_MainViewport, m_MainScissor, *m_Services);
			});

		context.PIXBeginEvent(L"RayTracingRenderer");
		m_PassList.Execute(context, m_Services, frame.Timer);
		context.PIXEndEvent();
	}

	void RayTracingRenderer::EndFrame(GraphicsContext& context, const RenderFrameDesc& frame)
	{
		(void)context;
		(void)frame;
	}
}
