// ============================================================================
// 模块：RendererHost（渲染管线宿主 · 实现）
// 架构层：Engine/04_Renderer（契约 2 / WP-2B）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/RendererHost.h"

#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"

namespace Renderer
{
	bool RendererHost::InitializeServices(bool enableShaderHotReload, const std::wstring& repoRoot)
	{
		if (!m_Services.Initialize())
			return false;

		std::wstring rootDir = repoRoot;
#ifdef TOOIYUME_SOURCE_ROOT_DIR
		if (rootDir.empty())
			rootDir = TOOIYUME_SOURCE_ROOT_DIR;
#endif
		m_HotReloader.Initialize(rootDir, enableShaderHotReload);
		return true;
	}

	void RendererHost::ShutdownPipeline()
	{
		m_HotReloader.ClearRegistrations();
		if (m_ActivePipeline)
		{
			if (Graphics::g_Device != nullptr)
				Graphics::g_CommandManager.IdleGPU();
			m_ActivePipeline->Shutdown();
			m_ActivePipeline.reset();
		}
		m_ActiveName.clear();
	}

	void RendererHost::ShutdownServices()
	{
		// [契约 2 严格反序] 先关热重载器，再关活跃管线，最后关 RenderServices
		m_HotReloader.Shutdown();
		ShutdownPipeline();
		m_Services.Shutdown();
	}

	Graphics::ShaderReloadReport RendererHost::TriggerShaderHotReload(bool forceAll)
	{
		m_LastReloadReport = m_HotReloader.ReloadModified(forceAll);
		return m_LastReloadReport;
	}

	bool RendererHost::SwitchPipeline(const std::wstring& name, uint32_t width, uint32_t height, bool simulateFailure)
	{
		if (name.empty())
			return false;

		if (!RendererRegistry::Instance().HasRenderer(name))
		{
			LOG_ERROR(L"[RendererHost] 未知管线：%ws（注册表里没有这个名字）\n", name.c_str());
			return false;
		}

		if (!simulateFailure && m_ActivePipeline && m_ActivePipeline->IsInitialized() && m_ActiveName == name)
			return true;

		LOG_INFO(L"[WP-1B] 切换 Renderer（先建新后替换，不重载模型）：%ws -> %ws%ws\n",
			m_ActiveName.c_str(), name.c_str(), simulateFailure ? L" [SimulateInitFailure]" : L"");

		std::wstring mutableName = name;
		RendererPtr candidate = RendererRegistry::Instance().CreateRenderer(mutableName);
		if (!candidate)
		{
			LOG_ERROR(L"[WP-1B][TD-08a] CreateRenderer 返回空：%ws（旧管线 %ws 与 UI 保持完好）\n",
				name.c_str(), m_ActiveName.c_str());
			return false;
		}

		m_LastSwitchError.clear();

		// [WP-3A / 契约 2 先备后切第 1 步] CheckSupport 硬件能力预检（不满足严禁静默冒名回退！）
		const AlgorithmCapsReport capsReport = candidate->CheckSupport(Graphics::GetGpuCaps(), width, height);
		if (!capsReport.Supported)
		{
			m_LastSwitchError = capsReport.UnsupportedReason.empty()
				? "Pipeline unsupported on current GPU"
				: capsReport.UnsupportedReason;
			LOG_ERROR(L"[RendererHost][CheckSupport] 拒绝切换至 %ws：%hs（旧管线 %ws 保持完好）\n",
				name.c_str(), m_LastSwitchError.c_str(), m_ActiveName.c_str());
			candidate.reset();
			return false;
		}

		// [WP-3A / 契约 2 先备后切第 2 步] VramBudgetGuard 双驻留前显存预检（超限拒切，严禁静默降分辨率！）
		std::string vramReason;
		if (!Graphics::CheckVramBudgetForAllocation(capsReport.EstimatedVramBytes, 0.90f, &vramReason))
		{
			m_LastSwitchError = vramReason;
			LOG_ERROR(L"[RendererHost][VramBudgetGuard] 拒绝切换至 %ws：%hs（旧管线 %ws 保持完好）\n",
				name.c_str(), m_LastSwitchError.c_str(), m_ActiveName.c_str());
			candidate.reset();
			return false;
		}

		RendererCreateDesc desc;
		desc.width = width;
		desc.height = height;
		desc.services = &m_Services;
		desc.simulateInitFailure = simulateFailure;

		if (!candidate->Initialize(desc))
		{
			m_LastSwitchError = "Candidate pipeline Initialize() failed";
			LOG_ERROR(L"[WP-1B][TD-08a] 候选 Renderer::Initialize 失败：%ws，已丢弃候选并保留旧管线 %ws\n",
				name.c_str(), m_ActiveName.c_str());
			candidate->Shutdown();
			candidate.reset();
			return false;
		}

		// 候选初始化成功：帧边界排空 GPU 后原子替换并释放旧管线（不触碰 RenderServices 与模型资产）
		if (Graphics::g_Device != nullptr)
			Graphics::g_CommandManager.IdleGPU();

		RendererPtr oldPipeline = std::move(m_ActivePipeline);
		m_ActivePipeline = std::move(candidate);
		m_ActiveName = name;

		if (oldPipeline)
		{
			oldPipeline->Shutdown();
			oldPipeline.reset();
		}

		// 注册新活跃管线的可热重载着色器
		m_HotReloader.ClearRegistrations();
		if (m_ActivePipeline)
			m_ActivePipeline->RegisterHotReloadShaders(m_HotReloader, m_Services);

		return true;
	}

	void RendererHost::OnResize(uint32_t width, uint32_t height)
	{
		if (m_ActivePipeline && m_ActivePipeline->IsInitialized())
			m_ActivePipeline->OnResize(width, height);
	}

	void RendererHost::BeginFrame(uint64_t frameIndex, const RenderFrameDesc& frame)
	{
		m_Services.BeginFrame(frameIndex);
		if (m_ActivePipeline && m_ActivePipeline->IsInitialized())
			m_ActivePipeline->BeginFrame(frame);
	}

	void RendererHost::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
	{
		if (m_ActivePipeline && m_ActivePipeline->IsInitialized())
			m_ActivePipeline->Update(frame, gfxContext);
	}

	void RendererHost::Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass)
	{
		if (m_ActivePipeline && m_ActivePipeline->IsInitialized())
			m_ActivePipeline->Render(context, frame, pass);
	}

	void RendererHost::EndFrame(GraphicsContext& context, const RenderFrameDesc& frame)
	{
		if (m_ActivePipeline && m_ActivePipeline->IsInitialized())
			m_ActivePipeline->EndFrame(context, frame);
	}

	std::shared_ptr<ColorBuffer> RendererHost::GetOutputColorBuffer() const
	{
		if (!m_ActivePipeline)
			return nullptr;
		return m_ActivePipeline->GetOutputColorBuffer();
	}
}
