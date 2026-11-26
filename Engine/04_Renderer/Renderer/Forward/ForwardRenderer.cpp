// ============================================================================
// 模块：ForwardRenderer（前向渲染管线薄编排器 · WP-2B 实现）
// 架构层：Engine/04_Renderer/Renderer/Forward
// ============================================================================
#include "00_Core/PCH.h"
#include "ForwardRenderer.h"

#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"

using namespace Graphics;
using namespace Renderer;

namespace Renderer::Forward
{
	AlgorithmCapsReport ForwardRenderer::CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const
	{
		(void)caps;
		AlgorithmCapsReport report;
		report.Supported = true;
		// LinearHDR(8B/px) + LDR(4B/px) + Depth(4B/px) = 16 B/px
		report.EstimatedVramBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 16ull;
		return report;
	}

	void ForwardRenderer::RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		m_TonemapPass.RegisterHotReload(reloader, services);
	}

	bool ForwardRenderer::Initialize(const RendererCreateDesc& desc)
	{
		if (m_Initialized)
			return true;

		if (desc.simulateInitFailure)
		{
			LOG_ERROR(L"[WP-1B][TD-08a] ForwardRenderer::Initialize 模拟失败返回 false\n");
			return false;
		}

		if (!AttachOrCreateServices(desc))
		{
			LOG_ERROR(L"[ForwardRenderer] RenderServices 初始化失败\n");
			return false;
		}

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };
		m_CreateDesc   = desc;

		CreateForwardBufferTargets();
		// [WP-2C] 前向主 Pass 同样接入 R16G16B16A16_FLOAT LinearHDRColor，并经共享 TonemapPass 输出 LDR
		m_MainPass.Initialize(Renderer::Deferred::LinearHDRBufferFormat, Config::depthBufferFormat, *m_Services);
		m_TonemapPass.Initialize(Config::backBufferFormat, m_Services);

		m_Initialized = true;
		return true;
	}

	void ForwardRenderer::Shutdown()
	{
		if (!m_Initialized)
			return;

		m_TonemapPass.Shutdown();
		m_MainPass.Shutdown(m_Services);
		DestroyForwardBufferTargets();
		ReleaseServicesIfOwned();

		m_Initialized = false;
	}

	void ForwardRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_CreateDesc.width  = width;
		m_CreateDesc.height = height;

		m_MainViewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
		m_MainScissor  = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

		if (!m_Initialized)
			return;

		g_CommandManager.IdleGPU();
		DestroyForwardBufferTargets();
		CreateForwardBufferTargets();
	}

	void ForwardRenderer::BeginFrame(const RenderFrameDesc& frame)
	{
		(void)frame;
	}

	void ForwardRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		AdvanceOwnedServicesFrame();
		m_DrawHelper.Reset();

		for (Scene::Model::ModelInstance* model : frame.Models)
		{
			if (model == nullptr)
				continue;

			if (model->IsDirty())
				model->Update(gfxContext, frame.delatT);

			m_DrawHelper.AddModel(*model);
		}
		m_DrawHelper.Sort();

		if (frame.Params != nullptr)
			Scene::LightingSystem::ApplyParams(gfxContext, *frame.Params);
	}

	void ForwardRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass, BatchType batchType)
	{
		if (!m_Initialized || m_ForwardBuffer == nullptr || frame.Camera == nullptr)
			return;

		m_Blackboard.Reset();
		m_Blackboard.Core.SceneDepth     = m_ForwardBuffer->DepthBuffer.get();
		m_Blackboard.Core.LinearHDRColor = m_ForwardBuffer->LinearHDRBuffer.get();
		m_Blackboard.Core.SceneColorLDR  = m_ForwardBuffer->SceneColorBuffer[0].get();
		m_Blackboard.Set(FrameResourceKey::SceneDepth,     m_ForwardBuffer->DepthBuffer.get());
		m_Blackboard.Set(FrameResourceKey::LinearHDRColor, m_ForwardBuffer->LinearHDRBuffer.get());
		m_Blackboard.Set(FrameResourceKey::SceneColorLDR,  m_ForwardBuffer->SceneColorBuffer[0].get());

		const RenderParams activeParams =
			frame.Params != nullptr ? *frame.Params : *Scene::LightingSystem::GetAppliedParams();

		ColorBuffer* hdr   = m_ForwardBuffer->LinearHDRBuffer.get();
		ColorBuffer* ldr   = m_ForwardBuffer->SceneColorBuffer[0].get();
		DepthBuffer* depth = m_ForwardBuffer->DepthBuffer.get();

		m_PassList.Reset();
		m_PassList.AddPass("ForwardMainPass",
			{
				{ hdr,   PassAccess::WriteRTV },
				{ depth, PassAccess::WriteDepth },
			},
			[this, &frame, pass, batchType](GraphicsContext& ctx)
			{
				m_MainPass.Execute(ctx, frame, m_DrawHelper, m_Blackboard, pass, batchType, m_MainViewport, m_MainScissor, *m_Services);
			});

		m_PassList.AddPass("TonemapPass",
			{
				{ hdr, PassAccess::ReadSRV_Pixel },
				{ ldr, PassAccess::WriteRTV },
			},
			[this, activeParams](GraphicsContext& ctx)
			{
				// Forward DefaultPS 当前仅输出未打光 BaseColor，走共享 TonemapPass 旁路直通保持与基线一致
				m_TonemapPass.Execute(ctx, m_Blackboard, activeParams, /*bypassDisplayEncode=*/true, m_MainViewport, m_MainScissor, *m_Services);
			});

		context.PIXBeginEvent(L"ForwardRenderer");
		m_PassList.Execute(context, m_Services, frame.Timer);
		context.PIXEndEvent();
	}

	void ForwardRenderer::EndFrame(GraphicsContext& frameContext, const RenderFrameDesc& frame)
	{
		(void)frameContext;
		for (auto* model : frame.Models)
		{
			if (model != nullptr)
				model->ClearDirty();
		}
	}

	void ForwardRenderer::CreateForwardBufferTargets()
	{
		if (m_CreateDesc.width == 0 || m_CreateDesc.height == 0)
			return;

		const uint32_t width  = m_CreateDesc.width;
		const uint32_t height = m_CreateDesc.height;

		m_ForwardBuffer = std::make_shared<ForwardBuffer>();
		const Color forwardClearColor(0.10f, 0.12f, 0.16f, 1.0f);
		m_ForwardBuffer->LinearHDRBuffer = BufferManager::CreateColorBuffer(
			L"Forward LinearHDRColor", width, height, Renderer::Deferred::LinearHDRBufferFormat, 1, &forwardClearColor);
		m_ForwardBuffer->SceneColorBuffer[0] = BufferManager::CreateColorBuffer(
			L"Scene Color Buffer", width, height, Config::backBufferFormat, 1, &forwardClearColor);
		m_ForwardBuffer->DepthBuffer = BufferManager::CreateDepthBuffer(
			L"Scene Depth Buffer", width, height, Config::depthBufferFormat);
	}

	void ForwardRenderer::DestroyForwardBufferTargets()
	{
		if (!m_ForwardBuffer)
			return;

		BufferManager::ReleaseColorBuffer(L"Forward LinearHDRColor", m_ForwardBuffer->LinearHDRBuffer);
		BufferManager::ReleaseColorBuffer(L"Scene Color Buffer",     m_ForwardBuffer->SceneColorBuffer[0]);
		for (auto& color : m_ForwardBuffer->SceneColorBuffer)
		{
			if (color) color->Destroy();
		}
		BufferManager::ReleaseDepthBuffer(L"Scene Depth Buffer", m_ForwardBuffer->DepthBuffer);
		m_ForwardBuffer.reset();
	}
} // namespace Renderer::Forward
