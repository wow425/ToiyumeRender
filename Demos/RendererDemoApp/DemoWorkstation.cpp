// ============================================================================
// Demos/RendererDemoApp/DemoWorkstation.cpp
// 统一图形研究工作站实现（单模块独立运行 / 多模块切换器二合一宿主）
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/RendererDemoApp/DemoWorkstation.h"

#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "00_Core/Utility/Utility.h"
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "01_Application/Display.h"
#include "01_Application/GameCore.h"
#include "01_Application/GameInput.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Resource/ColorBuffer.h"

#include "04_Renderer/DebugCapture.h"
#include "04_Renderer/GpuTimer.h"
#include "04_Renderer/ImGuiLayer.h"
#include "04_Renderer/PrimarySurfaceDesc.h"
#include "04_Renderer/RenderConfig.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "04_Renderer/RendererHost.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
#include "05_Scene/Model/ModelLoader.h"

#include "imgui.h"

#include <cstdio>

namespace Demo
{
	namespace
	{
		const ModelEntry kCatalog[] = {
			{ "merlin",              L"Model/Merlin/merlin.gltf" },
			{ "leimiaier",           L"Model/leimiaier/leimiaierALL.gltf" },
			{ "saber_emiya",         L"Model/Saber/saber_emiya.glb" },
			{ "saberCaster3NoArmor", L"Model/saberCaster/saberCaster3NoArmor.gltf" },
			{ "ChosenSword",         L"Model/saberCaster/ChosenSword.gltf" },
			{ "KyomachiyaA",         L"Model/Kyomachiya/KyomachiyaA.gltf" },
			{ "toneriko1",           L"Model/toneriko/toneriko1.gltf" },
		};

		const char* FeatureName(Renderer::RendererFeature feature)
		{
			switch (feature)
			{
			case Renderer::RendererFeature::Forward:     return "Forward";
			case Renderer::RendererFeature::Deferred:    return "Deferred";
			case Renderer::RendererFeature::Shadow:      return "Shadow";
			case Renderer::RendererFeature::SSAO:        return "SSAO";
			case Renderer::RendererFeature::TAA:         return "TAA";
			case Renderer::RendererFeature::Transparent: return "Transparent";
			case Renderer::RendererFeature::PostProcess: return "PostProcess";
			case Renderer::RendererFeature::Raytracing:  return "Raytracing";
			default:                                     return "None";
			}
		}

		std::string FeaturesToString(Renderer::RendererFeature mask)
		{
			std::string text;
			const Renderer::RendererFeature all[] = {
				Renderer::RendererFeature::Forward,     Renderer::RendererFeature::Deferred,
				Renderer::RendererFeature::Shadow,      Renderer::RendererFeature::SSAO,
				Renderer::RendererFeature::TAA,         Renderer::RendererFeature::Transparent,
				Renderer::RendererFeature::PostProcess, Renderer::RendererFeature::Raytracing,
			};
			for (Renderer::RendererFeature f : all)
			{
				if (!Renderer::HasFeature(mask, f))
					continue;
				if (!text.empty())
					text += " | ";
				text += FeatureName(f);
			}
			return text.empty() ? "None" : text;
		}

		class WorkstationApp final : public GameCore::IGameApp
		{
		public:
			explicit WorkstationApp(std::vector<std::unique_ptr<IDemoModule>> modules)
				: m_Modules(std::move(modules))
			{
			}

