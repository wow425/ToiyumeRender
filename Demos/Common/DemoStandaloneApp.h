// ============================================================================
// Demos/Common/DemoStandaloneApp.h
// 独立可执行 Demo 外壳：使任意 IDemoModule 既可单独编译运行，也可接入 RendererDemoApp 管理器
// ============================================================================
#pragma once

#include "00_Core/PCH.h"
#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "00_Core/Utility/Utility.h"

#include "01_Application/Display.h"
#include "01_Application/GameCore.h"
#include "01_Application/GameInput.h"

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"

#include "04_Renderer/DebugCapture.h"
#include "04_Renderer/GpuTimer.h"
#include "04_Renderer/ImGuiLayer.h"
#include "04_Renderer/RenderConfig.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RendererHost.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Model/Model.h"

#include "Demos/Common/DemoSceneHelpers.h"
#include "Demos/Common/IDemoModule.h"

#include "imgui.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace DemoCommon
{
	template <typename TModule>
	class DemoStandaloneApp final : public GameCore::IGameApp
	{
	public:
		DemoStandaloneApp() = default;

		void Startup() override
		{
			Asset::Initialize();
			Asset::LogResolution();

			CommandLineArgs::GetString(L"Screenshot", m_ScreenshotDir);
			CommandLineArgs::GetString(L"StatsOut", m_StatsOutPath);

			std::wstring rs;
			if (CommandLineArgs::GetString(L"ResizeStress", rs) && !rs.empty())
			{
				m_ResizeStressIterations = _wtoi(rs.c_str());
				if (m_ResizeStressIterations <= 0) m_ResizeStressIterations = 20;
			}
			std::wstring bw;
			if (CommandLineArgs::GetString(L"BatchWaitFrames", bw) && !bw.empty())
				m_BatchWaitFrames = _wtoi(bw.c_str());

			m_ForceRebuildOnLoad = CommandLineArgs::HasFlag(L"RebuildModels");

			const bool enableHotReload = m_ScreenshotDir.empty() && (m_ResizeStressIterations == 0);
			if (!m_Host.InitializeServices(enableHotReload))
			{
				LOG_ERROR(L"[%hs] RendererHost::InitializeServices 失败\n", m_Module.GetDisplayName());
				m_ExitCode = 1;
				PostQuitMessage(m_ExitCode);
				return;
			}

			const std::wstring pipeName = m_Module.GetPipelineName();
			if (!m_Host.SwitchPipeline(pipeName, Graphics::g_DisplayWidth, Graphics::g_DisplayHeight))
			{
				LOG_ERROR(L"[%hs] SwitchPipeline(%ws) 失败: %hs\n",
					m_Module.GetDisplayName(), pipeName.c_str(), m_Host.GetLastSwitchError().c_str());
				m_ExitCode = 2;
				PostQuitMessage(m_ExitCode);
				return;
			}
			BindOutput();

			m_GpuTimer.Initialize(Graphics::g_Device, 3);

			DemoHostContext ctx = BuildContext();
			m_Module.OnActivate(ctx, /*isFirstActivation=*/true);

			if (m_Module.ShouldEnsureDefaultModelOnActivate())
			{
				uint32_t startModel = 0;
				uint32_t cliModel = 0;
				uint32_t catalogCount = 0;
				GetModelCatalog(catalogCount);
				if (CommandLineArgs::GetInteger(L"ModelIndex", cliModel) && cliModel < catalogCount)
					startModel = cliModel;
				LoadModelByIndex(ctx, startModel, m_ForceRebuildOnLoad);
			}

			if (!Renderer::ImGuiLayer::Initialize(GameCore::GetWindowHandle(), &m_Host.GetServices().GetDescriptorHeap()))
				LOG_ERROR(L"[%hs] ImGuiLayer::Initialize 失败\n", m_Module.GetDisplayName());
			GameCore::SetWndProcHook(&DemoStandaloneApp::WndProcThunk);

			LOG_INFO(L"[%hs] 独立启动完成 (%ux%u)\n",
				m_Module.GetDisplayName(), Graphics::g_DisplayWidth, Graphics::g_DisplayHeight);
		}

		void Cleanup() override
		{
			ExportStatsIfRequested();

			DemoHostContext ctx = BuildContext();
			m_Module.OnDeactivate(ctx);
			m_CameraController.reset();

			GameCore::SetWndProcHook(nullptr);
			Renderer::ImGuiLayer::Shutdown();
			m_GpuTimer.Shutdown();

			if (Graphics::g_Device != nullptr)
				Graphics::g_CommandManager.IdleGPU();
			::Display::SceneColorBuffer = nullptr;

			m_Host.ShutdownPipeline();
			ClearAllModels(ctx);
			m_Host.ShutdownServices();
		}

		void OnResize(uint32_t width, uint32_t height) override
		{
			m_Host.OnResize(width, height);
			BindOutput();
			if (width > 0 && height > 0)
			{
				m_Camera.SetAspectRatio(static_cast<float>(height) / static_cast<float>(width));
				m_Camera.Update();
			}
		}

		void Update(float deltaT) override
		{
			if (m_ExitCode != 0)
			{
				PostQuitMessage(m_ExitCode);
				return;
			}

			if (m_ResizeStressIterations > 0)
			{
				const int iters = m_ResizeStressIterations;
				m_ResizeStressIterations = 0;
				const bool ok = RunResizeStress(iters);
				m_ExitCode = ok ? 0 : 1;
				PostQuitMessage(m_ExitCode);
				return;
			}

			ApplyPendingActions();

			if (GameInput::IsFirstPressed(GameInput::kKey_f5))
			{
				s_PendingHotReload = true;
				s_PendingHotReloadForceAll =
					GameInput::IsPressed(GameInput::kKey_lshift) || GameInput::IsPressed(GameInput::kKey_rshift);
			}
			if (s_PendingHotReload)
			{
				const bool forceAll = s_PendingHotReloadForceAll;
				s_PendingHotReload = false;
				s_PendingHotReloadForceAll = false;
				m_Host.TriggerShaderHotReload(forceAll);
			}

			m_CpuFrameMs = Graphics::GetFrameTime() * 1000.0f;
			m_GpuTimer.TryGetElapsedMs(m_GpuFrameMs);

			DemoHostContext ctx = BuildContext();
			ctx.FreezeCameraForScreenshot = !m_ScreenshotDir.empty();
			ctx.WantCaptureKeyboard = Renderer::ImGuiLayer::IsInitialized() && Renderer::ImGuiLayer::WantCaptureKeyboard();
			m_Module.Update(ctx, deltaT);

			if (!m_Host.GetActivePipeline() || !m_Host.GetActivePipeline()->IsInitialized())
				return;

			m_Host.GetServices().BeginFrame(Graphics::GetFrameCount());

			GraphicsContext& gfxContext = GraphicsContext::Begin(L"StandaloneDemo Update");
			Renderer::RenderFrameDesc desc = BuildFrameDesc(deltaT);
			m_Host.Update(desc, gfxContext);
			gfxContext.Finish();
		}

		void RenderScene() override
		{
			if (!m_Host.GetActivePipeline() || !m_Host.GetActivePipeline()->IsInitialized())
				return;

			GraphicsContext& gfxContext = GraphicsContext::Begin(L"StandaloneDemo Render");
			Renderer::RenderFrameDesc desc = BuildFrameDesc(0.0f);
			desc.Timer = &m_GpuTimer;

			m_GpuTimer.Begin(gfxContext);
			m_Host.Render(gfxContext, desc, Renderer::DrawPass::kOpaque);
			m_Host.EndFrame(gfxContext, desc);
			m_GpuTimer.End(gfxContext);

			m_LastHeapBindCount = gfxContext.GetSetDescriptorHeapsCount();
			ASSERT(m_LastHeapBindCount <= 1, "StandaloneDemo exceeded 1 SetDescriptorHeaps!");

			const uint64_t fenceValue = gfxContext.Finish();
			m_GpuTimer.OnSubmitted(fenceValue);

			++m_FrameCounter;
			if (!m_ScreenshotDir.empty())
			{
				if (m_FrameCounter == 1 || m_FrameCounter == 2 || m_FrameCounter == 5 ||
					m_FrameCounter == 30 || m_FrameCounter == 90)
				{
					SaveScreenshot(m_ScreenshotDir, L"screenshot", m_FrameCounter);
				}
				if (m_BatchWaitFrames > 0 && m_FrameCounter >= m_BatchWaitFrames)
				{
					ExportStatsIfRequested();
					PostQuitMessage(0);
				}
			}

			if (m_RequestCapture)
			{
				m_RequestCapture = false;
				const std::wstring dir = m_ScreenshotDir.empty() ? std::wstring(L".") : m_ScreenshotDir;
				SaveScreenshot(dir, L"ui_capture", ++m_CaptureCounter);
			}
		}

		void RenderUI(GraphicsContext& context) override
		{
			if (!Renderer::ImGuiLayer::IsInitialized() || !::Display::SceneColorBuffer)
				return;

			Renderer::ImGuiLayer::BeginFrame();

			DrawOverviewAndDevTools();
			DemoHostContext ctx = BuildContext();
			m_Module.DrawPanels(ctx);

			Renderer::ImGuiLayer::Render(context, *::Display::SceneColorBuffer);
		}

		bool HasRenderUI() const override { return true; }

	private:
		static bool WndProcThunk(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam)
		{
			if (msg == WM_KEYDOWN && wParam == VK_F5)
			{
				s_PendingHotReload = true;
				s_PendingHotReloadForceAll = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);
			}
			return Renderer::ImGuiLayer::WndProcHandler(hwnd, msg, wParam, lParam);
		}

		DemoHostContext BuildContext()
		{
			DemoHostContext ctx;
			ctx.Host                 = &m_Host;
			ctx.Params               = &m_Params;
			ctx.Camera               = &m_Camera;
			ctx.CameraController     = &m_CameraController;
			ctx.GpuTimer             = &m_GpuTimer;
			ctx.SceneModels          = &m_SceneModels;
			ctx.SelectedModel        = &m_SelectedModel;
			ctx.CurrentModelIndex    = &m_CurrentModelIndex;
			ctx.TargetRadius         = &m_TargetRadius;
			ctx.ForceRebuildOnLoad   = &m_ForceRebuildOnLoad;
			ctx.PendingAddModelIndex = &m_PendingAddModelIndex;
			ctx.PendingRemoveModel   = &m_PendingRemoveModel;
			ctx.ModelComboIndex      = &m_ModelComboIndex;
			ctx.CpuFrameMs           = m_CpuFrameMs;
			ctx.GpuFrameMs           = m_GpuFrameMs;
			ctx.LastHeapBindCount    = m_LastHeapBindCount;
			return ctx;
		}

		Renderer::RenderFrameDesc BuildFrameDesc(float deltaT)
		{
			Renderer::RenderFrameDesc desc;
			desc.Camera = &m_Camera;
			desc.delatT = deltaT;
			desc.Params = &m_Params;
			for (SceneModel& m : m_SceneModels)
			{
				if (m.instance && !m.instance->IsNull())
					desc.Models.push_back(m.instance.get());
			}
			return desc;
		}

		void BindOutput()
		{
			::Display::SceneColorBuffer = m_Host.GetOutputColorBuffer();
		}

		void ApplyPendingActions()
		{
			DemoHostContext ctx = BuildContext();
			if (m_PendingAddModelIndex >= 0)
			{
				const uint32_t idx = static_cast<uint32_t>(m_PendingAddModelIndex);
				m_PendingAddModelIndex = -1;
				AddModel(ctx, idx);
			}
			if (m_PendingRemoveModel != -1)
			{
				const int32_t target = m_PendingRemoveModel;
				m_PendingRemoveModel = -1;
				if (target == -2) ClearAllModels(ctx);
				else              RemoveModel(ctx, target);
			}
		}

		void SaveScreenshot(const std::wstring& dir, const wchar_t* tag, int index)
		{
			if (!::Display::SceneColorBuffer || !::Display::SceneColorBuffer->GetResource())
				return;
			wchar_t path[512] = {};
			swprintf_s(path, L"%ws\\%ws_%d.png", dir.c_str(), tag, index);
			const bool ok = Renderer::DebugCapture::SaveColorBufferToPNG(*::Display::SceneColorBuffer, path);
			LOG_INFO(L"[%hs] Screenshot %ws (%ws)\n", m_Module.GetDisplayName(), path, ok ? L"OK" : L"FAILED");
		}

		bool RunResizeStress(int iterations)
		{
			static const struct { uint32_t w, h; } kSizes[] = {
				{ 1280, 720 }, { 1920, 1080 }, { 800, 600 }, { 1600, 500 }, { 500, 900 },
			};
			::Display::Resize(kSizes[0].w, kSizes[0].h);
			const uint32_t rtvLiveBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
			const uint32_t srvLiveBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
			const uint32_t dsvLiveBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetLiveAllocated();

			for (int i = 0; i < iterations; ++i)
			{
				const auto& s = kSizes[(i + 1) % static_cast<int>(_countof(kSizes))];
				::Display::Resize(s.w, s.h);
			}

			const uint32_t rtvLiveEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
			const uint32_t srvLiveEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
			const uint32_t dsvLiveEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetLiveAllocated();
			const uint32_t doubleFree =
				Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetDoubleFreeCount() +
				Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetDoubleFreeCount() +
				Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetDoubleFreeCount();
			const bool passed = (rtvLiveEnd == rtvLiveBase) && (srvLiveEnd == srvLiveBase) && (dsvLiveEnd == dsvLiveBase) && (doubleFree == 0u);
			LOG_INFO(L"[%hs][ResizeStress] iters=%d live=(%u->%u,%u->%u,%u->%u) double_free=%u VERDICT=%ws\n",
				m_Module.GetDisplayName(), iterations,
				rtvLiveBase, rtvLiveEnd, srvLiveBase, srvLiveEnd, dsvLiveBase, dsvLiveEnd,
				doubleFree, passed ? L"PASS" : L"FAIL");
			return passed;
		}

		void ExportStatsIfRequested()
		{
			if (m_StatsExported || m_StatsOutPath.empty())
				return;
			m_StatsExported = true;

			Renderer::FrameStatsReport rep;
			rep.Pipeline     = m_Module.GetDisplayName();
			rep.Width        = Graphics::g_DisplayWidth;
			rep.Height       = Graphics::g_DisplayHeight;
			rep.FrameIndex   = Graphics::GetFrameCount();
			rep.CpuFrameMs   = m_CpuFrameMs;
			rep.GpuFrameMs   = m_GpuFrameMs;
			const Graphics::VramUsageInfo vram = Graphics::QueryVramUsage();
			rep.VramUsageMB  = static_cast<double>(vram.CurrentUsageBytes) / (1024.0 * 1024.0);
			rep.VramBudgetMB = static_cast<double>(vram.BudgetBytes) / (1024.0 * 1024.0);
			rep.BindlessSM66 = m_Host.GetServices().IsBindlessSM66Ready();
			rep.SetHeapCalls = m_LastHeapBindCount;
			for (const auto& pt : m_GpuTimer.GetPassTimings())
				rep.Passes.push_back({ pt.Name, pt.GpuMs });

			std::string err;
			Renderer::WriteStatsReportJson(m_StatsOutPath, rep, &err);
		}

		void DrawOverviewAndDevTools()
		{
			ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(330, 210), ImGuiCond_FirstUseEver);
			if (ImGui::Begin(m_Module.GetDisplayName()))
			{
				ImGui::Text("Pipeline: %s", NarrowAscii(m_Host.GetActivePipelineName()).c_str());
				ImGui::Text("CPU: %.2f ms (%.1f FPS) | GPU: %.3f ms",
					m_CpuFrameMs, (m_CpuFrameMs > 0.0f) ? 1000.0f / m_CpuFrameMs : 0.0f, m_GpuFrameMs);
				ImGui::Text("SetDescriptorHeaps/frame: %u", m_LastHeapBindCount);
				if (m_GpuTimer.IsInitialized())
				{
					for (const auto& pt : m_GpuTimer.GetPassTimings())
						ImGui::Text("  %-20s: %.3f ms", pt.Name.c_str(), pt.GpuMs);
				}
				ImGui::Separator();
				const bool postConnected = m_Host.GetActivePipeline() &&
					Renderer::HasFeature(m_Host.GetActivePipeline()->GetFeatures(), Renderer::RendererFeature::PostProcess);
				ImGui::BeginDisabled(!postConnected);
				ImGui::SliderFloat("Exposure", &m_Params.Exposure, 0.05f, 8.0f);
				ImGui::Checkbox("Tonemap (ACES)", &m_Params.EnableTonemap);
				ImGui::Checkbox("sRGB encode", &m_Params.EnableSRGBEncode);
				ImGui::EndDisabled();
				if (ImGui::Button("Capture Screenshot"))
					m_RequestCapture = true;
				ImGui::SameLine();
				if (m_Host.IsShaderHotReloadEnabled() && ImGui::Button("Reload Shaders (F5)"))
					s_PendingHotReload = true;
			}
			ImGui::End();
		}

	private:
		TModule                m_Module;
		Renderer::RendererHost m_Host;
		Renderer::RenderParams m_Params;
		Renderer::GpuTimer     m_GpuTimer;
		Scene::Camera::Camera  m_Camera;
		std::unique_ptr<Scene::Camera::CameraController> m_CameraController;

		std::vector<SceneModel> m_SceneModels;
		int32_t m_SelectedModel        = -1;
		int32_t m_CurrentModelIndex    = -1;
		float   m_TargetRadius         = 1.0f;
		bool    m_ForceRebuildOnLoad   = false;
		int32_t m_PendingAddModelIndex = -1;
		int32_t m_PendingRemoveModel   = -1;
		int     m_ModelComboIndex      = 0;

		std::wstring m_ScreenshotDir;
		std::wstring m_StatsOutPath;
		int          m_ResizeStressIterations = 0;
		int          m_BatchWaitFrames        = 0;
		int          m_FrameCounter           = 0;
		int          m_ExitCode               = 0;
		bool         m_StatsExported          = false;
		bool         m_RequestCapture         = false;
		int          m_CaptureCounter         = 0;
		float        m_CpuFrameMs             = 0.0f;
		float        m_GpuFrameMs             = 0.0f;
		uint32_t     m_LastHeapBindCount      = 0;

		static inline bool s_PendingHotReload         = false;
		static inline bool s_PendingHotReloadForceAll = false;
	};
}