			void Startup() override
			{
				Asset::Initialize();
				Asset::LogResolution();

				// ---- 1. 通用 CLI 参数解析 ----
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

				uint32_t switchCount = 0;
				if (CommandLineArgs::GetInteger(L"SwitchTest", switchCount) && switchCount > 0)
				{
					m_SwitchTestMode = true;
					m_SwitchTestTarget = switchCount;
				}

				m_ForceRebuildOnLoad = CommandLineArgs::HasFlag(L"RebuildModels");

				std::wstring startName = m_Modules.empty()
					? std::wstring(L"DeferredRenderer")
					: m_Modules.front()->GetPipelineName();
				bool cliSpecifiedStart = false;
				{
					std::wstring cliRenderer;
					if (CommandLineArgs::GetString(L"StartRenderer", cliRenderer) && !cliRenderer.empty())
					{
						startName = cliRenderer;
						cliSpecifiedStart = true;
					}
				}
				{
					std::wstring presetPath;
					if (CommandLineArgs::GetString(L"Preset", presetPath) && !presetPath.empty())
					{
						if (LoadPresetFromFile(presetPath, /*switchPipeline=*/false) && !cliSpecifiedStart)
							startName = m_Config.GetPipelineRendererName();
					}
				}

				uint32_t startModel = 0;
				{
					uint32_t cliModel = 0;
					uint32_t catalogCount = 0;
					GetModelCatalog(catalogCount);
					if (CommandLineArgs::GetInteger(L"ModelIndex", cliModel) && cliModel < catalogCount)
						startModel = cliModel;
				}

				m_ModuleNames.clear();
				for (const auto& mod : m_Modules)
					m_ModuleNames.push_back(mod->GetDisplayName());

				// ---- 2. 初始化统一渲染服务与初始模块 ----
				const bool enableHotReload = m_ScreenshotDir.empty() && (m_ResizeStressIterations == 0);
				if (!m_Host.InitializeServices(enableHotReload))
				{
					LOG_ERROR(L"[Workstation] RendererHost::InitializeServices 失败\n");
					m_ExitCode = 1;
					PostQuitMessage(m_ExitCode);
					return;
				}

				if (!SwitchModule(startName))
				{
					LOG_ERROR(L"[Workstation] 启动管线 %ws 失败: %hs\n",
						startName.c_str(), m_Host.GetLastSwitchError().c_str());
					m_ExitCode = 2;
					PostQuitMessage(m_ExitCode);
					return;
				}

				m_GpuTimer.Initialize(Graphics::g_Device, 3);
				if (m_ActiveModule == nullptr || m_ActiveModule->ShouldEnsureDefaultModelOnActivate() ||
					cliSpecifiedStart || m_ForceRebuildOnLoad)
				{
					HostContext ctx = BuildContext();
					LoadModelByIndex(ctx, startModel, m_ForceRebuildOnLoad);
				}

				if (!m_CameraController)
				{
					HostContext ctx = BuildContext();
					SetupStandardRightHandedCamera(ctx, /*resetPose=*/true);
				}

				if (!Renderer::ImGuiLayer::Initialize(GameCore::GetWindowHandle(), &m_Host.GetServices().GetDescriptorHeap()))
					LOG_ERROR(L"[Workstation] ImGuiLayer::Initialize 失败\n");
				GameCore::SetWndProcHook(&WorkstationApp::WndProcThunk);

				LOG_INFO(L"[Workstation] 启动完成 (modules=%zu, active=%hs, %ux%u)\n",
					m_Modules.size(),
					m_ActiveModule ? m_ActiveModule->GetDisplayName() : "<none>",
					Graphics::g_DisplayWidth, Graphics::g_DisplayHeight);
			}

			void Cleanup() override
			{
				ExportStatsIfRequested();

				HostContext ctx = BuildContext();
				if (m_ActiveModule != nullptr)
					m_ActiveModule->OnDeactivate(ctx);
				m_CameraController.reset();

				GameCore::SetWndProcHook(nullptr);
				Renderer::ImGuiLayer::Shutdown();
				m_GpuTimer.Shutdown();

				if (Graphics::g_Device != nullptr)
					Graphics::g_CommandManager.IdleGPU();
				Display::SceneColorBuffer = nullptr;

				m_Host.ShutdownPipeline();
				m_ActiveModule = nullptr;
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

				if (m_SwitchTestMode)
					RunSwitchTest();

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
					const auto rep = m_Host.TriggerShaderHotReload(forceAll);
					LOG_INFO(L"[HotReload] F5 (forceAll=%d): %hs\n", forceAll ? 1 : 0, rep.Summary.c_str());
				}

				m_CpuFrameMs = Graphics::GetFrameTime() * 1000.0f;
				m_GpuTimer.TryGetElapsedMs(m_GpuFrameMs);
				if (m_BatchWaitFrames <= 0)
					ExportStatsIfRequested();

				HostContext ctx = BuildContext();
				if (m_ActiveModule != nullptr)
					m_ActiveModule->Update(ctx, deltaT);
				else if (m_CameraController)
					m_CameraController->Update(deltaT);

				auto* pipe = m_Host.GetActivePipeline();
				if (pipe == nullptr || !pipe->IsInitialized())
					return;

				m_Host.GetServices().BeginFrame(Graphics::GetFrameCount());

				GraphicsContext& gfxContext = GraphicsContext::Begin(L"Workstation Update");
				Renderer::RenderFrameDesc desc = BuildFrameDesc(deltaT);
				m_Host.Update(desc, gfxContext);
				gfxContext.Finish();
			}

			void RenderScene() override
			{
				auto* pipe = m_Host.GetActivePipeline();
				if (pipe == nullptr || !pipe->IsInitialized())
					return;

				GraphicsContext& gfxContext = GraphicsContext::Begin(L"Workstation Render");
				Renderer::RenderFrameDesc desc = BuildFrameDesc(0.0f);
				desc.Timer = &m_GpuTimer;

				m_GpuTimer.Begin(gfxContext);
				m_Host.Render(gfxContext, desc, Renderer::DrawPass::kOpaque);
				m_Host.EndFrame(gfxContext, desc);
				m_GpuTimer.End(gfxContext);

				m_LastHeapBindCount = gfxContext.GetSetDescriptorHeapsCount();
				ASSERT(m_LastHeapBindCount <= 1, "Workstation RenderScene exceeded 1 SetDescriptorHeaps!");

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

				if (m_CaptureRequested)
				{
					m_CaptureRequested = false;
					const std::wstring dir = m_ScreenshotDir.empty() ? std::wstring(L".") : m_ScreenshotDir;
					SaveScreenshot(dir, L"ui_capture", ++m_CaptureCounter);
				}
			}

			void RenderUI(GraphicsContext& context) override
			{
				if (!Renderer::ImGuiLayer::IsInitialized() || !Display::SceneColorBuffer)
					return;

				Renderer::ImGuiLayer::BeginFrame();

				DrawPerformancePanel();
				if (m_Modules.size() > 1)
					DrawSwitcherPanel();
				DrawDevToolsPanel();

				if (m_ActiveModule != nullptr)
				{
					HostContext ctx = BuildContext();
					m_ActiveModule->DrawPanels(ctx);
				}

				if (m_ShowDemoWindow)
					ImGui::ShowDemoWindow(&m_ShowDemoWindow);

				Renderer::ImGuiLayer::Render(context, *Display::SceneColorBuffer);
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

			HostContext BuildContext()
			{
				HostContext ctx;
				ctx.Host                      = &m_Host;
				ctx.Params                    = &m_Params;
				ctx.Camera                    = &m_Camera;
				ctx.CameraController          = &m_CameraController;
				ctx.GpuTimer                  = &m_GpuTimer;
				ctx.SceneModels               = &m_SceneModels;
				ctx.SelectedModel             = &m_SelectedModel;
				ctx.CurrentModelIndex         = &m_CurrentModelIndex;
				ctx.TargetRadius              = &m_TargetRadius;
				ctx.ForceRebuildOnLoad        = &m_ForceRebuildOnLoad;
				ctx.PendingAddModelIndex      = &m_PendingAddModelIndex;
				ctx.PendingRemoveModel        = &m_PendingRemoveModel;
				ctx.ModelComboIndex           = &m_ModelComboIndex;
				ctx.FreezeCameraForScreenshot = !m_ScreenshotDir.empty();
				ctx.WantCaptureKeyboard       = Renderer::ImGuiLayer::IsInitialized() && Renderer::ImGuiLayer::WantCaptureKeyboard();
				ctx.CpuFrameMs                = m_CpuFrameMs;
				ctx.GpuFrameMs                = m_GpuFrameMs;
				ctx.LastHeapBindCount         = m_LastHeapBindCount;
				return ctx;
			}

			Renderer::RenderFrameDesc BuildFrameDesc(float deltaT) const
			{
				Renderer::RenderFrameDesc desc;
				desc.Camera = &m_Camera;
				desc.delatT = deltaT;
				for (const SceneModel& m : m_SceneModels)
				{
					if (m.instance && !m.instance->IsNull())
						desc.Models.push_back(m.instance.get());
				}
				desc.Params = const_cast<Renderer::RenderParams*>(&m_Params);
				return desc;
			}

			IDemoModule* FindModule(const std::wstring& nameOrPipeline) const
			{
				const std::string narrowQuery = NarrowAscii(nameOrPipeline);
				for (const auto& mod : m_Modules)
				{
					if (mod->GetPipelineName() == nameOrPipeline || narrowQuery == mod->GetDisplayName())
						return mod.get();
				}
				return nullptr;
			}

			bool SwitchModule(const std::wstring& name)
			{
				IDemoModule* targetMod = FindModule(name);
				if (targetMod == nullptr && !m_Modules.empty() && m_Modules.size() == 1)
					targetMod = m_Modules.front().get();

				const std::wstring pipelineName = (targetMod != nullptr) ? targetMod->GetPipelineName() : name;
				const bool isFirstActivation = (m_ActiveModule == nullptr);

				if (!m_Host.SwitchPipeline(pipelineName, Graphics::g_DisplayWidth, Graphics::g_DisplayHeight))
					return false;

				HostContext ctx = BuildContext();
				if (m_ActiveModule != nullptr && m_ActiveModule != targetMod)
					m_ActiveModule->OnDeactivate(ctx);

				m_ActiveModule = targetMod;
				for (size_t i = 0; i < m_Modules.size(); ++i)
				{
					if (m_Modules[i].get() == m_ActiveModule)
					{
						m_ModuleComboIndex = static_cast<int>(i);
						break;
					}
				}

				BindOutput();
				if (m_ActiveModule != nullptr)
					m_ActiveModule->OnActivate(ctx, isFirstActivation);

				LOG_INFO(L"[Workstation] 激活模块: %hs (%ws)\n",
					m_ActiveModule ? m_ActiveModule->GetDisplayName() : "<Unknown>",
					m_Host.GetActivePipelineName().c_str());
				return true;
			}

			void BindOutput()
			{
				Display::SceneColorBuffer = m_Host.GetOutputColorBuffer();
			}

			void ApplyPendingActions()
			{
				if (!m_PendingModuleName.empty())
				{
					const std::wstring name = m_PendingModuleName;
					m_PendingModuleName.clear();
					SwitchModule(name);
				}

				HostContext ctx = BuildContext();
				if (m_PendingAddModelIndex >= 0)
				{
					const uint32_t idx = static_cast<uint32_t>(m_PendingAddModelIndex);
					m_PendingAddModelIndex = -1;
					AddModel(ctx, idx, m_ForceRebuildOnLoad);
				}
				if (m_PendingRemoveModel != -1)
				{
					const int32_t target = m_PendingRemoveModel;
					m_PendingRemoveModel = -1;
					if (target == -2) ClearAllModels(ctx);
					else              RemoveModel(ctx, target);
				}
			}

			void RunSwitchTest()
			{
				static constexpr int kFramesPerSwitch = 3;
				if ((m_SwitchTestFrame++ % kFramesPerSwitch) != 0)
					return;

				if (m_SwitchTestCount >= m_SwitchTestTarget)
				{
					LOG_INFO(L"[SwitchTest] PASS: completed %u module switches without crash/heap corruption\n", m_SwitchTestCount);
					PostQuitMessage(0);
					return;
				}
				if (m_Modules.size() < 2)
				{
					PostQuitMessage(0);
					return;
				}

				IDemoModule* nextMod = m_Modules[m_SwitchTestCount % m_Modules.size()].get();
				if (nextMod == m_ActiveModule)
					nextMod = m_Modules[(m_SwitchTestCount + 1) % m_Modules.size()].get();

				++m_SwitchTestCount;
				LOG_INFO(L"[SwitchTest] switch %u/%u -> %hs\n",
					m_SwitchTestCount, m_SwitchTestTarget, nextMod->GetDisplayName());
				SwitchModule(nextMod->GetPipelineName());

#ifdef _DEBUG
				const int heapOk = _CrtCheckMemory();
				ASSERT(heapOk != 0, "CRT debug heap corruption detected after SwitchModule");
				LOG_INFO(L"[SwitchTest] switch %u/%u CRT heap check = %d\n",
					m_SwitchTestCount, m_SwitchTestTarget, heapOk);
#endif
			}

			bool RunResizeStress(int iterations)
			{
				static const struct { uint32_t w, h; } kSizes[] = {
					{ 1280, 720 }, { 1920, 1080 }, { 800, 600 }, { 1600, 500 }, { 500, 900 },
				};
				Display::Resize(kSizes[0].w, kSizes[0].h);
				const uint32_t rtvBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
				const uint32_t srvBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
				const uint32_t dsvBase = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetLiveAllocated();

				for (int i = 0; i < iterations; ++i)
				{
					const auto& s = kSizes[(i + 1) % static_cast<int>(_countof(kSizes))];
					Display::Resize(s.w, s.h);
				}

				const uint32_t rtvEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
				const uint32_t srvEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
				const uint32_t dsvEnd = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetLiveAllocated();
				const uint32_t doubleFree =
					Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetDoubleFreeCount() +
					Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetDoubleFreeCount() +
					Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetDoubleFreeCount();
				const bool passed = (rtvEnd == rtvBase) && (srvEnd == srvBase) && (dsvEnd == dsvBase) && (doubleFree == 0u);
				LOG_INFO(L"[Workstation][ResizeStress] iters=%d live=(%u->%u,%u->%u,%u->%u) double_free=%u VERDICT=%ws\n",
					iterations, rtvBase, rtvEnd, srvBase, srvEnd, dsvBase, dsvEnd,
					doubleFree, passed ? L"PASS" : L"FAIL");
				return passed;
			}

			void SaveScreenshot(const std::wstring& dir, const wchar_t* tag, int index)
			{
				if (!Display::SceneColorBuffer || !Display::SceneColorBuffer->GetResource())
					return;
				wchar_t path[512] = {};
				swprintf_s(path, L"%ws\\%ws_%d.png", dir.c_str(), tag, index);
				const bool ok = Renderer::DebugCapture::SaveColorBufferToPNG(*Display::SceneColorBuffer, path);
				LOG_INFO(L"[Workstation] 截帧 %ws (%ws)\n", path, ok ? L"OK" : L"FAILED");
			}

			bool SavePresetToFile(const std::wstring& path)
			{
				m_Config.SetPipelineFromRendererName(m_Host.GetActivePipelineName());
				m_Config.FromRenderParams(m_Params);
				std::string err;
				if (!m_Config.SaveToFile(path, &err))
				{
					m_PresetStatus = "Save failed: " + err;
					return false;
				}
				m_PresetStatus = "Saved: " + NarrowAscii(path);
				return true;
			}

			bool LoadPresetFromFile(const std::wstring& path, bool switchPipeline)
			{
				Renderer::RenderConfig loaded;
				std::string err;
				if (!loaded.LoadFromFile(path, &err))
				{
					m_PresetStatus = "Load failed: " + err;
					return false;
				}
				m_Config = loaded;
				m_Params = m_Config.ToRenderParams();
				if (switchPipeline)
				{
					const std::wstring targetPipe = m_Config.GetPipelineRendererName();
					if (!targetPipe.empty() && targetPipe != m_Host.GetActivePipelineName())
						m_PendingModuleName = targetPipe;
				}
				m_PresetStatus = "Loaded: " + NarrowAscii(path);
				return true;
			}

			void ExportStatsIfRequested()
			{
				if (m_StatsWritten || m_StatsOutPath.empty())
					return;
				if (m_BatchWaitFrames <= 0 && m_GpuTimer.GetPassTimings().empty() && Graphics::GetFrameCount() < 15)
					return;

				m_StatsWritten = true;
				Renderer::FrameStatsReport rep;
				rep.Pipeline     = NarrowAscii(m_Host.GetActivePipelineName());
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

			void DrawPerformancePanel()
			{
				ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(ImVec2(330, 210), ImGuiCond_FirstUseEver);
				if (!ImGui::Begin("Performance"))
				{
					ImGui::End();
					return;
				}

				ImGui::Text("CPU frame: %.2f ms (%.1f FPS)",
					m_CpuFrameMs, (m_CpuFrameMs > 0.0f) ? 1000.0f / m_CpuFrameMs : 0.0f);
				if (m_GpuTimer.IsInitialized())
				{
					ImGui::Text("GPU frame: %.3f ms", m_GpuFrameMs);
					for (const auto& pt : m_GpuTimer.GetPassTimings())
						ImGui::Text("  %-18s: %.3f ms", pt.Name.c_str(), pt.GpuMs);
					bool enabled = m_GpuTimer.IsEnabled();
					if (ImGui::Checkbox("GPU timer enabled", &enabled))
						m_GpuTimer.SetEnabled(enabled);
				}
				else
				{
					ImGui::TextDisabled("GPU frame: n/a");
				}

				ImGui::Separator();
				ImGui::Text("Frame: %llu | SetHeaps/frame: %u",
					static_cast<unsigned long long>(Graphics::GetFrameCount()), m_LastHeapBindCount);
				const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
				const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
				ImGui::Text("Desc live: RTV %u / SRV %u", rtvLive, srvLive);
				if (m_Host.GetServices().IsInitialized())
				{
					const auto& uheap = m_Host.GetServices().GetDescriptorHeap();
					ImGui::Text("UnifiedHeap A:%u B:%u C:%u D:%u",
						uheap.GetZoneALiveCount(), uheap.GetZoneBUsedInCurrentFrame(),
						uheap.GetZoneCLiveCount(), uheap.GetZoneDLiveCount());
					ImGui::Text("Bindless SM6.6: %s",
						m_Host.GetServices().IsBindlessSM66Ready() ? "ACTIVE" : "Fallback SM6.4");
				}
				const Graphics::VramUsageInfo vram = Graphics::QueryVramUsage();
				ImGui::Text("VRAM (local): %.1f / %.1f MB",
					static_cast<double>(vram.CurrentUsageBytes) / (1024.0 * 1024.0),
					static_cast<double>(vram.BudgetBytes) / (1024.0 * 1024.0));

				ImGui::End();
			}

			void DrawSwitcherPanel()
			{
				ImGui::SetNextWindowPos(ImVec2(16, 620), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(ImVec2(360, 250), ImGuiCond_FirstUseEver);
				if (!ImGui::Begin("Pipeline Switcher"))
				{
					ImGui::End();
					return;
				}

				ImGui::Text("Active Demo: %s", m_ActiveModule ? m_ActiveModule->GetDisplayName() : "<none>");
				ImGui::TextDisabled("Pipeline:    %s", NarrowAscii(m_Host.GetActivePipelineName()).c_str());

				std::vector<const char*> names;
				names.reserve(m_ModuleNames.size());
				for (const std::string& n : m_ModuleNames)
					names.push_back(n.c_str());
				if (!names.empty())
					ImGui::Combo("Available", &m_ModuleComboIndex, names.data(), static_cast<int>(names.size()));

				const bool sameAsCurrent =
					m_ActiveModule != nullptr &&
					m_ModuleComboIndex >= 0 && m_ModuleComboIndex < static_cast<int>(m_ModuleNames.size()) &&
					m_ModuleNames[static_cast<size_t>(m_ModuleComboIndex)] == m_ActiveModule->GetDisplayName();

				ImGui::BeginDisabled(sameAsCurrent || m_ModuleNames.empty());
				if (ImGui::Button("Switch pipeline"))
				{
					const std::string& target = m_ModuleNames[static_cast<size_t>(m_ModuleComboIndex)];
					m_PendingModuleName = std::wstring(target.begin(), target.end());
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::TextDisabled("(next frame)");

				if (auto* pipe = m_Host.GetActivePipeline())
				{
					ImGui::Separator();
					ImGui::TextWrapped("Features: %s", FeaturesToString(pipe->GetFeatures()).c_str());
					if (const auto* surf = pipe->GetPrimarySurface(); surf != nullptr && surf->IsComplete())
					{
						ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
							"PrimarySurface: %s (%ux%u, Bindless=%s)",
							Renderer::PrimarySurfaceSourceName(surf->Source),
							surf->Width, surf->Height,
							surf->IsBindlessReady() ? "OK" : "OFF");
					}
				}

				ImGui::End();
			}

			void DrawDevToolsPanel()
			{
				ImGui::SetNextWindowPos(ImVec2(390, 180), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(ImVec2(300, 220), ImGuiCond_FirstUseEver);
				if (!ImGui::Begin("DevTools"))
				{
					ImGui::End();
					return;
				}

				auto* pipe = m_Host.GetActivePipeline();
				const bool postConnected = pipe &&
					Renderer::HasFeature(pipe->GetFeatures(), Renderer::RendererFeature::PostProcess);
				ImGui::BeginDisabled(!postConnected);
				ImGui::SliderFloat("Exposure", &m_Params.Exposure, 0.05f, 8.0f);
				ImGui::Checkbox("Tonemap (ACES)", &m_Params.EnableTonemap);
				ImGui::Checkbox("sRGB encode", &m_Params.EnableSRGBEncode);
				ImGui::EndDisabled();
				if (!postConnected)
					ImGui::TextDisabled("TonemapPass bypassed on this pipeline.");

				ImGui::Separator();
				ImGui::Text("Prefilter mip range: 0 .. %.0f", Scene::LightingSystem::GetPrefilterMaxMip());

				if (ImGui::Button("Capture SceneColor"))
					m_CaptureRequested = true;
				ImGui::SameLine();
				if (ImGui::Button("ImGui Demo"))
					m_ShowDemoWindow = !m_ShowDemoWindow;

				ImGui::Separator();
				static constexpr wchar_t kDefaultPresetPath[] = L"preset_last.json";
				if (ImGui::Button("Save Preset"))
					SavePresetToFile(kDefaultPresetPath);
				ImGui::SameLine();
				if (ImGui::Button("Load Preset"))
					LoadPresetFromFile(kDefaultPresetPath, /*switchPipeline=*/(m_Modules.size() > 1));
				if (!m_PresetStatus.empty())
					ImGui::TextWrapped("%s", m_PresetStatus.c_str());

				ImGui::Separator();
				if (m_Host.IsShaderHotReloadEnabled())
				{
					if (ImGui::Button("Reload Shaders (F5)"))
					{
						s_PendingHotReload = true;
						s_PendingHotReloadForceAll = false;
					}
					ImGui::SameLine();
					if (ImGui::Button("Force All"))
					{
						s_PendingHotReload = true;
						s_PendingHotReloadForceAll = true;
					}
					const auto& rep = m_Host.GetLastReloadReport();
					if (!rep.Summary.empty())
						ImGui::TextDisabled("%s", rep.Summary.c_str());
					if (!rep.LastError.empty())
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
						ImGui::TextWrapped("DXC ERROR: %s", rep.LastError.c_str());
						ImGui::PopStyleColor();
					}
				}
				else
				{
					ImGui::TextDisabled("Hot reload disabled in batch/screenshot mode");
				}

				ImGui::End();
			}

		private:
			std::vector<std::unique_ptr<IDemoModule>> m_Modules;
			std::vector<std::string>                  m_ModuleNames;
			IDemoModule*                              m_ActiveModule = nullptr;

			Renderer::RendererHost m_Host;
			Renderer::RenderParams m_Params;
			Renderer::RenderConfig m_Config;
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
			int     m_ModuleComboIndex     = 0;

			std::wstring m_PendingModuleName;
			std::wstring m_ScreenshotDir;
			std::wstring m_StatsOutPath;
			std::string  m_PresetStatus;

			int      m_ResizeStressIterations = 0;
			int      m_BatchWaitFrames        = 0;
			int      m_FrameCounter           = 0;
			int      m_ExitCode               = 0;
			bool     m_StatsWritten           = false;
			bool     m_CaptureRequested       = false;
			bool     m_ShowDemoWindow         = false;
			int      m_CaptureCounter         = 0;

			bool     m_SwitchTestMode   = false;
			uint32_t m_SwitchTestTarget = 0;
			uint32_t m_SwitchTestCount  = 0;
			int      m_SwitchTestFrame  = 0;

			float    m_CpuFrameMs        = 0.0f;
			float    m_GpuFrameMs        = 0.0f;
			uint32_t m_LastHeapBindCount = 0;

			static inline bool s_PendingHotReload         = false;
			static inline bool s_PendingHotReloadForceAll = false;
		};
	}

	const ModelEntry* GetModelCatalog(uint32_t& outCount)
	{
		outCount = static_cast<uint32_t>(sizeof(kCatalog) / sizeof(kCatalog[0]));
		return kCatalog;
	}

	std::string NarrowAscii(const std::wstring& text)
	{
		std::string result;
		result.reserve(text.size());
		for (wchar_t ch : text)
			result.push_back((ch < 0x80) ? static_cast<char>(ch) : '?');
		return result;
	}

	void ClearAllModels(HostContext& ctx)
	{
		if (ctx.SceneModels == nullptr || ctx.SceneModels->empty())
			return;

		Graphics::g_CommandManager.IdleGPU();
		ctx.SceneModels->clear();
		if (ctx.SelectedModel)     *ctx.SelectedModel = -1;
		if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = -1;
		LOG_INFO(L"[Scene] 已清空所有模型\n");
	}

	void AddModel(HostContext& ctx, uint32_t catalogIndex, bool forceRebuild)
	{
		uint32_t count = 0;
		const ModelEntry* catalog = GetModelCatalog(count);
		if (ctx.SceneModels == nullptr || catalogIndex >= count)
			return;

		const std::wstring path = Asset::Resolve(catalog[catalogIndex].relativePath);
		std::shared_ptr<Scene::Model::Model> source = Scene::Model::LoadModel(path, forceRebuild);
		if (source == nullptr)
		{
			LOG_INFO(L"[Scene] 模型加载失败：%ws\n", path.c_str());
			return;
		}

		const float radius = source->m_BoundingSphere.GetRadius();
		const float x = static_cast<float>(ctx.SceneModels->size()) * radius * 2.5f;

		ctx.SceneModels->emplace_back();
		SceneModel& entry = ctx.SceneModels->back();
		entry.catalogIndex = catalogIndex;
		entry.catalogName  = catalog[catalogIndex].name;
		entry.targetRadius = radius;
		entry.instance     = std::make_unique<Scene::Model::ModelInstance>(source);
		entry.instance->SetWorldPosition(Math::Vector3(x, 0.0f, 0.0f));

		if (ctx.SelectedModel)     *ctx.SelectedModel = static_cast<int32_t>(ctx.SceneModels->size()) - 1;
		if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = static_cast<int32_t>(catalogIndex);

		LOG_INFO(L"[Scene] 已添加模型：%hs（第 %d 个，半径 %.3f）\n",
			catalog[catalogIndex].name, static_cast<int>(ctx.SceneModels->size()), radius);
	}

	void RemoveModel(HostContext& ctx, int32_t sceneIndex)
	{
		if (ctx.SceneModels == nullptr || sceneIndex < 0 || sceneIndex >= static_cast<int32_t>(ctx.SceneModels->size()))
			return;

		Graphics::g_CommandManager.IdleGPU();
		const std::string name = (*ctx.SceneModels)[static_cast<size_t>(sceneIndex)].catalogName;
		ctx.SceneModels->erase(ctx.SceneModels->begin() + sceneIndex);

		if (ctx.SelectedModel && *ctx.SelectedModel >= static_cast<int32_t>(ctx.SceneModels->size()))
			*ctx.SelectedModel = static_cast<int32_t>(ctx.SceneModels->size()) - 1;

		LOG_INFO(L"[Scene] 已删除模型：%hs（剩余 %d 个）\n", name.c_str(), static_cast<int>(ctx.SceneModels->size()));
	}

	bool LoadModelByIndex(HostContext& ctx, uint32_t index, bool forceRebuild)
	{
		uint32_t count = 0;
		GetModelCatalog(count);
		if (index >= count)
			return false;

		ClearAllModels(ctx);
		AddModel(ctx, index, forceRebuild);

		if (ctx.SceneModels == nullptr || ctx.SceneModels->empty())
		{
			if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = -1;
			return false;
		}

		if (ctx.TargetRadius)    *ctx.TargetRadius = ctx.SceneModels->back().targetRadius;
		if (ctx.ModelComboIndex) *ctx.ModelComboIndex = static_cast<int>(index);
		return true;
	}

	void SetupStandardRightHandedCamera(HostContext& ctx, bool resetPose)
	{
		if (ctx.Camera == nullptr || ctx.CameraController == nullptr)
			return;

		const float aspect = (Graphics::g_DisplayWidth > 0)
			? (static_cast<float>(Graphics::g_DisplayHeight) / static_cast<float>(Graphics::g_DisplayWidth))
			: (9.0f / 16.0f);

		ctx.Camera->SetLeftHanded(false);
		ctx.Camera->ReverseZ(false);
		if (resetPose || ctx.CameraController->get() == nullptr)
		{
			ctx.Camera->SetEyeAtUp(
				Math::Vector3(0.0f, 0.0f, 5.0f),
				Math::Vector3(0.0f, 0.0f, -5.0f),
				Math::Vector3(Math::kYUnitVector));
			ctx.Camera->SetZRange(0.1f, 1000.0f);
		}
		ctx.Camera->SetAspectRatio(aspect);
		ctx.Camera->Update();

		if (resetPose || dynamic_cast<Scene::Camera::FlyingFPSCamera*>(ctx.CameraController->get()) == nullptr)
		{
			ctx.CameraController->reset(new Scene::Camera::FlyingFPSCamera(*ctx.Camera, Math::Vector3(Math::kYUnitVector)));
		}
	}

	void DrawSceneModelPanel(HostContext& ctx)
	{
		if (ctx.SceneModels == nullptr || ctx.ModelComboIndex == nullptr ||
			ctx.ForceRebuildOnLoad == nullptr || ctx.PendingAddModelIndex == nullptr ||
			ctx.PendingRemoveModel == nullptr || ctx.SelectedModel == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(16, 232), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(360, 380), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Scene / Model"))
		{
			ImGui::End();
			return;
		}

		uint32_t modelCount = 0;
		const ModelEntry* catalog = GetModelCatalog(modelCount);
		std::vector<const char*> names;
		names.reserve(modelCount);
		for (uint32_t i = 0; i < modelCount; ++i)
			names.push_back(catalog[i].name);

		ImGui::Combo("Catalog", ctx.ModelComboIndex, names.data(), static_cast<int>(names.size()));
		ImGui::Checkbox("Force rebuild (.ty)", ctx.ForceRebuildOnLoad);

		if (ImGui::Button("Add to scene"))
			*ctx.PendingAddModelIndex = *ctx.ModelComboIndex;
		ImGui::SameLine();
		ImGui::TextDisabled("(adds at frame start)");

		ImGui::Separator();
		ImGui::Text("Scene models: %d", static_cast<int>(ctx.SceneModels->size()));

		if (ctx.SceneModels->empty())
		{
			ImGui::TextDisabled("(empty - use Add to scene)");
		}
		else
		{
			for (int i = 0; i < static_cast<int>(ctx.SceneModels->size()); ++i)
			{
				const SceneModel& item = (*ctx.SceneModels)[static_cast<size_t>(i)];
				char label[128] = {};
				snprintf(label, sizeof(label), "%d: %s##sm%d", i, item.catalogName.c_str(), i);
				if (ImGui::Selectable(label, *ctx.SelectedModel == i))
					*ctx.SelectedModel = i;
			}

			ImGui::BeginDisabled(*ctx.SelectedModel < 0 || *ctx.SelectedModel >= static_cast<int>(ctx.SceneModels->size()));
			if (ImGui::Button("Remove selected"))
				*ctx.PendingRemoveModel = *ctx.SelectedModel;
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Clear all"))
				*ctx.PendingRemoveModel = -2;
		}

		ImGui::Separator();
		if (*ctx.SelectedModel >= 0 && *ctx.SelectedModel < static_cast<int>(ctx.SceneModels->size()))
		{
			SceneModel& item = (*ctx.SceneModels)[static_cast<size_t>(*ctx.SelectedModel)];
			ImGui::Text("Selected: %s", item.catalogName.c_str());
			ImGui::Text("Bounding radius: %.3f", item.instance ? static_cast<float>(item.instance->GetRadius()) : 0.0f);

			Math::Vector3 pos = item.instance ? item.instance->GetWorldPosition() : Math::Vector3(Math::kOrigin);
			float xyz[3] = { static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()) };
			if (ImGui::DragFloat3("Position", xyz, 0.02f, -100.0f, 100.0f))
			{
				if (item.instance)
					item.instance->SetWorldPosition(Math::Vector3(xyz[0], xyz[1], xyz[2]));
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset pos") && item.instance)
				item.instance->SetWorldPosition(Math::Vector3(Math::kOrigin));

			if (ImGui::DragFloat("Target radius", &item.targetRadius, 0.01f, 0.05f, 50.0f))
			{
				if (item.instance)
					item.instance->Resize(item.targetRadius);
			}
		}
		else
		{
			ImGui::TextDisabled("Select a model above to edit position/scale.");
		}

		ImGui::End();
	}

	int RunWorkstationApp(
		const wchar_t* appTitle,
		std::vector<std::unique_ptr<IDemoModule>> modules,
		HINSTANCE hInst,
		int nCmdShow)
	{
		WorkstationApp app(std::move(modules));
		return GameCore::RunApplication(app, appTitle, hInst, nCmdShow);
	}
}
