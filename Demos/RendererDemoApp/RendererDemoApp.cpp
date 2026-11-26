// ============================================================
// RendererDemoApp —— R3 统一入口
// ------------------------------------------------------------
// 它和三个旧 Demo 的区别只有一句话：
//   **Renderer 不再是一个写死的全局对象，而是通过 RendererRegistry
//     在运行时创建出来的、可以整体替换的实例。**
//
// 为什么必须是"实例"而不是全局对象：
//   切换 Renderer 意味着「销毁旧的一整套 GPU 资源 → 创建新的一整套」。
//   全局对象做不到"销毁后再创建"（它的生命周期就是进程），
//   只能做成 unique_ptr 才能在同一个进程里反复 Initialize/Shutdown。
//
// 切换顺序（路线图 §6.1，一步都不能省）：
//   1. IdleGPU                 —— 旧的 GPU 工作必须真正做完，否则下一步就
//                                 在资源还在被命令列表引用时销毁它；
//   2. 断开输出绑定            —— Display::SceneColorBuffer 是 shared_ptr，
//                                 它撑着旧 Renderer 的 SceneColor。先清空，w
//                                 否则 Shutdown 之后它仍指向已析构的对象；
//   3. 释放模型                —— Model 内部持有 TextureRef，而 Renderer::Shutdown
//                                 会调 TextureManager::Shutdown() 清空纹理缓存，
//                                 TextureRef 的 m_ref 会变成悬垂指针；
//   4. 旧 Renderer Shutdown + reset；
//   5. 经 RendererRegistry 创建并 Initialize 新的；
//   6. 重新绑定输出 + 重设相机宽高比；
//   7. 重新加载模型（纹理缓存是空的，必须重新走一遍）。
//
// 为什么 UI 里的"切换"只是登记一个请求：
//   ImGui 的绘制发生在 RenderUI，此时 GraphicsContext 是开着的。
//   而 Shutdown/Initialize 内部会提交自己的命令列表（LightingSystem 的
//   InitializeBuffer 甚至会 Finish(true) 强等 GPU）。在一个已经打开的
//   命令列表里再开一个上下文会打乱录制。所以 UI 只写 m_Pending*，
//   真正的动作在下一帧 Update() 的最开头、任何 GraphicsContext 之前执行。
//
// 验收（路线图 §6.3）：
//   - 同一入口切 ForwardRenderer / DeferredRenderer；
//   - 切模型无残留、无崩溃、无持续泄漏（描述符 live 数可在 Performance 面板看）；
//   - FPS / CPU 帧时间 / 真实 GPU 时间 / Debug View 可用；
//   - 拖动已接通的参数（曝光、光照强度、IBL）画面可预测变化。
// ============================================================

#include "00_Core/SystemTime.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include "01_Application/GameCore.h"
#include "01_Application/Display.h"
#include "01_Application/GameInput.h"

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"

#include "04_Renderer/DebugCapture.h"
#include "04_Renderer/GpuTimer.h"
#include "04_Renderer/ImGuiLayer.h"
#include "04_Renderer/RenderConfig.h"
#include "04_Renderer/RenderParams.h"
#include <fstream>
#include <sstream>
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/PrimarySurfaceDesc.h"
#include "04_Renderer/RendererHost.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/Base/RendererRegistry.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "05_Scene/Model/Model.h"
#include "05_Scene/Model/ModelLoader.h"

#include "Demos/Common/DemoSceneHelpers.h"
#include "Demos/Common/IDemoModule.h"
#include "Demos/DeferredRenderDemo/DeferredRenderDemoModule.h"
#include "Demos/ForwardRenderDemo/ForwardRenderDemoModule.h"
#include "Demos/GFL2RenderDemo/GFL2RenderDemoModule.h"
#include "Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h"

#include "imgui.h"

#include <string>
#include <vector>

using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;
// RendererFeature / RenderFrameDesc / RendererCreateDesc / DrawPass 等都在
// namespace Renderer 里（见 Engine/04_Renderer/Renderer/Base/BaseRenderer.h:43）。
// 旧 Demo 全用 Renderer::Xxx 前缀，这里直接引入命名空间，避免面板代码被前缀淹没。
using namespace Renderer;

namespace
{
	// ImGui 默认字体只有 ASCII（ImGuiLayer 明确不引入中文字体），
	// 所以面板上的**显示文本必须是英文**，中文只写在源码注释里。
	// 这里把宽字符串转成窄字符串用于 UI 显示，非 ASCII 字符一律替换成 '?'，
	// 避免把未映射的 wchar_t 直接塞进 UTF-8 文本里变成乱码。
	std::string Narrow(const std::wstring& text)
	{
		std::string result;
		result.reserve(text.size());
		for (wchar_t ch : text)
			result.push_back(ch < 0x80 ? (char)ch : '?');
		return result;
	}

	const char* FeatureName(RendererFeature feature)
	{
		switch (feature)
		{
		case RendererFeature::Forward:     return "Forward";
		case RendererFeature::Deferred:    return "Deferred";
		case RendererFeature::Shadow:      return "Shadow";
		case RendererFeature::SSAO:        return "SSAO";
		case RendererFeature::TAA:         return "TAA";
		case RendererFeature::Transparent: return "Transparent";
		case RendererFeature::PostProcess: return "PostProcess";
		case RendererFeature::Raytracing:  return "Raytracing";
		default:                           return "None";
		}
	}

	// 能力位图转成一行可读文本。注意：这里显示的是 Renderer 自己声明的能力，
	// 不是"已验证实现"的能力 —— Deferred 声明了 Shadow/SSAO/Transparent/PostProcess，
	// 但这四段目前是空壳（只有 PIX 事件，没有 GPU 命令），面板上如实标注。
	std::string FeaturesToString(RendererFeature mask)
	{
		std::string text;
		const RendererFeature all[] = {
			RendererFeature::Forward, RendererFeature::Deferred, RendererFeature::Shadow,
			RendererFeature::SSAO, RendererFeature::TAA, RendererFeature::Transparent,
			RendererFeature::PostProcess, RendererFeature::Raytracing,
		};
		for (RendererFeature feature : all)
		{
			if (!HasFeature(mask, feature))
				continue;
			if (!text.empty())
				text += " | ";
			text += FeatureName(feature);
		}
		return text.empty() ? "None" : text;
	}

	// 截图输出目录。没有 -Screenshot 参数时返回空串（= 不截图）。
	std::wstring ScreenshotDirectory(void)
	{
		std::wstring dir;
		if (!CommandLineArgs::GetString(L"Screenshot", dir) || dir.empty())
			return std::wstring();
		return dir;
	}
}


class RendererDemoApp : public GameCore::IGameApp
{
public:
	RendererDemoApp(void) = default;

	virtual void Startup(void) override;
	virtual void Cleanup(void) override;

	virtual void Update(float deltaT) override;
	virtual void RenderScene(void) override;

	// R3：ImGui 面板。GameCore 保证调用点在 RenderScene 之后、Display::Present 之前，
	// 目标就是 Display::SceneColorBuffer（已经处于 RENDER_TARGET）。
	// 因此 UI 会被随后 PreparePresentSDR 的 CopyResource 一起拷到 backbuffer 上，
	// 不需要改动 Display 的呈现路径（路线图 §6.2 的方案 A）。
	virtual void RenderUI(GraphicsContext& context) override;
	virtual bool HasRenderUI(void) const override { return true; }

	virtual void OnResize(uint32_t width, uint32_t height) override;

private:
	// ------------------------------------------------------------
	// 模型目录：UI 里能选到的每一项都必须是**磁盘上真实存在**的资产。
	// 路线图 §6.1 明确要求"UI 模型列表必须来源于实际可加载资产"，
	// 所以这里不做通配扫描（Assets/Model 下还有只有 .pmx 的目录，加载器不支持），
	// 而是显式列出已经验证过的条目。
	// ------------------------------------------------------------
	// ---- Renderer 生命周期（主控管理器仅调度 IDemoModule，不实现具体管线） ----
	bool SwitchRenderer(const std::wstring& name, bool simulateFailure = false);
	DemoCommon::IDemoModule* FindModule(const std::wstring& nameOrPipeline) const;
	DemoCommon::DemoHostContext BuildDemoContext(void);
	void BindRendererOutput(void);
	void UnloadModel(void);
	bool LoadModelByIndex(uint32_t index, bool forceRebuild);

	// R4.6：多模型操作（全部在帧边界调用）
	void   AddModel(uint32_t catalogIndex, bool forceRebuild = false);
	void   RemoveModel(int32_t sceneIndex);
	void   ClearAllModels(void);
	void ApplyPendingActions(void);

	// [WP-0B] 自动化管线反复热切换压测（-SwitchTest <N>）：
	// 每隔数帧在 Deferred / Forward / RayTracing 间轮换 N 次，Debug 下同步执行 _CrtCheckMemory()
	// 校验 CRT 堆完整性（验证 TD-03a / TD-05d 修复后反复切换不崩、无堆损坏）。
	void RunSwitchTest(void);
	// [WP-1B / TD-08a] 模拟新 Renderer Initialize() 返回 false 时的保底回归验证（-SwitchFailTest <dir>）
	void RunSwitchFailTest(void);
	// [WP-1C / TD-09c] DXR 删除全部模型后清屏验收（-EmptyDxrTest <dir>）
	void RunEmptyDxrTest(void);
	// [WP-3B / 契约 7.2] 自动化 F5 热重载验收（语法错回滚 + 实时画面变化 + >=5 次重载零泄漏）
	void RunHotReloadTest(void);
	void ExportStatsIfRequested(void);
	bool SavePresetToFile(const std::wstring& path);
	bool LoadPresetFromFile(const std::wstring& path, bool switchPipeline);

	// ---- R3 脚本化验收（-R3Test <dir>）----
	// 依次做「切模型 / 改参数 / 切 Renderer / 切回」并在每步之后做一次 GPU 回读。
	// 全部走与手动点按钮完全相同的代码路径（不是另写一套测试逻辑），
	// 这样验收脚本本身不会掩盖真实交互路径上的 bug。
	void RunR3Test(void);

	// ---- R4 脚本化验收（-R4Test <dir>）----
	// 与 R3Test 同一套约定：每步都走真实交互路径，截图全部走 GPU 回读。
	// 覆盖 R4 的四条改动：显示编码链（含 Skybox）、sRGB 编码、Prefilter mip 上界、AlphaTest。
	// 刻意包含 A/B 步（同一参数只翻一个开关），这样"这条链真的在起作用"
	// 可以被 md5 与像素统计证明，而不是靠肉眼判断。
	void RunR4Test(void);
	bool SaveCaptureTo(const std::wstring& fullPath);

	// ---- R6 脚本化验收（-R6Test <dir>）----
	// R6/D3 的验收对象是「光追算法是否真的在起作用」。光追的失败模式很隐蔽：
	// 阴影射线打空、偏差过大、SBT miss 表错位，都会得到一张"看起来像有光"的图。
	// 因此每一步都是**单变量 A/B**，靠像素统计证明开关真的改变了结果：
	//   阴影 开/关 -> 同一帧光照下，模型自遮挡区域必须变亮
	//   反射 开/关 -> 同一帧下，掠射角区域必须出现天空色
	//   调试通道   -> 直接把 visibility / 反射色 / 命中距离画出来
	void RunR6Test(void);

	// ---- R7 脚本化验收（-R7Test <dir>）----
	// R7/D4 的验收对象是「贴图颜色到底有没有进着色」以及「GGX 有没有真的替换
	// 掉 Lambert」。这两件事在 P0 里是**同时**引入的，因此单看一张图无法归因：
	//   贴图接错（UV 偏移错/描述符表越界/采样器错）与 BRDF 换掉，都会让画面变化。
	// 所以这里用 2×2 矩阵把两个变量解耦：
	//   A5a 贴图关 + GGX 关 -> 必须逐字节等于 R6 基线 md5 a43b6059…（可自我证伪）
	//   A5b 贴图关 + GGX 开 -> 只有高光变化，颜色仍是中性灰
	//   A5c 贴图开 + GGX 关 -> 有颜色但仍是 Lambert
	//   A5d 贴图开 + GGX 开 -> P0 的最终画面
	// 另外单独取证 BaseColor 调试通道（与 DeferredRenderer 的同一通道逐像素比对）。
	void RunR7Test(void);
	// [WP-3C] 自动化 Shadow(Simple/CSM) + SSAO + TAA A/B 验收（-WP3CTest <dir>）
	void RunWp3cTest(void);
	// [契约 4] 统一主表面契约跨管线自动化验收（-PrimarySurfaceTest <dir>）
	void RunPrimarySurfaceTest(void);

	// ---- 每帧（管理器只绘制 Performance / Demo Manager / DevTools，其余委托活跃模块） ----
	void DrawPanels(void);
	void DrawPerformancePanel(void);
	void DrawRendererPanel(void);
	void DrawDevToolsPanel(void);
	bool SaveCapture(const std::wstring& directory, const wchar_t* prefix, int index);

	static bool WndProcThunk(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam);

private:
	// [WP-2B / 契约 2] 统一渲染宿主：托管 RenderServices 与活跃 IPipeline，内置先备后切协议，
	// 消除 Demo 侧 3 路 dynamic_cast（TD-01d）。
	Renderer::RendererHost m_Host;
	Renderer::IPipeline*   m_Renderer = nullptr; // 指向 m_Host.GetActivePipeline() 的非拥有视图
	std::wstring           m_RendererName;
	std::vector<std::unique_ptr<DemoCommon::IDemoModule>> m_Modules;
	DemoCommon::IDemoModule* m_ActiveModule = nullptr;
	std::vector<std::string> m_RendererNames;   // Demo 模块名称列表（ASCII）

	// RenderParams：唯一 CPU 真源。面板只写这里，每帧作为指针交给 Renderer，
	// 由 LightingSystem::ApplyParams 做"内容相同就不重传"的去重。
	Renderer::RenderParams m_Params;

	std::vector<DemoCommon::SceneModel> m_SceneModels;
	int32_t  m_SelectedModel = -1;       // UI 里选中的那一项
	int32_t  m_CurrentModelIndex = -1;   // 兼容 R3Test 的"最近加载的 catalog 索引"
	float    m_TargetRadius = 1.0f;
	bool     m_ForceRebuildOnLoad = false;

	// UI → 帧边界的待执行动作（增删模型必须延迟到安全点执行）
	int32_t  m_PendingAddModelIndex = -1;
	int32_t  m_PendingRemoveModel   = -1;   // -1=无动作，-2=清空全部，>=0=删除该索引

	// 相机
	Scene::Camera::Camera m_Camera;
	unique_ptr<Scene::Camera::CameraController> m_CameraController;

	// GPU 计时（真实 GPU 时间，和 CPU 帧时间严格分开显示）
	Renderer::GpuTimer m_GpuTimer;
	float   m_GpuFrameMs = 0.0f;
	float   m_CpuFrameMs = 0.0f;

	// UI → 帧边界动作的登记位
	std::wstring m_PendingRendererName;
	int32_t      m_PendingModelIndex = -1;
	bool         m_CaptureRequested = false;
	bool         m_ShowDemoWindow = false;
	int          m_CaptureCounter = 0;

	// R3 脚本化验收状态
	bool         m_R3TestMode = false;
	std::wstring m_R3TestDir;
	int          m_R3TestStep = 0;
	int          m_R3TestFrame = 0;

	bool         m_R4TestMode = false;
	std::wstring m_R4TestDir;
	int          m_R4TestStep = 0;
	int          m_R4TestFrame = 0;

	bool         m_R6TestMode = false;
	std::wstring m_R6TestDir;
	int          m_R6TestStep = 0;
	int          m_R6TestFrame = 0;

	bool         m_R7TestMode = false;
	std::wstring m_R7TestDir;
	int          m_R7TestStep = 0;
	int          m_R7TestFrame = 0;
	std::wstring m_R3NextCapture;      // 下一步动作执行后要截的图
	std::wstring m_PendingCapturePath; // 本帧 RenderScene 之后要截的图（完整路径）

	// 「包含 ImGui 画面的截图」不能在 RenderUI 里做：此时 GraphicsContext 由
	// GameCore 持有，DebugCapture 内部要自己开一个上下文，嵌套会破坏命令列表。
	// 所以 RenderUI 只登记，等下一帧 Update 开头（SceneColor 里还留着上一帧
	// 含 UI 的完整画面、且新一帧还没清屏）再回读。
	std::wstring m_PendingUICapturePath;

	// [WP-3A] 模拟无 DXR 与启动失败非零退出码支持
	bool         m_StrictStartRenderer = false;
	int          m_FatalExitCode = 0;
	bool         m_LoggedWp3aTelemetry = false;

	// [WP-3B] 分域 RenderConfig / -Preset / -StatsOut / -Experiment / F5 热重载
	Renderer::RenderConfig m_Config;
	std::wstring           m_StatsOutPath;
	bool                   m_StatsWritten = false;
	bool                   m_ExperimentMode = false;
	std::string            m_PresetStatus;
	static bool            s_PendingHotReload;
	static bool            s_PendingHotReloadForceAll;

	// [WP-3B] -HotReloadTest <dir> 自动化 F5 实测状态
	bool                   m_HotReloadTestMode = false;
	std::wstring           m_HotReloadTestDir;
	int                    m_HotReloadTestFrame = 0;
	std::wstring           m_HotReloadTargetFile;
	std::string            m_HotReloadBackupBytes;
	bool                   m_HotReloadBackupValid = false;
	uint32_t               m_HotReloadSuccessCount = 0;

	// [WP-0B] -SwitchTest <N> 反复切换压测状态
	bool         m_SwitchTestMode = false;
	uint32_t     m_SwitchTestTarget = 0;
	uint32_t     m_SwitchTestCount = 0;
	int          m_SwitchTestFrame = 0;

	// [WP-1B] -SwitchFailTest <dir> 失败保底验收状态与 SetDescriptorHeaps 帧内计数
	bool         m_SwitchFailTestMode = false;
	std::wstring m_SwitchFailTestDir;
	int          m_SwitchFailTestFrame = 0;
	bool         m_SwitchFailTriggered = false;
	uint32_t     m_LastRenderSceneHeapBindCount = 0;
	std::wstring m_LoggedHeapBindForRenderer;

	// [WP-1C / TD-09c] -EmptyDxrTest <dir> DXR 空场景清屏验收状态
	bool         m_EmptyDxrTestMode = false;
	std::wstring m_EmptyDxrTestDir;
	int          m_EmptyDxrTestFrame = 0;

	// [WP-3C] -WP3CTest <dir> 延迟管线 Shadow/SSAO/TAA 自动化 A/B 验收状态
	bool         m_Wp3cTestMode = false;
	std::wstring m_Wp3cTestDir;
	int          m_Wp3cTestStep = 0;
	int          m_Wp3cTestFrame = 0;

	// [契约 4] -PrimarySurfaceTest <dir> 统一表面契约跨管线验收状态
	bool         m_PrimarySurfaceTestMode = false;
	std::wstring m_PrimarySurfaceTestDir;
	int          m_PrimarySurfaceTestStep = 0;
	int          m_PrimarySurfaceTestFrame = 0;

	// 面板临时状态（[TD-10b] 移除冗余 m_DebugViewIndex 副本，UI 直接绑定 m_Params.Debug 唯一真源）
	int m_RendererComboIndex = 0;
	int m_ModelComboIndex = 0;
};


// ============================================================
// Win32 输入转发
// ------------------------------------------------------------
// GameCore 的 WndProc 会把**所有**消息无条件转发到这里，并且丢弃返回值
// （imgui_impl_win32 的 handler 一律 return 0，返回值不能用来判断"是否消费"）。
// 所以真正决定"要不要把输入交给相机"的是 io.WantCaptureMouse/Keyboard。
// ============================================================
bool RendererDemoApp::s_PendingHotReload = false;
bool RendererDemoApp::s_PendingHotReloadForceAll = false;

bool RendererDemoApp::WndProcThunk(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam)
{
	// [WP-3B / 契约 7.2] 捕获 F5（增量热重载）与 Shift+F5（强制全量重编），延迟至帧边界单线程执行
	if (msg == WM_KEYDOWN && wParam == VK_F5)
	{
		s_PendingHotReload = true;
		s_PendingHotReloadForceAll = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);
	}
	return Renderer::ImGuiLayer::WndProcHandler(hwnd, msg, wParam, lParam);
}


// ============================================================
// Renderer 切换（WP-1B / TD-01b / TD-08a：先建新后替换 + 零模型重载）
// ============================================================
DemoCommon::DemoHostContext RendererDemoApp::BuildDemoContext(void)
{
	DemoCommon::DemoHostContext ctx;
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
	ctx.FreezeCameraForScreenshot = !ScreenshotDirectory().empty();
	ctx.WantCaptureKeyboard  = Renderer::ImGuiLayer::IsInitialized() && Renderer::ImGuiLayer::WantCaptureKeyboard();
	ctx.CpuFrameMs           = m_CpuFrameMs;
	ctx.GpuFrameMs           = m_GpuFrameMs;
	ctx.LastHeapBindCount    = m_LastRenderSceneHeapBindCount;
	return ctx;
}

DemoCommon::IDemoModule* RendererDemoApp::FindModule(const std::wstring& nameOrPipeline) const
{
	const std::string narrowQuery = Narrow(nameOrPipeline);
	for (const auto& mod : m_Modules)
	{
		if (mod->GetPipelineName() == nameOrPipeline || narrowQuery == mod->GetDisplayName())
			return mod.get();
	}
	return nullptr;
}

bool RendererDemoApp::SwitchRenderer(const std::wstring& name, bool simulateFailure)
{
	DemoCommon::IDemoModule* targetMod = FindModule(name);
	const std::wstring pipelineName = (targetMod != nullptr) ? targetMod->GetPipelineName() : name;
	const bool isFirstActivation = (m_ActiveModule == nullptr);

	if (!m_Host.SwitchPipeline(pipelineName, g_DisplayWidth, g_DisplayHeight, simulateFailure))
		return false;

	DemoCommon::DemoHostContext ctx = BuildDemoContext();
	if (m_ActiveModule != nullptr && m_ActiveModule != targetMod)
		m_ActiveModule->OnDeactivate(ctx);

	m_ActiveModule = targetMod;
	m_Renderer     = m_Host.GetActivePipeline();
	m_RendererName = m_Host.GetActivePipelineName();
	m_LoggedHeapBindForRenderer.clear();

	for (size_t i = 0; i < m_Modules.size(); ++i)
	{
		if (m_Modules[i].get() == m_ActiveModule)
		{
			m_RendererComboIndex = static_cast<int>(i);
			break;
		}
	}

	BindRendererOutput();
	if (m_ActiveModule != nullptr)
		m_ActiveModule->OnActivate(ctx, isFirstActivation);

	LOG_INFO(L"[Manager] 切换完成：%hs (%ws)（输出 %ws，场景保留模型数=%zu）\n",
		m_ActiveModule ? m_ActiveModule->GetDisplayName() : "<Unknown>",
		m_RendererName.c_str(),
		Display::SceneColorBuffer ? L"已重绑" : L"为空",
		m_SceneModels.size());
	return true;
}

void RendererDemoApp::BindRendererOutput(void)
{
	Display::SceneColorBuffer = m_Host.GetOutputColorBuffer();
}

void RendererDemoApp::UnloadModel(void)
{
	ClearAllModels();
}

void RendererDemoApp::ClearAllModels(void)
{
	DemoCommon::DemoHostContext ctx = BuildDemoContext();
	DemoCommon::ClearAllModels(ctx);
}

void RendererDemoApp::AddModel(uint32_t catalogIndex, bool forceRebuild)
{
	DemoCommon::DemoHostContext ctx = BuildDemoContext();
	DemoCommon::AddModel(ctx, catalogIndex, forceRebuild);
}

void RendererDemoApp::RemoveModel(int32_t sceneIndex)
{
	DemoCommon::DemoHostContext ctx = BuildDemoContext();
	DemoCommon::RemoveModel(ctx, sceneIndex);
}

bool RendererDemoApp::LoadModelByIndex(uint32_t index, bool forceRebuild)
{
	DemoCommon::DemoHostContext ctx = BuildDemoContext();
	return DemoCommon::LoadModelByIndex(ctx, index, forceRebuild);
}


// ============================================================
// 帧边界动作
// ------------------------------------------------------------
// 必须在这里、且只在任何 GraphicsContext::Begin 之前执行 —— 见文件头注释。
// ============================================================
void RendererDemoApp::ApplyPendingActions(void)
{
	if (!m_PendingRendererName.empty())
	{
		const std::wstring name = m_PendingRendererName;
		m_PendingRendererName.clear();
		SwitchRenderer(name);
	}

	if (m_PendingModelIndex >= 0)
	{
		const uint32_t index = (uint32_t)m_PendingModelIndex;
		m_PendingModelIndex = -1;
		LoadModelByIndex(index, m_ForceRebuildOnLoad);
	}

	// R4.6：模型增删。必须在这里（帧边界）执行 —— 见 DrawScenePanel 的说明。
	if (m_PendingAddModelIndex >= 0)
	{
		const uint32_t index = (uint32_t)m_PendingAddModelIndex;
		m_PendingAddModelIndex = -1;
		AddModel(index);
	}

	if (m_PendingRemoveModel != -1)
	{
		const int32_t target = m_PendingRemoveModel;
		m_PendingRemoveModel = -1;

		if (target == -2)          // 约定：-2 = 清空全部
			ClearAllModels();
		else
			RemoveModel(target);
	}

	// [WP-3B / 契约 7.2] 帧边界单线程执行 F5 / Shift+F5 Shader 热重载（无后台线程、零竞态）
	if (s_PendingHotReload)
	{
		const bool forceAll = s_PendingHotReloadForceAll;
		s_PendingHotReload = false;
		s_PendingHotReloadForceAll = false;
		const auto rep = m_Host.TriggerShaderHotReload(forceAll);
		LOG_INFO(L"[HotReload] F5 trigger (forceAll=%d): %hs\n", forceAll ? 1 : 0, rep.Summary.c_str());
	}
}



// ============================================================
// R3 脚本化验收（-R3Test <dir>）
// ------------------------------------------------------------
// 每一步都调用与 UI 按钮**完全相同**的函数（SwitchRenderer / LoadModelByIndex /
// 直接写 m_Params），因此它验证的是真实交互路径，而不是一条平行实现。
// 每步之间隔 kFramesPerStep 帧：足够让 GPU 队列消化、让 GpuTimer 拿到结果。
//
// 截图全部走 DebugCapture 的 GPU 回读（SKILL.md H3：外部截图不算证据）。
// ============================================================
void RendererDemoApp::RunR3Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R3TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
		const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
		const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
		                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
		LOG_INFO(L"[R3-Test] %ws renderer=%ws model=%d RTV=%u SRV=%u pool=%u\n",
			tag, m_RendererName.c_str(), m_CurrentModelIndex, rtvLive, srvLive, heapPool);
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R3TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R3-Test] step %d -> %ws\n", m_R3TestStep, path);
	};

	switch (m_R3TestStep++)
	{
	case 0:
		LogState(L"[0] initial");
		RequestShot(L"r3_00_initial_deferred.png");
		break;

	case 1:
		// 太阳强度 3 -> 8：画面应明显变亮（LightingPS 的 DirectionalIntensity 直接乘 PostParams）
		m_Params.SunIntensity = 8.0f;
		LogState(L"[1] sun intensity 8");
		RequestShot(L"r3_01_sun8.png");
		break;

	case 2:
		// 关掉平行光：只剩 IBL + 点光，画面应该暗下来
		m_Params.EnableSunLight = false;
		LogState(L"[2] sun off");
		RequestShot(L"r3_02_sun_off.png");
		break;

	case 3:
		// 恢复光照，切 Debug View = BaseColor（应为纯材质基色，无光照）
		m_Params.EnableSunLight = true;
		m_Params.SunIntensity = 3.0f;
		m_Params.Debug = Renderer::DebugView::BaseColor;
		LogState(L"[3] debug basecolor");
		RequestShot(L"r3_03_basecolor.png");
		break;

	case 4:
		// Debug View = Depth（R1 起 GBuffer 深度编码依赖 InvRenderTargetSize，resize 后必须仍然正确）
		m_Params.Debug = Renderer::DebugView::Depth;
		LogState(L"[4] debug depth");
		RequestShot(L"r3_04_depth.png");
		break;

	case 5:
		// 回到 Final，曝光 1 -> 3，画面应整体变亮且高光更早进入 ACES 压缩
		m_Params.Debug = Renderer::DebugView::Final;
		m_Params.Exposure = 3.0f;
		LogState(L"[5] exposure 3");
		RequestShot(L"r3_05_exposure3.png");
		break;

	case 6:
		// 切模型（第 3 项 saber_emiya）：验证换模型无残留 —— 旧模型的 MeshConstants 要释放
		m_Params.Exposure = 1.0f;
		LoadModelByIndex(2, m_ForceRebuildOnLoad);
		LogState(L"[6] model saber_emiya");
		RequestShot(L"r3_06_model_saber.png");
		break;

	case 7:
		// 切 Renderer：Deferred -> Forward。这是 R3 的核心验收项。
		// 若上一步可选资产 saber_emiya 缺失导致场景变空，先补回模型 0 再切管线，
		// 保持与 Golden 基线一致的同时验证 SwitchRenderer 本身不重载已加载模型。
		if (m_SceneModels.empty())
			LoadModelByIndex(0, false);
		SwitchRenderer(L"ForwardRenderer");
		LogState(L"[7] after switch to Forward");
		RequestShot(L"r3_07_forward.png");
		break;

	case 8:
		// 切回 Deferred：验证「切回去」也能恢复（EnvLighting 的 m_Baked 复位就靠这一步暴露）
		SwitchRenderer(L"DeferredRenderer");
		LogState(L"[8] back to Deferred");
		RequestShot(L"r3_08_back_deferred.png");
		break;

	case 9:
		// ImGui 面板取证：RenderUI 在本帧稍后把六个面板画进 SceneColor，
		// 下一帧 Update 开头再把那张「含 UI 的完整画面」回读出来。
		// 这样做绕开了「不能在 GameCore 的 GraphicsContext 里嵌套开上下文」的限制。
		LogState(L"[9] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r3_09_imgui_panels.png", m_R3TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R3-Test] step %d -> %ws (with ImGui)\n", m_R3TestStep, uiPath);
		}
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R3-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}


// ============================================================
// R4 脚本化验收（-R4Test <dir>）
// ------------------------------------------------------------
// 设计原则与 R3Test 一致：每一步调用真实交互路径上的同一批函数/字段，
// 截图全部走 DebugCapture 的 GPU 回读（外部截图不算证据）。
//
// 与 R3Test 的关键差别：R4 涉及的是"整条链有没有接对"，
// 因此每一步都安排成**单变量 A/B**，让"改动生效"可以被证明：
//   sRGB 关/开  -> 同一曝光同一场景，只翻一个开关
//   Tonemap 关/开 -> 同一 HDR 输入，只翻 ACES
//   AlphaTest   -> 用 toneriko1（glTF alphaMode=MASK）验证镂空边缘
// md5 与像素统计由外部脚本汇总，日志里只记录参数与描述符计数。
// ============================================================
void RendererDemoApp::RunR4Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R4TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
		const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
		const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
		                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
		LOG_INFO(L"[R4-Test] %ws renderer=%ws model=%d exposure=%.2f tonemap=%d srgb=%d prefilterMip=%.1f uploads=%llu RTV=%u SRV=%u pool=%u\n",
			tag, m_RendererName.c_str(), m_CurrentModelIndex,
			m_Params.Exposure, m_Params.EnableTonemap ? 1 : 0, m_Params.EnableSRGBEncode ? 1 : 0,
			Scene::LightingSystem::GetPrefilterMaxMip(),
			(unsigned long long)Scene::LightingSystem::GetUploadCount(),
			rtvLive, srvLive, heapPool);
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R4TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R4-Test] step %d -> %ws\n", m_R4TestStep, path);
	};

	switch (m_R4TestStep++)
	{
	case 0:
		// 基线：所有 R4 改动生效后的默认画面（Exposure=1 ACES on sRGB on）
		LogState(L"[0] r4 baseline (full display chain)");
		RequestShot(L"r4_00_baseline.png");
		break;

	case 1:
		// A/B 1：关掉 sRGB 编码。若这一步与上一步 md5 相同，说明编码没接上。
		m_Params.EnableSRGBEncode = false;
		LogState(L"[1] srgb encode OFF");
		RequestShot(L"r4_01_srgb_off.png");
		break;

	case 2:
		// A/B 2：恢复 sRGB，关掉 ACES。验证色调映射确实作用在 HDR 上。
		m_Params.EnableSRGBEncode = true;
		m_Params.EnableTonemap = false;
		LogState(L"[2] tonemap OFF");
		RequestShot(L"r4_02_tonemap_off.png");
		break;

	case 3:
		// A/B 3：恢复 ACES，曝光 1 -> 0.5。
		m_Params.EnableTonemap = true;
		m_Params.Exposure = 0.5f;
		LogState(L"[3] exposure 0.5");
		RequestShot(L"r4_03_exposure_0p5.png");
		break;

	case 4:
		// Skybox 取证：曝光拉到 4。天空若仍参与曝光链，必须整体变亮/更早截断；
		// 若天空数值与第 0 步一致，说明 Skybox 又绕过了曝光。
		m_Params.Exposure = 4.0f;
		LogState(L"[4] exposure 4 (skybox must follow)");
		RequestShot(L"r4_04_exposure_4.png");
		break;

	case 5:
		// AlphaTest：切到 toneriko1（glTF alphaMode = MASK）。
		// R4 之前 GBufferPS 没有 clip，镂空边缘会被画成实心方块。
		m_Params.Exposure = 1.0f;
		LoadModelByIndex(6, m_ForceRebuildOnLoad);
		LogState(L"[5] alphaTest model toneriko1");
		RequestShot(L"r4_05_alphatest_model.png");
		break;

	case 6:
		// AlphaTest 的对照：同一模型切 Debug View = BaseColor。
		// 该视图直接输出 GBuffer 的 baseColor（clip 之后才写进去的），
		// 因此镂空区域在 BaseColor 视图里也必须是空的 —— 否则 clip 根本没执行。
		m_Params.Debug = Renderer::DebugView::BaseColor;
		LogState(L"[6] alphaTest basecolor view");
		RequestShot(L"r4_06_alphatest_basecolor.png");
		break;

	case 7:
		// IBL 取证：回到 Final，关掉太阳与点光，只留 IBL。
		// 这一帧直接反映 Prefilter/BRDFLUT 烘焙产物 + 新的 mip 上界。
		m_Params.Debug = Renderer::DebugView::Final;
		m_Params.EnableSunLight = false;
		m_Params.EnablePointLight = false;
		LogState(L"[7] IBL only");
		RequestShot(L"r4_07_ibl_only.png");
		break;

	case 8:
		// IBL 关断对照：同样的只留环境光场景，把 IBL 也关掉。
		// 若与上一步几乎相同，说明 IBL 通路没生效。
		m_Params.EnableIBL = false;
		LogState(L"[8] IBL OFF");
		RequestShot(L"r4_08_ibl_off.png");
		break;

	case 9:
		// 复原并检查描述符是否有泄漏（连续 10 步切换后 RTV/SRV/pool 应稳定）。
		m_Params.EnableIBL = true;
		m_Params.EnableSunLight = true;
		m_Params.EnablePointLight = true;
		LoadModelByIndex(0, m_ForceRebuildOnLoad);
		LogState(L"[9] restored");
		RequestShot(L"r4_09_restored.png");
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R4-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// R6 脚本化验收（-R6Test <dir>）
// ------------------------------------------------------------
// 与 R3Test / R4Test 同一套约定：每步都走真实交互路径上的同一批字段，
// 截图全部走 DebugCapture 的 GPU 回读。
//
// R6 的验收对象是"光追算法是否真的在起作用"。光追的失败模式比光栅隐蔽得多：
// 阴影射线打空、TMin 偏差过大、SBT miss 表记录错位，都能得到一张看起来
// 正常、只是"有点平"的图。因此这里每一步都是**单变量 A/B**，让开关的效果
// 必须能被像素统计证明，而不是靠肉眼判断"好像有点阴影"。
// ============================================================
void RendererDemoApp::RunR6Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R6TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		LOG_INFO(L"[R6-Test] %ws renderer=%ws shadow=%d reflection=%d normalBias=%.4f shadowDist=%.1f reflectDist=%.1f reflectIntensity=%.2f rtDebug=%d\n",
			tag,
			m_RendererName.c_str(),
			m_Params.EnableRTShadow ? 1 : 0,
			m_Params.EnableRTReflection ? 1 : 0,
			m_Params.RTShadowNormalBias,
			m_Params.RTShadowMaxDistance,
			m_Params.RTReflectionMaxDistance,
			m_Params.RTReflectionIntensity,
			(int)m_Params.RTDebugView);

		// [WP-2B / TD-01d] 通过 IPipeline::QueryRayTracingStats 无 RTTI 读取光追统计
		Renderer::RayTracingFrameStats stats = {};
		if (m_Renderer != nullptr && m_Renderer->QueryRayTracingStats(stats))
		{
			LOG_INFO(L"[R6-Test]   rays: primary=%llu shadow=%llu reflection=%llu total=%llu | geo: tri=%u blas=%u inst=%u\n",
				(unsigned long long)stats.PrimaryRays,
				(unsigned long long)stats.ShadowRays,
				(unsigned long long)stats.ReflectionRays,
				(unsigned long long)stats.TotalRays,
				stats.TriangleCount, stats.BlasCount, stats.InstanceCount);
		}
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R6TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R6-Test] step %d -> %ws\n", m_R6TestStep, path);
	};

	switch (m_R6TestStep++)
	{
	case 0:
		// 基线：DXR + 阴影开 + 反射关（RenderParams 的默认组合）。
		LogState(L"[0] rt baseline (shadow on, reflection off)");
		RequestShot(L"r6_00_baseline_shadow_on.png");
		break;

	case 1:
		// A/B 1：只关阴影。其余参数完全相同。
		// 若与上一步一样，说明阴影射线根本没参与着色（或偏差大到看不见接触阴影）。
		m_Params.EnableRTShadow = false;
		LogState(L"[1] rt shadow OFF");
		RequestShot(L"r6_01_shadow_off.png");
		break;

	case 2:
		// 调试通道：把 visibility 直接画成灰度。
		// 这是最硬的证据 —— 阴影射线如果全打空，这张图会是纯白。
		m_Params.EnableRTShadow = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shadow;
		LogState(L"[2] debug view = shadow visibility");
		RequestShot(L"r6_02_debug_shadow.png");
		break;

	case 3:
		// A/B 2：反射开。单独一步，与"只有阴影"的基线对照。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		LogState(L"[3] rt reflection ON");
		RequestShot(L"r6_03_reflection_on.png");
		break;

	case 4:
		// 反射调试通道：直接看反射射线的命中颜色。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Reflection;
		LogState(L"[4] debug view = reflection color");
		RequestShot(L"r6_04_debug_reflection.png");
		break;

	case 5:
		// 反射关断对照：同一调试通道下关掉反射，应回到背景色（没算反射）。
		m_Params.EnableRTReflection = false;
		LogState(L"[5] reflection debug with reflection OFF");
		RequestShot(L"r6_05_debug_reflection_off.png");
		break;

	case 6:
		// 反射强度 0/1 的极端值：强度=0 时画面必须与"反射关"完全一致。
		// 这是 lerp 系数是否真的接上了 `RTReflectionIntensity` 的证据。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		m_Params.RTReflectionIntensity = 0.0f;
		LogState(L"[6] reflection intensity 0");
		RequestShot(L"r6_06_reflect_intensity_0.png");
		break;

	case 7:
		// 反射强度 1.0：全反射，画面应明显偏向天空色。
		m_Params.RTReflectionIntensity = 1.0f;
		LogState(L"[7] reflection intensity 1");
		RequestShot(L"r6_07_reflect_intensity_1.png");
		break;

	case 8:
		// 命中距离调试通道：验证 TraceReflectionRay 的 RayTCurrent 真的回传了。
		m_Params.EnableRTReflection = true;
		m_Params.RTReflectionIntensity = 0.6f;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::HitDistance;
		LogState(L"[8] debug view = hit distance");
		RequestShot(L"r6_08_debug_hitdistance.png");
		break;

	case 9:
		// 偏差敏感性：偏差从 0.002 m 放大到 0.05 m。
		// 太大时会漏光（接触阴影消失），这是"偏差是两个量纲不同的东西"的证据。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shadow;
		m_Params.RTShadowNormalBias = 0.05f;
		LogState(L"[9] normal bias 0.05 m");
		RequestShot(L"r6_09_shadow_bias_large.png");
		break;

	case 10:
		// 复原并确认描述符没有泄漏（连续切换后 RTV/SRV/pool 应稳定）。
		m_Params.RTShadowNormalBias = 0.002f;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTShadow = true;
		m_Params.EnableRTReflection = false;
		LogState(L"[10] restored to defaults");
		RequestShot(L"r6_10_restored.png");
		break;

	case 11:
		// ImGui 面板取证（与 R3Test 第 9 步同一机制）：
		// RenderUI 在本帧稍后把面板画进 SceneColor，下一帧 Update 开头再把
		// 「含 UI 的完整画面」回读出来。绕开「不能在 GameCore 的
		// GraphicsContext 里嵌套开上下文」的限制。
		LogState(L"[11] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r6_11_imgui_panel.png", m_R6TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R6-Test] step %d -> %ws (with ImGui)\n", m_R6TestStep, uiPath);
		}
		break;

	case 12:
		LogState(L"[12] final");
		LOG_INFO(L"[R6-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// R7 脚本化验收（-R7Test <dir>）
// ------------------------------------------------------------
// 与 R3Test / R4Test / R6Test 同一套约定：每一步都直接写 m_Params 的同一批
// 字段（与 UI 勾选框完全同一条路径），截图全部走 DebugCapture 的 GPU 回读。
//
// 为什么必须做成 2×2 矩阵：
//   R7 P0 同时引入了「BaseColor 贴图」与「GGX/Cook-Torrance」两个变量。
//   只对比"R6 vs R7"的话，任何一个变量接错都会表现为"画面变了"，
//   无法判断是贴图没进来还是 BRDF 换了。两个正交开关把四象限拆开：
//     (贴图关, GGX关) = 必须逐字节等于 R6 基线 -> 证明 R7 没动到旧路径
//     (贴图关, GGX开) = 中性灰 + 高光    -> 证明 GGX 单独生效
//     (贴图开, GGX关) = 有颜色 + Lambert -> 证明贴图单独生效
//     (贴图开, GGX开) = 最终画面
//   其中第一象限是**可自我证伪**的：若它不等于 a43b6059…，说明 R7 改坏了
//   默认路径，此时后面三张图无论多好看都不算通过。
// ============================================================
void RendererDemoApp::RunR7Test(void)
{
	static const int kFramesPerStep = 10;
	if ((m_R7TestFrame++ % kFramesPerStep) != 0)
		return;

	auto LogState = [this](const wchar_t* tag)
	{
		LOG_INFO(L"[R7-Test] %ws renderer=%ws textures=%d ggx=%d rtDebug=%ws shadow=%d reflection=%d\n",
			tag,
			m_RendererName.c_str(),
			m_Params.EnableMaterialTextures ? 1 : 0,
			m_Params.EnableGGX ? 1 : 0,
			Renderer::RayTracingDebugViewName(m_Params.RTDebugView),
			m_Params.EnableRTShadow ? 1 : 0,
			m_Params.EnableRTReflection ? 1 : 0);

		Renderer::RayTracingFrameStats stats = {};
		if (m_Renderer != nullptr && m_Renderer->QueryRayTracingStats(stats))
		{
			LOG_INFO(L"[R7-Test]   rays: primary=%llu shadow=%llu reflection=%llu total=%llu | geo: tri=%u blas=%u inst=%u\n",
				(unsigned long long)stats.PrimaryRays,
				(unsigned long long)stats.ShadowRays,
				(unsigned long long)stats.ReflectionRays,
				(unsigned long long)stats.TotalRays,
				stats.TriangleCount, stats.BlasCount, stats.InstanceCount);
		}
	};

	auto RequestShot = [this](const wchar_t* fileName)
	{
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_R7TestDir.c_str(), fileName);
		m_PendingCapturePath = path;
		LOG_INFO(L"[R7-Test] step %d -> %ws\n", m_R7TestStep, path);
	};

	switch (m_R7TestStep++)
	{
	case 0:
		// A5d：最终画面（贴图开 + GGX 开）。这是用户在 UI 里默认看到的组合。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		LogState(L"[0] A5d textures ON + GGX ON (final look)");
		RequestShot(L"r7_00_textures_ggx.png");
		break;

	case 1:
		// A5a：两个都关。**必须逐字节等于 R6 基线 md5 a43b6059…**
		// 这一步是整个 R7 最关键的自我证伪点：它证明 R7 的改动在关闭时
		// 与 R6 的着色路径完全等价（没有被顺手改掉的隐含行为）。
		m_Params.EnableMaterialTextures = false;
		m_Params.EnableGGX = false;
		LogState(L"[1] A5a textures OFF + GGX OFF (must equal R6 baseline)");
		RequestShot(L"r7_01_A5a_both_off.png");
		break;

	case 2:
		// A5b：贴图关、GGX 开。颜色应仍是中性灰 (0.8,0.8,0.8)，
		// 但高光分布与 A5a 不同（GGX 的 D/V/F 三项都在工作）。
		m_Params.EnableMaterialTextures = false;
		m_Params.EnableGGX = true;
		LogState(L"[2] A5b textures OFF + GGX ON");
		RequestShot(L"r7_02_A5b_ggx_only.png");
		break;

	case 3:
		// A5c：贴图开、GGX 关。有真实颜色，但着色仍是 R6 的 Lambert + 天空环境。
		// 与 A5d 对比可以看出 GGX 单独贡献了多少。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = false;
		LogState(L"[3] A5c textures ON + GGX OFF (Lambert + textures)");
		RequestShot(L"r7_03_A5c_textures_only.png");
		break;

	case 4:
		// BaseColor 调试通道：这是与 DeferredRenderer 唯一能逐像素比对的通道。
		// 两边都选 BaseColor 时基色应当一致，若不一致说明贴图寻址/UV/采样器
		// 这条链出了问题（GGX 与光照都已被这个通道绕过，不参与比对）。
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::BaseColor;
		LogState(L"[4] debug view = BaseColor (compare with Deferred)");
		RequestShot(L"r7_04_debug_basecolor.png");
		break;

	case 5:
		// 【A7 关键对照】切到 DeferredRenderer，用**同一次运行、同一个模型、同一帧参数**
		// 拍一张 BaseColor 通道。这是唯一能把「贴图链路对不对」变成可判定命题的办法：
		//   若两边基色一致 → 贴图寻址/UV/采样器/baseColorFactor 全部正确；
		//   若不一致 → 差异一定在 DXR 侧的 UV 偏移、描述符表偏移或采样器上。
		// 为什么必须现场拍而不是拿历史 R3Test 的图比：
		//   历史图 `docs/AI/R6switch/r3_03_basecolor.png` 是在**另一条相机/模型状态下**
		//   拍的，模型可能不是同一个（R3Test 第 6 步会换模型），拿来比对会把
		//   "模型不同"误判成"贴图链路错了"。
		//
		// 【必须同时改 m_DebugViewIndex，不能只改 m_Params.Debug】
		//   DrawDebugViewPanel() 每帧无条件执行
		//       m_Params.Debug = (Renderer::DebugView)m_DebugViewIndex;
		//   （见 RendererDemoApp.cpp 的 DrawDebugViewPanel 末尾）。只设 m_Params.Debug
		//   会在同一帧的 UI 阶段被覆盖回 Final —— 实测后果：拍出来的 Deferred 是
		//   完整着色画面而不是 BaseColor 通道，看起来像"两边基色完全不同"，
		//   极易被误判成 DXR 贴图链路错了。这是本轮踩到的最隐蔽的一个坑。
		m_Params.EnableRTReflection = false;
		m_Params.Debug = Renderer::DebugView::BaseColor;
		SwitchRenderer(L"DeferredRenderer");
		LogState(L"[5] A7 deferred BaseColor (same model, same frame)");
		RequestShot(L"r7_05_deferred_basecolor.png");
		break;

	case 6:
		// 切回 DXR 并恢复通道。这一步同时验证「切走再切回」后贴图仍正确
		// （WP-1B 后统一堆与模型全程不卸载，直接复用 Zone A 材质块）。
		m_Params.Debug = Renderer::DebugView::Final;
		SwitchRenderer(L"RayTracingRenderer");
		m_Params.EnableMaterialTextures = true;
		m_Params.EnableGGX = true;
		m_Params.RTDebugView = Renderer::RayTracingDebugView::BaseColor;
		LogState(L"[6] back to DXR, BaseColor again (descriptor re-alloc check)");
		RequestShot(L"r7_06_dxr_basecolor_after_switch.png");
		break;

	case 7:
		// 反射 + 贴图组合：确认 R7 没有破坏 R6 的反射路径。
		m_Params.RTDebugView = Renderer::RayTracingDebugView::Shaded;
		m_Params.EnableRTReflection = true;
		LogState(L"[7] textures + GGX + reflection");
		RequestShot(L"r7_07_with_reflection.png");
		break;

	case 8:
		// 复原到 R6 的默认组合（阴影开、反射关），用于与 -R6Test 第 0 步交叉验证。
		m_Params.EnableRTReflection = false;
		LogState(L"[8] restored to R6 defaults");
		RequestShot(L"r7_08_restored.png");
		break;

	case 9:
		// ImGui 面板取证：确认两个新勾选框与 BaseColor 通道在 UI 上真的存在
		// （不能只在代码里存在而面板没画出来）。
		LogState(L"[9] ui capture");
		{
			wchar_t uiPath[512] = {};
			swprintf_s(uiPath, L"%ws\\r7_09_imgui_panel.png", m_R7TestDir.c_str());
			m_PendingUICapturePath = uiPath;
			LOG_INFO(L"[R7-Test] step %d -> %ws (with ImGui)\n", m_R7TestStep, uiPath);
		}
		break;

	case 10:
		LogState(L"[10] final");
		LOG_INFO(L"[R7-Test] all steps done\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// [WP-0B] 管线反复热切换压测（-SwitchTest <N>）
// ------------------------------------------------------------
// 目的：以自动化方式验证 TD-03a（Shutdown 反序）与 TD-05d（PSO InputLayout
// new[]/delete[] 配对）在反复切换 Renderer >= 5 次后无崩溃、无 CRT 堆损坏、
// Debug Layer 0 ERROR。每步间隔 3 帧，保证至少提交并呈现完整帧。
// ============================================================
void RendererDemoApp::RunSwitchTest(void)
{
	static const int kFramesPerSwitch = 3;
	if ((m_SwitchTestFrame++ % kFramesPerSwitch) != 0)
		return;

	if (m_SwitchTestCount >= m_SwitchTestTarget)
	{
		LOG_INFO(L"[SwitchTest] PASS: completed %u renderer switches without crash/heap corruption\n", m_SwitchTestCount);
		PostQuitMessage(0);
		return;
	}

	static const wchar_t* kCycle[] = { L"ForwardRenderer", L"DeferredRenderer", L"GFL2CharacterPipeline", L"RayTracingRenderer" };
	const wchar_t* targetName = kCycle[m_SwitchTestCount % _countof(kCycle)];
	// 若当前恰好已是该渲染器则顺延一位，确保每步都触发真实的 Shutdown + Initialize。
	if (m_RendererName == targetName)
		targetName = kCycle[(m_SwitchTestCount + 1) % _countof(kCycle)];

	++m_SwitchTestCount;
	LOG_INFO(L"[SwitchTest] switch %u/%u -> %ws\n", m_SwitchTestCount, m_SwitchTestTarget, targetName);
	SwitchRenderer(targetName);

#ifdef _DEBUG
	const int heapOk = _CrtCheckMemory();
	ASSERT(heapOk != 0, "CRT debug heap corruption detected after SwitchRenderer");
	LOG_INFO(L"[SwitchTest] switch %u/%u CRT heap check = %d\n", m_SwitchTestCount, m_SwitchTestTarget, heapOk);
#endif
}

// ============================================================
// [WP-1B / TD-08a] 模拟新 Renderer Initialize() 失败时的保底验收（-SwitchFailTest <dir>）
// ------------------------------------------------------------
// 流程：
//   frame 5  : 拍摄故障前含 UI 画面 before_fail_ui.png 与纯场景 before_fail_scene.png
//   frame 8  : 尝试切换到 ForwardRenderer（simulateFailure=true），断言返回 false、
//              旧 Renderer（DeferredRenderer）、SceneColor 与已加载模型均原封不动
//   frame 12 : 拍摄故障后纯场景 after_fail_scene.png 与含 UI 画面 after_fail_ui.png
//   frame 15 : 打印 PASS 并优雅退出（exit 0）
// ============================================================
void RendererDemoApp::RunSwitchFailTest(void)
{
	++m_SwitchFailTestFrame;
	auto MakePath = [this](const wchar_t* name) {
		wchar_t buf[512] = {};
		swprintf_s(buf, L"%ws\\%ws", m_SwitchFailTestDir.c_str(), name);
		return std::wstring(buf);
	};

	if (m_SwitchFailTestFrame == 5)
	{
		m_PendingCapturePath = MakePath(L"before_fail_scene.png");
	}
	else if (m_SwitchFailTestFrame == 6)
	{
		m_PendingUICapturePath = MakePath(L"before_fail_ui.png");
	}
	else if (m_SwitchFailTestFrame == 8 && !m_SwitchFailTriggered)
	{
		m_SwitchFailTriggered = true;
		const std::wstring prevRenderer = m_RendererName;
		const size_t prevModelCount = m_SceneModels.size();
		const Scene::Model::Model* prevModelPtr = prevModelCount > 0 ? m_SceneModels[0].instance->GetModel() : nullptr;

		const bool switched = SwitchRenderer(L"ForwardRenderer", /*simulateFailure=*/true);
		const bool intact =
			(!switched) &&
			(m_RendererName == prevRenderer) &&
			(m_Renderer != nullptr && m_Renderer->IsInitialized()) &&
			(Display::SceneColorBuffer != nullptr && Display::SceneColorBuffer->GetResource() != nullptr) &&
			(m_SceneModels.size() == prevModelCount) &&
			(prevModelCount == 0 || m_SceneModels[0].instance->GetModel() == prevModelPtr) &&
			Renderer::ImGuiLayer::IsInitialized();

		LOG_INFO(L"[SwitchFailTest] switched=%d renderer=%ws models=%zu uiInit=%d intact=%d\n",
			switched ? 1 : 0, m_RendererName.c_str(), m_SceneModels.size(),
			Renderer::ImGuiLayer::IsInitialized() ? 1 : 0, intact ? 1 : 0);
		ASSERT(intact, "TD-08a SwitchRenderer failure rollback violated!");
		if (!intact)
		{
			LOG_ERROR(L"[SwitchFailTest] FAIL: old renderer/model/UI not preserved!\n");
			PostQuitMessage(1);
		}
	}
	else if (m_SwitchFailTestFrame == 12)
	{
		m_PendingCapturePath = MakePath(L"after_fail_scene.png");
	}
	else if (m_SwitchFailTestFrame == 13)
	{
		m_PendingUICapturePath = MakePath(L"after_fail_ui.png");
	}
	else if (m_SwitchFailTestFrame == 16)
	{
		LOG_INFO(L"[SwitchFailTest] PASS: simulated Initialize() failure preserved old renderer, model and UI\n");
		PostQuitMessage(0);
	}
}

// ============================================================
// [WP-1C / TD-09c] DXR 删空全部模型后清屏验收（-EmptyDxrTest <dir>）
// ============================================================
void RendererDemoApp::RunEmptyDxrTest(void)
{
	++m_EmptyDxrTestFrame;
	auto MakePath = [this](const wchar_t* name) {
		wchar_t buf[512] = {};
		swprintf_s(buf, L"%ws\\%ws", m_EmptyDxrTestDir.c_str(), name);
		return std::wstring(buf);
	};

	if (m_EmptyDxrTestFrame == 5)
	{
		m_PendingCapturePath = MakePath(L"dxr_before_clear.png");
	}
	else if (m_EmptyDxrTestFrame == 8)
	{
		ClearAllModels();
		LOG_INFO(L"[EmptyDxrTest] Cleared all models in DXR mode (count=%zu)\n", m_SceneModels.size());
	}
	else if (m_EmptyDxrTestFrame == 12)
	{
		m_PendingCapturePath = MakePath(L"dxr_after_clear.png");
	}
	else if (m_EmptyDxrTestFrame == 15)
	{
		LOG_INFO(L"[EmptyDxrTest] PASS: DXR empty scene clear capture finished\n");
		PostQuitMessage(0);
	}
}

bool RendererDemoApp::SavePresetToFile(const std::wstring& path)
{
	m_Config.SetPipelineFromRendererName(m_RendererName);
	m_Config.FromRenderParams(m_Params);
	std::string err;
	if (!m_Config.SaveToFile(path, &err))
	{
		m_PresetStatus = "Save failed: " + err;
		LOG_ERROR(L"[Preset] 保存失败 %ws：%hs\n", path.c_str(), err.c_str());
		return false;
	}
	m_PresetStatus = "Saved: " + Narrow(path);
	LOG_INFO(L"[Preset] 已保存配置至 %ws\n", path.c_str());
	return true;
}

bool RendererDemoApp::LoadPresetFromFile(const std::wstring& path, bool switchPipeline)
{
	Renderer::RenderConfig loaded;
	std::string err;
	if (!loaded.LoadFromFile(path, &err))
	{
		m_PresetStatus = "Load failed: " + err;
		LOG_ERROR(L"[Preset] 加载失败 %ws（已安全拦截不崩溃）：%hs\n", path.c_str(), err.c_str());
		return false;
	}
	m_Config = loaded;
	m_Params = m_Config.ToRenderParams();
	if (switchPipeline)
	{
		const std::wstring targetPipe = m_Config.GetPipelineRendererName();
		if (!targetPipe.empty() && targetPipe != m_RendererName)
			m_PendingRendererName = targetPipe;
	}
	m_PresetStatus = "Loaded: " + Narrow(path);
	LOG_INFO(L"[Preset] 已加载配置 %ws (pipeline=%hs, exposure=%.2f)\n",
		path.c_str(), m_Config.pipeline.c_str(), m_Params.Exposure);
	return true;
}

void RendererDemoApp::ExportStatsIfRequested(void)
{
	if (m_StatsOutPath.empty() || m_StatsWritten)
		return;
	if (m_GpuTimer.GetPassTimings().empty() && Graphics::GetFrameCount() < 15)
		return;

	Renderer::FrameStatsReport rep;
	rep.Pipeline     = Narrow(m_RendererName);
	rep.Width        = g_DisplayWidth;
	rep.Height       = g_DisplayHeight;
	rep.FrameIndex   = Graphics::GetFrameCount();
	rep.CpuFrameMs   = m_CpuFrameMs;
	rep.GpuFrameMs   = m_GpuFrameMs;
	const Graphics::VramUsageInfo vram = Graphics::QueryVramUsage();
	rep.VramUsageMB  = static_cast<double>(vram.CurrentUsageBytes) / (1024.0 * 1024.0);
	rep.VramBudgetMB = static_cast<double>(vram.BudgetBytes) / (1024.0 * 1024.0);
	rep.BindlessSM66 = m_Host.GetServices().IsBindlessSM66Ready();
	rep.SetHeapCalls = m_LastRenderSceneHeapBindCount;
	for (const auto& pt : m_GpuTimer.GetPassTimings())
		rep.Passes.push_back({ pt.Name, pt.GpuMs });

	std::string err;
	if (Renderer::WriteStatsReportJson(m_StatsOutPath, rep, &err))
	{
		m_StatsWritten = true;
		LOG_INFO(L"[StatsOut] 已写出客观性能报告：%ws (GPU=%.3fms, passes=%zu)\n",
			m_StatsOutPath.c_str(), rep.GpuFrameMs, rep.Passes.size());
	}
	else
	{
		LOG_ERROR(L"[StatsOut] 写出失败 %ws：%hs\n", m_StatsOutPath.c_str(), err.c_str());
	}
}

// ============================================================
// [WP-3B / 契约 7.2] 自动化 F5 Shader 热重载验收（-HotReloadTest <dir>）
// ------------------------------------------------------------
// 严格执行任务卡双场景验证，并保证退出前 100% 字节级还原原 HLSL 文件：
//   1) Case 1：故意写入 HLSL 语法错误 -> 按 F5 触发增量重编 -> 必须失败并生成非空红字
//      LastError、零崩溃、旧 PSO 保持渲染（hr_01_after_syntax_error.png vs hr_00_baseline.png RMSE==0）；
//   2) Case 2：修改输出颜色公式为反色 -> F5 即时变色（hr_02_live_edited.png）；连续热重载 >= 5 次
//      经 DeferredReleaseQueue 退休旧 PSO -> 还原原始源码并 F5（hr_03_restored.png RMSE==0），
//      退出校验 Debug Layer 0 ERROR 与 Device::Release refcount==0。
// ============================================================
void RendererDemoApp::RunHotReloadTest(void)
{
	++m_HotReloadTestFrame;
	auto MakePath = [this](const wchar_t* name) {
		wchar_t buf[512] = {};
		swprintf_s(buf, L"%ws\\%ws", m_HotReloadTestDir.c_str(), name);
		return std::wstring(buf);
	};
	auto WriteAllText = [](const std::wstring& path, const std::string& content) -> bool {
		std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!ofs.is_open()) return false;
		ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
		ofs.flush();
		// 将文件 mtime 前推 2 秒，确保 100% 跨越文件系统时间戳粒度触发 F5 增量检出
		ofs.close();
		HANDLE hFile = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			FILETIME ft = {};
			::GetSystemTimeAsFileTime(&ft);
			ULARGE_INTEGER ui = {};
			ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
			static uint64_t s_Bump = 1;
			ui.QuadPart += (s_Bump++) * 20000000ull; // +2s/step
			ft.dwLowDateTime = ui.LowPart; ft.dwHighDateTime = ui.HighPart;
			::SetFileTime(hFile, nullptr, nullptr, &ft);
			::CloseHandle(hFile);
		}
		return true;
	};

	if (m_HotReloadTestFrame == 2)
	{
		const bool bindless = m_Host.GetServices().IsBindlessSM66Ready();
		const std::wstring rel = bindless
			? L"Engine\\Shader\\04_PostProcess\\TonemapBindlessPS.hlsl"
			: L"Engine\\Shader\\04_PostProcess\\TonemapPS.hlsl";
		m_HotReloadTargetFile = m_Host.GetHotReloader().GetRepoRootDir() + L"\\" + rel;
		std::ifstream ifs(m_HotReloadTargetFile, std::ios::in | std::ios::binary);
		if (!ifs.is_open())
		{
			LOG_ERROR(L"[HotReloadTest] FAIL: cannot open %ws\n", m_HotReloadTargetFile.c_str());
			PostQuitMessage(3);
			return;
		}
		std::ostringstream ss;
		ss << ifs.rdbuf();
		m_HotReloadBackupBytes = ss.str();
		m_HotReloadBackupValid = true;
		LOG_INFO(L"[HotReloadTest] Target=%ws (%zu bytes backed up)\n",
			m_HotReloadTargetFile.c_str(), m_HotReloadBackupBytes.size());
	}
	else if (m_HotReloadTestFrame == 5)
	{
		m_PendingCapturePath = MakePath(L"hr_00_baseline.png");
	}
	else if (m_HotReloadTestFrame == 8 && m_HotReloadBackupValid)
	{
		// Case 1：故意注入 HLSL 语法错误，触发 F5 增量重编
		const std::string broken = m_HotReloadBackupBytes + "\nSYNTAX_ERROR_INTENTIONAL_WP3B;;;\n";
		WriteAllText(m_HotReloadTargetFile, broken);
		const auto rep = m_Host.TriggerShaderHotReload(/*forceAll=*/false);
		LOG_INFO(L"[HotReloadTest] Case1 (syntax error): ok=%u fail=%u hasErr=%d\n",
			rep.Succeeded, rep.Failed, rep.LastError.empty() ? 0 : 1);
		if (rep.Failed == 0 || rep.LastError.empty())
		{
			LOG_ERROR(L"[HotReloadTest] FAIL: expected DXC syntax error!\n");
			WriteAllText(m_HotReloadTargetFile, m_HotReloadBackupBytes);
			PostQuitMessage(4);
		}
	}
	else if (m_HotReloadTestFrame == 12)
	{
		m_PendingCapturePath = MakePath(L"hr_01_after_syntax_error.png");
	}
	else if (m_HotReloadTestFrame == 15 && m_HotReloadBackupValid)
	{
		// Case 2：修改颜色公式（1.0 - displayColor 反色）并 F5 增量热重载
		std::string edited = m_HotReloadBackupBytes;
		const std::string needle = "return float4(displayColor, 1.0f);";
		const size_t pos = edited.find(needle);
		if (pos != std::string::npos)
			edited.replace(pos, needle.size(), "return float4(1.0f - displayColor, 1.0f);");
		WriteAllText(m_HotReloadTargetFile, edited);
		const auto rep = m_Host.TriggerShaderHotReload(/*forceAll=*/false);
		m_HotReloadSuccessCount += rep.Succeeded;
		LOG_INFO(L"[HotReloadTest] Case2 reload #1 (live invert): ok=%u fail=%u totalOk=%u\n",
			rep.Succeeded, rep.Failed, m_HotReloadSuccessCount);
		if (rep.Succeeded == 0 || rep.Failed != 0)
		{
			LOG_ERROR(L"[HotReloadTest] FAIL: live edit reload failed: %hs\n", rep.LastError.c_str());
			WriteAllText(m_HotReloadTargetFile, m_HotReloadBackupBytes);
			PostQuitMessage(5);
		}
	}
	else if (m_HotReloadTestFrame == 19)
	{
		m_PendingCapturePath = MakePath(L"hr_02_live_edited.png");
	}
	else if (m_HotReloadTestFrame >= 21 && m_HotReloadTestFrame <= 24 && m_HotReloadBackupValid)
	{
		// 连续触发 4 次额外热重载（合计 >= 5 次），压测 DeferredReleaseQueue 旧 PSO 退休零泄漏
		const auto rep = m_Host.TriggerShaderHotReload(/*forceAll=*/true);
		m_HotReloadSuccessCount += rep.Succeeded;
		LOG_INFO(L"[HotReloadTest] Stress reload frame %d: ok=%u fail=%u totalOk=%u\n",
			m_HotReloadTestFrame, rep.Succeeded, rep.Failed, m_HotReloadSuccessCount);
		if (rep.Failed != 0)
		{
			WriteAllText(m_HotReloadTargetFile, m_HotReloadBackupBytes);
			PostQuitMessage(6);
		}
	}
	else if (m_HotReloadTestFrame == 27 && m_HotReloadBackupValid)
	{
		// 还原原始 HLSL 字节并再次 F5 热重载恢复原画面
		WriteAllText(m_HotReloadTargetFile, m_HotReloadBackupBytes);
		const auto rep = m_Host.TriggerShaderHotReload(/*forceAll=*/false);
		m_HotReloadSuccessCount += rep.Succeeded;
		LOG_INFO(L"[HotReloadTest] Restored original HLSL: ok=%u fail=%u totalOk=%u\n",
			rep.Succeeded, rep.Failed, m_HotReloadSuccessCount);
		if (rep.Succeeded == 0 || rep.Failed != 0)
		{
			PostQuitMessage(7);
		}
	}
	else if (m_HotReloadTestFrame == 31)
	{
		m_PendingCapturePath = MakePath(L"hr_03_restored.png");
	}
	else if (m_HotReloadTestFrame == 34)
	{
		LOG_INFO(L"[HotReloadTest] PASS: syntax error rollback + live edit + %u reloads + exact restore\n",
			m_HotReloadSuccessCount);
		PostQuitMessage(0);
	}
}

// ============================================================
// [WP-3C] 自动化 Shadow(Simple/CSM) + SSAO + TAA A/B 验收（-WP3CTest <dir>）
// ------------------------------------------------------------
// 通过与 UI 勾选框完全一致的 RenderParams 真源路径依次切换：
//   0: 全关基线 (Shadow=0 SSAO=0 TAA=0) -> 须与 golden_p2 RMSE==0
//   1: Shadow SimpleShadowMap (2048) ON
//   2: Shadow 热切至 CSM (4 cascades, 2048) ON（不重启、延迟回收旧深度图）
//   3: Shadow 连续热切 Simple(1024) -> CSM(2, 2048) 压测无崩无漏
//   4: SSAO ON (Blur ON)
//   5: SSAO ON + DebugView::AO 直接取证遮蔽图
//   6: TAA ON（连续累积 10 帧 Halton(2,3) 亚像素抖动）
//   7: Shadow(CSM) + SSAO + TAA 三者全开组合验证
//   8: 含 ImGui 面板截图（验证 Renderer Feature Toggles 含 TAA 与 WP-3C 控制面板）
//   9: 全部关闭复原 -> 须与 step 0 逐像素 RMSE==0
// ============================================================
void RendererDemoApp::RunWp3cTest(void)
{
	static const int kFramesPerStep = 10;
	if ((m_Wp3cTestFrame++ % kFramesPerStep) != 0)
		return;

	auto MakePath = [this](const wchar_t* fileName) {
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_Wp3cTestDir.c_str(), fileName);
		return std::wstring(path);
	};

	auto LogState = [this](const wchar_t* tag) {
		const uint32_t dsvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].GetLiveAllocated();
		const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
		LOG_INFO(L"[WP3C-Test] %ws shadow=%d(alg=%u,res=%u,cas=%u) ssao=%d(blur=%d) taa=%d debug=%u DSV=%u SRV=%u heaps=%u\n",
			tag,
			m_Params.EnableShadow ? 1 : 0, m_Params.ShadowAlgorithm, m_Params.ShadowResolution, m_Params.ShadowCascadeCount,
			m_Params.EnableSSAO ? 1 : 0, m_Params.SSAOEnableBlur ? 1 : 0,
			m_Params.EnableTAA ? 1 : 0, (uint32_t)m_Params.Debug,
			dsvLive, srvLive, m_LastRenderSceneHeapBindCount);
	};

	switch (m_Wp3cTestStep++)
	{
	case 0:
		m_Params.EnableShadow = false;
		m_Params.EnableSSAO   = false;
		m_Params.EnableTAA    = false;
		m_Params.Debug        = Renderer::DebugView::Final;
		LogState(L"[0] baseline all OFF");
		m_PendingCapturePath = MakePath(L"wp3c_00_baseline_off.png");
		break;

	case 1:
		m_Params.EnableShadow     = true;
		m_Params.ShadowAlgorithm  = 0; // SimpleShadowMap
		m_Params.ShadowResolution = 2048;
		LogState(L"[1] Shadow SimpleShadowMap 2048");
		m_PendingCapturePath = MakePath(L"wp3c_01_shadow_simple.png");
		break;

	case 2:
		m_Params.ShadowAlgorithm    = 1; // CSM
		m_Params.ShadowCascadeCount = 4;
		m_Params.ShadowResolution   = 2048;
		LogState(L"[2] Shadow CSM 4-cascade 2048");
		m_PendingCapturePath = MakePath(L"wp3c_02_shadow_csm.png");
		break;

	case 3:
		// 额外热切换验证：Simple(1024)
		m_Params.ShadowAlgorithm  = 0;
		m_Params.ShadowResolution = 1024;
		LogState(L"[3] Shadow hot-switch Simple 1024");
		break;

	case 4:
		m_Params.EnableShadow   = false;
		m_Params.EnableSSAO     = true;
		m_Params.SSAOEnableBlur = true;
		LogState(L"[4] SSAO ON (blur ON)");
		m_PendingCapturePath = MakePath(L"wp3c_03_ssao_on.png");
		break;

	case 5:
		m_Params.Debug = Renderer::DebugView::AO;
		LogState(L"[5] SSAO DebugView::AO");
		m_PendingCapturePath = MakePath(L"wp3c_04_ssao_debug_ao.png");
		break;

	case 6:
		m_Params.Debug      = Renderer::DebugView::Final;
		m_Params.EnableSSAO = false;
		m_Params.EnableTAA  = true;
		LogState(L"[6] TAA ON warmup");
		break;

	case 7:
		LogState(L"[7] TAA ON converged capture");
		m_PendingCapturePath = MakePath(L"wp3c_05_taa_on.png");
		break;

	case 8:
		m_Params.EnableShadow       = true;
		m_Params.ShadowAlgorithm    = 1;
		m_Params.ShadowCascadeCount = 4;
		m_Params.ShadowResolution   = 2048;
		m_Params.EnableSSAO         = true;
		m_Params.EnableTAA          = true;
		LogState(L"[8] ALL ON (CSM + SSAO + TAA) warmup");
		break;

	case 9:
		LogState(L"[9] ALL ON capture");
		m_PendingCapturePath = MakePath(L"wp3c_06_all_on.png");
		break;

	case 10:
		LogState(L"[10] ImGui panels capture (ALL ON)");
		m_PendingUICapturePath = MakePath(L"wp3c_07_imgui_panel.png");
		break;

	case 11:
		m_Params.EnableShadow = false;
		m_Params.EnableSSAO   = false;
		m_Params.EnableTAA    = false;
		m_Params.Debug        = Renderer::DebugView::Final;
		LogState(L"[11] restored all OFF");
		m_PendingCapturePath = MakePath(L"wp3c_08_restored_off.png");
		break;

	case 12:
		LogState(L"[12] final");
		LOG_INFO(L"[WP3C-Test] PASS: Shadow(Simple/CSM) + SSAO + TAA A/B and hot-switch checks completed\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}

// ============================================================
// [契约 4] 统一 PrimarySurfaceDesc 跨管线验收（-PrimarySurfaceTest <dir>）
// ------------------------------------------------------------
// 1. DeferredRenderer：校验 Source=RasterGBuffer + IsBindlessReady，回读 Albedo/Normal/Material PNG；
// 2. 帧边界热切至 RayTracingRenderer：校验 Source=DxrPrimaryRay + IsBindlessReady，回读 Depth/Albedo/Normal/Material PNG；
// 3. 切回 DeferredRenderer 并干净退出，供脚本验证光栅与 DXR 表面轮廓重合、深度范围合法且 0 泄漏。
// ============================================================
void RendererDemoApp::RunPrimarySurfaceTest(void)
{
	static const int kFramesPerStep = 8;
	if ((m_PrimarySurfaceTestFrame++ % kFramesPerStep) != 0)
		return;

	auto MakePath = [this](const wchar_t* fileName) {
		wchar_t path[512] = {};
		swprintf_s(path, L"%ws\\%ws", m_PrimarySurfaceTestDir.c_str(), fileName);
		return std::wstring(path);
	};

	auto VerifyAndDump = [this, &MakePath](Renderer::PrimarySurfaceSource expectedSource, const wchar_t* prefix, bool dumpDepth) -> bool {
		if (!m_Renderer)
			return false;
		const Renderer::PrimarySurfaceDesc* surf = m_Renderer->GetPrimarySurface();
		if (surf == nullptr || surf->Source != expectedSource || !surf->IsBindlessReady())
		{
			LOG_ERROR(L"[PrimarySurfaceTest] FAIL: %ws surface invalid (ptr=%p src=%u bindless=%d)\n",
				prefix, (void*)surf, surf ? (unsigned)surf->Source : 0u, surf && surf->IsBindlessReady() ? 1 : 0);
			m_FatalExitCode = 1;
			return false;
		}
		const auto packed = surf->PackBindlessIndices();
		LOG_INFO(L"[PrimarySurfaceTest] %ws OK: source=%hs size=%ux%u idx(D=%u,A=%u,N=%u,M=%u,E=%u) heaps=%u\n",
			prefix, Renderer::PrimarySurfaceSourceName(surf->Source), packed.Width, packed.Height,
			packed.DepthSrvIdx, packed.AlbedoAoSrvIdx, packed.NormalSrvIdx, packed.MaterialSrvIdx,
			packed.EmissionSrvIdx, m_LastRenderSceneHeapBindCount);

		auto DumpColor = [&](const Renderer::PrimarySurfaceSlot& slot, const wchar_t* suffix) {
			if (!slot.IsValid()) return false;
			wchar_t fname[128] = {};
			swprintf_s(fname, L"%ws_%ws.png", prefix, suffix);
			return Renderer::DebugCapture::SaveColorBufferToPNG(*static_cast<ColorBuffer*>(slot.Resource), MakePath(fname));
		};
		bool ok = DumpColor(surf->AlbedoAO, L"albedo")
			&& DumpColor(surf->Normal,   L"normal")
			&& DumpColor(surf->Material, L"material");
		if (dumpDepth)
			ok = ok && DumpColor(surf->Depth, L"depth");
		if (!ok)
		{
			LOG_ERROR(L"[PrimarySurfaceTest] FAIL: DebugCapture export failed for %ws\n", prefix);
			m_FatalExitCode = 1;
		}
		return ok;
	};

	switch (m_PrimarySurfaceTestStep++)
	{
	case 0:
		// 预热 DeferredRenderer 若干帧后在 step 1 验收并回读
		break;

	case 1:
		if (!VerifyAndDump(Renderer::PrimarySurfaceSource::RasterGBuffer, L"deferred", /*dumpDepth=*/false))
			return;
		m_PendingRendererName = L"RayTracingRenderer";
		break;

	case 2:
		if (!VerifyAndDump(Renderer::PrimarySurfaceSource::DxrPrimaryRay, L"dxr", /*dumpDepth=*/true))
			return;
		m_PendingRendererName = L"DeferredRenderer";
		break;

	case 3:
		// 确认切回光栅管线后 PrimarySurface 重新指向 RasterGBuffer 且无残余状态
		if (m_Renderer == nullptr || m_Renderer->GetPrimarySurface() == nullptr ||
			m_Renderer->GetPrimarySurface()->Source != Renderer::PrimarySurfaceSource::RasterGBuffer)
		{
			LOG_ERROR(L"[PrimarySurfaceTest] FAIL: switch back to DeferredRenderer did not restore RasterGBuffer surface\n");
			m_FatalExitCode = 1;
			return;
		}
		LOG_INFO(L"[PrimarySurfaceTest] PASS: RasterGBuffer + DxrPrimaryRay unified surface contract verified\n");
		PostQuitMessage(0);
		break;

	default:
		break;
	}
}


// ============================================================
// 生命周期
// ============================================================
void RendererDemoApp::Startup(void)
{
	// ---- [TD-10b / WP-3B] 阶段 1：先集中解析全部 CLI 参数（含 -Preset/-Experiment/-StatsOut/-HotReloadTest） ----
	bool cliSpecifiedStartRenderer = false;
	// 原实现先调用 SwitchRenderer()（其内部会用默认 model 0 且 m_ForceRebuildOnLoad==false
	// 加载模型），随后才读取 -RebuildModels 与 -ModelIndex 再加载第二次，导致 -RebuildModels
	// 对首帧首次加载完全失效且启动白白加载两次模型。
	std::wstring startRendererName = L"DeferredRenderer";
	{
		std::wstring cliRenderer;
		if (CommandLineArgs::GetString(L"StartRenderer", cliRenderer) && !cliRenderer.empty())
		{
			startRendererName = cliRenderer;
			cliSpecifiedStartRenderer = true;
			LOG_INFO(L"[R3] -StartRenderer %ws\n", startRendererName.c_str());
		}
	}

	if (CommandLineArgs::HasFlag(L"SimulateNoDXR"))
	{
		Graphics::SetSimulatedNoDxrForTest(true);
		m_StrictStartRenderer = true;
		LOG_INFO(L"[WP-3A] -SimulateNoDXR enabled: forcing RaytracingTier=NOT_SUPPORTED for CheckSupport verification\n");
	}
	if (CommandLineArgs::HasFlag(L"StrictStartRenderer"))
	{
		m_StrictStartRenderer = true;
	}

	{
		std::wstring statsPath;
		if (CommandLineArgs::GetString(L"StatsOut", statsPath) && !statsPath.empty())
		{
			m_StatsOutPath = statsPath;
			LOG_INFO(L"[CLI] -StatsOut %ws\n", m_StatsOutPath.c_str());
		}
	}
	{
		std::wstring hrDir;
		if (CommandLineArgs::GetString(L"HotReloadTest", hrDir) && !hrDir.empty())
		{
			m_HotReloadTestMode = true;
			m_HotReloadTestDir = hrDir;
			LOG_INFO(L"[HotReloadTest] 自动化 F5 热重载验收启用，输出目录：%ws\n", m_HotReloadTestDir.c_str());
		}
	}
	{
		std::wstring expPath;
		if (CommandLineArgs::GetString(L"Experiment", expPath) && !expPath.empty())
		{
			m_ExperimentMode = true;
			if (LoadPresetFromFile(expPath, /*switchPipeline=*/false) && !cliSpecifiedStartRenderer)
				startRendererName = m_Config.GetPipelineRendererName();
		}
	}
	{
		std::wstring presetPath;
		if (CommandLineArgs::GetString(L"Preset", presetPath) && !presetPath.empty())
		{
			if (LoadPresetFromFile(presetPath, /*switchPipeline=*/false) && !cliSpecifiedStartRenderer)
				startRendererName = m_Config.GetPipelineRendererName();
		}
	}

	m_ForceRebuildOnLoad = CommandLineArgs::HasFlag(L"RebuildModels");
	if (m_ForceRebuildOnLoad)
		LOG_INFO(L"[CLI] -RebuildModels enabled (will force rebuild .ty on initial model load)\n");

	uint32_t startModel = 0;
	{
		uint32_t cliModel = 0;
		uint32_t catalogCount = 0;
		DemoCommon::GetModelCatalog(catalogCount);
		if (CommandLineArgs::GetInteger(L"ModelIndex", cliModel) && cliModel < catalogCount)
		{
			startModel = cliModel;
			LOG_INFO(L"[R3] -ModelIndex %u\n", startModel);
		}
	}

	{
		uint32_t switchCount = 0;
		if (CommandLineArgs::GetInteger(L"SwitchTest", switchCount) && switchCount > 0)
		{
			m_SwitchTestMode = true;
			m_SwitchTestTarget = switchCount;
			LOG_INFO(L"[SwitchTest] 管线反复切换压测启用，目标次数：%u\n", m_SwitchTestTarget);
		}
	}
	{
		std::wstring failTestDir;
		if (CommandLineArgs::GetString(L"SwitchFailTest", failTestDir) && !failTestDir.empty())
		{
			m_SwitchFailTestMode = true;
			m_SwitchFailTestDir = failTestDir;
			LOG_INFO(L"[SwitchFailTest] TD-08a 失败保底验收启用，输出目录：%ws\n", m_SwitchFailTestDir.c_str());
		}
	}
	{
		std::wstring emptyDxrDir;
		if (CommandLineArgs::GetString(L"EmptyDxrTest", emptyDxrDir) && !emptyDxrDir.empty())
		{
			m_EmptyDxrTestMode = true;
			m_EmptyDxrTestDir = emptyDxrDir;
			LOG_INFO(L"[EmptyDxrTest] TD-09c DXR 空场景清屏验收启用，输出目录：%ws\n", m_EmptyDxrTestDir.c_str());
		}
	}

	// ---- R3/R4/R6/R7 脚本化验收参数 ----
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R3Test", testDir) && !testDir.empty())
		{
			m_R3TestMode = true;
			m_R3TestDir = testDir;
			LOG_INFO(L"[R3-Test] 脚本化验收开始，输出目录 %ws\n", m_R3TestDir.c_str());
		}
	}
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R4Test", testDir) && !testDir.empty())
		{
			m_R4TestMode = true;
			m_R4TestDir = testDir;
			LOG_INFO(L"[R4-Test] 脚本化验收开始，输出目录 %ws\n", m_R4TestDir.c_str());
		}
	}
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R6Test", testDir) && !testDir.empty())
		{
			m_R6TestMode = true;
			m_R6TestDir = testDir;
			LOG_INFO(L"[R6-Test] 脚本化验收开始，输出目录 %ws\n", m_R6TestDir.c_str());
		}
	}
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"R7Test", testDir) && !testDir.empty())
		{
			m_R7TestMode = true;
			m_R7TestDir = testDir;
			LOG_INFO(L"[R7-Test] 脚本化验收开始，输出目录 %ws\n", m_R7TestDir.c_str());
		}
	}
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"WP3CTest", testDir) && !testDir.empty())
		{
			m_Wp3cTestMode = true;
			m_Wp3cTestDir = testDir;
			LOG_INFO(L"[WP3C-Test] 延迟管线 Shadow/SSAO/TAA 验收开始，输出目录 %ws\n", m_Wp3cTestDir.c_str());
		}
	}
	{
		std::wstring testDir;
		if (CommandLineArgs::GetString(L"PrimarySurfaceTest", testDir) && !testDir.empty())
		{
			m_PrimarySurfaceTestMode = true;
			m_PrimarySurfaceTestDir = testDir;
			LOG_INFO(L"[PrimarySurfaceTest] 统一表面契约跨管线验收开始，输出目录 %ws\n", m_PrimarySurfaceTestDir.c_str());
		}
	}

	// ---- 阶段 2：注册具体 Demo 模块并初始化 RenderServices ----
	{
		m_Modules.clear();
		m_Modules.push_back(std::make_unique<DemoModules::DeferredRenderDemoModule>());
		m_Modules.push_back(std::make_unique<DemoModules::ForwardRenderDemoModule>());
		m_Modules.push_back(std::make_unique<DemoModules::RayTracingRenderDemoModule>());
		m_Modules.push_back(std::make_unique<DemoModules::GFL2RenderDemoModule>());

		m_RendererNames.clear();
		for (const auto& mod : m_Modules)
			m_RendererNames.push_back(mod->GetDisplayName());

		auto& registry = Renderer::RendererRegistry::Instance();
		LOG_INFO(L"[Manager] 已加载 Demo 模块数量：%u (底层 Renderer 注册数：%u)\n",
			static_cast<uint32_t>(m_Modules.size()),
			static_cast<uint32_t>(registry.GetRegisteredNames().size()));
	}

	const bool enableHotReload = m_HotReloadTestMode || (!m_ExperimentMode && ScreenshotDirectory().empty());
	if (!m_Host.InitializeServices(enableHotReload))
	{
		LOG_ERROR(L"[WP-2B] 致命：RendererHost::InitializeServices 失败\n");
		return;
	}

	const bool startOk = SwitchRenderer(startRendererName, /*simulateFailure=*/false);
	if (!startOk && m_StrictStartRenderer)
	{
		LOG_ERROR(L"[WP-3A][CheckSupport] 请求的启动管线 %ws 不可用（原因：%hs），严禁冒名回退，将以非零退出码 2 优雅中止\n",
			startRendererName.c_str(), m_Host.GetLastSwitchError().c_str());
		m_FatalExitCode = 2;
		PostQuitMessage(m_FatalExitCode);
		return;
	}

	m_GpuTimer.Initialize(Graphics::g_Device, 3);
	if (m_ActiveModule == nullptr || m_ActiveModule->ShouldEnsureDefaultModelOnActivate() || cliSpecifiedStartRenderer || m_ForceRebuildOnLoad)
		LoadModelByIndex(startModel, m_ForceRebuildOnLoad);

	if (!m_CameraController)
	{
		DemoCommon::DemoHostContext ctx = BuildDemoContext();
		DemoCommon::SetupStandardRightHandedCamera(ctx, /*resetPose=*/true);
	}

	// ---- ImGui（WP-1B：复用 RenderServices 的 4 分区单可见堆 Zone D） ----
	if (!Renderer::ImGuiLayer::Initialize(GameCore::GetWindowHandle(), &m_Host.GetServices().GetDescriptorHeap()))
		LOG_ERROR(L"[R3] 错误：ImGuiLayer::Initialize 失败，UI 不可用\n");
	GameCore::SetWndProcHook(&RendererDemoApp::WndProcThunk);

	LOG_INFO(L"[R3] RendererDemoApp 启动完成\n");
}

void RendererDemoApp::Cleanup(void)
{
	// 安全兜底：若 -HotReloadTest 异常提前退出，务必还原磁盘上的原始 HLSL 字节
	if (m_HotReloadBackupValid && !m_HotReloadTargetFile.empty())
	{
		std::ofstream ofs(m_HotReloadTargetFile, std::ios::out | std::ios::binary | std::ios::trunc);
		if (ofs.is_open())
			ofs.write(m_HotReloadBackupBytes.data(), static_cast<std::streamsize>(m_HotReloadBackupBytes.size()));
		m_HotReloadBackupValid = false;
	}
	ExportStatsIfRequested();

	// [WP-1B / 契约 2 严格反序关闭]
	// 1. 消息钩子 -> 2. ImGuiLayer（归还 Zone D）-> 3. GpuTimer -> 4. IdleGPU ->
	// 5. Renderer（含 EnvLighting HDR TextureRef）-> 6. 场景模型（清 Model/TextureRef/Zone A）->
	// 7. RenderServices（LightingSystem -> TextureManager -> UnifiedDescriptorHeap）
	GameCore::SetWndProcHook(nullptr);
	Renderer::ImGuiLayer::Shutdown();

	m_GpuTimer.Shutdown();

	Graphics::g_CommandManager.IdleGPU();
	Display::SceneColorBuffer = nullptr;

	m_Host.ShutdownPipeline();
	m_Renderer = nullptr;
	UnloadModel();
	m_Host.ShutdownServices();
}

void RendererDemoApp::OnResize(uint32_t width, uint32_t height)
{
	LOG_INFO(L"[R3] OnResize %ux%u\n", width, height);

	m_Host.OnResize(width, height);

	// Renderer 重建了内部 RT，Display 里的 shared_ptr 必须重绑，否则它会指向旧对象。
	BindRendererOutput();

	// SetAspectRatio 只更新投影矩阵，不会把相机标脏；这里显式 Update 一次，
	// 保证"窗口刚变完、玩家还没动鼠标"的那一帧 ViewProj 也是对的。
	m_Camera.SetAspectRatio((float)height / (float)width);
	m_Camera.Update();
}


// ============================================================
// 每帧
// ============================================================
void RendererDemoApp::Update(float deltaT)
{
	{
		static int s_uc = 0;
	}
	// 上一帧 RenderUI 请求的「含 ImGui 画面」截图，在这里补做：
	// 此刻 SceneColor 里仍是上一帧（几何 + UI）的完整结果，且本帧还没开始渲染。
	if (!m_PendingUICapturePath.empty())
	{
		const std::wstring path = m_PendingUICapturePath;
		m_PendingUICapturePath.clear();
		SaveCaptureTo(path);
	}

	// [WP-0B] -SwitchTest <N>：帧边界反复切换 Renderer（Deferred -> Forward -> RayTracing 循环），
	// 每次切换后在 Debug 下调用 _CrtCheckMemory() 校验堆完整性（验证 TD-03a / TD-05d 修复）。
	if (m_SwitchTestMode)
		RunSwitchTest();
	if (m_SwitchFailTestMode)
		RunSwitchFailTest();
	if (m_EmptyDxrTestMode)
		RunEmptyDxrTest();
	if (m_HotReloadTestMode)
		RunHotReloadTest();
	ExportStatsIfRequested();

	// [TD-11b] 记录前 3 帧实测 dt，确认首帧无历史 uptime 尖峰（<= 0.1s）
	{
		static int s_dtLogFrames = 0;
		if (s_dtLogFrames < 3)
		{
			LOG_INFO(L"[WP-1C][TD-11b] frame %d deltaT=%.6f s\n", s_dtLogFrames, deltaT);
			++s_dtLogFrames;
		}
	}

	// R3 脚本化验收（-R3Test）：它内部调用的就是 UI 按钮调用同一批函数，
	// 因此必须同样在任何 GraphicsContext 之前执行。
	if (m_R3TestMode)
		RunR3Test();
	if (m_R4TestMode)
		RunR4Test();
	if (m_R6TestMode)
		RunR6Test();
	if (m_R7TestMode)
		RunR7Test();
	if (m_Wp3cTestMode)
		RunWp3cTest();
	if (m_PrimarySurfaceTestMode)
		RunPrimarySurfaceTest();

	// 帧边界动作：切 Renderer / 换模型。必须在任何 GraphicsContext 之前。
	ApplyPendingActions();

	if (m_FatalExitCode != 0)
	{
		PostQuitMessage(m_FatalExitCode);
		return;
	}

	m_CpuFrameMs = Graphics::GetFrameTime() * 1000.0f;
	if (m_GpuTimer.TryGetElapsedMs(m_GpuFrameMs) && !m_LoggedWp3aTelemetry && !m_GpuTimer.GetPassTimings().empty())
	{
		m_LoggedWp3aTelemetry = true;
		const Graphics::VramUsageInfo vram = Graphics::QueryVramUsage();
		std::string passSummary;
		for (const auto& pt : m_GpuTimer.GetPassTimings())
		{
			char item[128] = {};
			snprintf(item, sizeof(item), "%s=%.3fms ", pt.Name.c_str(), pt.GpuMs);
			passSummary += item;
		}
		LOG_INFO(L"[WP-3A][Telemetry] Renderer=%ws GPU=%.3fms Passes=[%hs] VRAM=%.1f/%.1fMB BindlessSM66=%d\n",
			m_RendererName.c_str(), m_GpuFrameMs, passSummary.c_str(),
			static_cast<double>(vram.CurrentUsageBytes) / (1024.0 * 1024.0),
			static_cast<double>(vram.BudgetBytes) / (1024.0 * 1024.0),
			m_Host.GetServices().IsBindlessSM66Ready() ? 1 : 0);
	}

	// ImGui 捕获键盘时不驱动相机，否则在输入框里打字会把相机一起拖着走。
	// 注意：io.WantCaptureKeyboard 是在 ImGui::NewFrame 里算出来的，
	// 而 NewFrame 发生在本帧稍后的 RenderUI 里，所以这里拿到的是上一帧的状态。
	// 一帧的判定延迟对输入门控没有可见影响（而且这是 ImGui 官方推荐的用法）。
	if (m_ActiveModule != nullptr)
	{
		DemoCommon::DemoHostContext ctx = BuildDemoContext();
		m_ActiveModule->Update(ctx, deltaT);
	}
	else if (m_CameraController)
	{
		m_CameraController->Update(deltaT);
	}

	if (!m_Renderer || !m_Renderer->IsInitialized())
		return;

	// [WP-2B] 每帧经 RendererHost 推进 4 分区可见堆 Zone B 的 3 帧环形槽区
	m_Host.GetServices().BeginFrame(Graphics::GetFrameCount());

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = deltaT;
	for (DemoCommon::SceneModel& m : m_SceneModels)
	{
		if (m.instance && !m.instance->IsNull())
			desc.Models.push_back(m.instance.get());
	}
	desc.Params = &m_Params;

	m_Renderer->Update(desc, gfxContext);

	gfxContext.Finish();
}

void RendererDemoApp::RenderScene(void)
{
	if (!m_Renderer || !m_Renderer->IsInitialized())
		return;

	GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

	Renderer::RenderFrameDesc desc;
	desc.Camera = &m_Camera;
	desc.delatT = 0.0f;
	for (DemoCommon::SceneModel& m : m_SceneModels)
	{
		if (m.instance && !m.instance->IsNull())
			desc.Models.push_back(m.instance.get());
	}
	desc.Params = &m_Params;
	desc.Timer  = &m_GpuTimer;

	// GPU 计时：Begin/End 必须在同一个命令列表内，且 End 要包住全部绘制。
	m_GpuTimer.Begin(gfxContext);

	m_Renderer->Render(gfxContext, desc, Renderer::DrawPass::kOpaque);
	m_Renderer->EndFrame(gfxContext, desc);

	// [WP-1B / 契约 1 门禁] 校验主渲染命令列表内 SetDescriptorHeaps 调用次数 <= 1
	m_LastRenderSceneHeapBindCount = gfxContext.GetSetDescriptorHeapsCount();
	ASSERT(m_LastRenderSceneHeapBindCount <= 1, "RenderScene SetDescriptorHeaps count exceeded 1!");
	if (m_LoggedHeapBindForRenderer != m_RendererName)
	{
		m_LoggedHeapBindForRenderer = m_RendererName;
		LOG_INFO(L"[WP-1B] RenderScene SetDescriptorHeaps count = %u (renderer=%ws, ZoneA=%u, ZoneB=%u, ZoneD=%u)\n",
			m_LastRenderSceneHeapBindCount, m_RendererName.c_str(),
			m_Host.GetServices().GetDescriptorHeap().GetZoneALiveCount(),
			m_Host.GetServices().GetDescriptorHeap().GetZoneBUsedInCurrentFrame(),
			m_Host.GetServices().GetDescriptorHeap().GetZoneDLiveCount());
	}

	m_GpuTimer.End(gfxContext);

	const uint64_t fenceValue = gfxContext.Finish();

	// 提交之后才把围栏值交给计时器：只有它完成了才能安全 Map 回读堆。
	m_GpuTimer.OnSubmitted(fenceValue);

	// ---- GPU 回读截帧（必须在 Finish 之后：DebugCapture 内部会自己开上下文）----
	const std::wstring shotDir = ScreenshotDirectory();
	if (!shotDir.empty())
	{
		static int s_frame = 0;
		++s_frame;
		if (s_frame == 1 || s_frame == 2 || s_frame == 5 || s_frame == 30 || s_frame == 90)
			SaveCapture(shotDir, L"screenshot", s_frame);
	}
	if (m_CaptureRequested)
	{
		m_CaptureRequested = false;
		const std::wstring dir = shotDir.empty() ? std::wstring(L".") : shotDir;
		SaveCapture(dir, L"ui_capture", ++m_CaptureCounter);
	}
	if (!m_PendingCapturePath.empty())
	{
		const std::wstring path = m_PendingCapturePath;
		m_PendingCapturePath.clear();
		SaveCaptureTo(path);
	}
}

bool RendererDemoApp::SaveCapture(const std::wstring& directory, const wchar_t* prefix, int index)
{
	wchar_t path[512] = {};
	swprintf_s(path, L"%ws\\%ws_%d.png", directory.c_str(), prefix, index);
	return SaveCaptureTo(path);
}

bool RendererDemoApp::SaveCaptureTo(const std::wstring& fullPath)
{
	if (!Display::SceneColorBuffer || !Display::SceneColorBuffer->GetResource())
		return false;

	// 文件名里带上**资源实际尺寸**：如果 Renderer 侧 RT 与 Display 绑定的输出
	// 不是同一张（切 Renderer 后忘记重绑就会出现这种情况），看文件名就能发现。
	const D3D12_RESOURCE_DESC desc = Display::SceneColorBuffer->GetResource()->GetDesc();
	const bool ok = Renderer::DebugCapture::SaveColorBufferToPNG(*Display::SceneColorBuffer, fullPath);
	if (ok)
		LOG_INFO(L"[R3] 已导出 %ws (%ux%u)\n", fullPath.c_str(), (uint32_t)desc.Width, (uint32_t)desc.Height);
	else
		LOG_INFO(L"[R3] 导出失败 %ws\n", fullPath.c_str());
	return ok;
}


// ============================================================
// ImGui 面板
// ============================================================
void RendererDemoApp::RenderUI(GraphicsContext& context)
{
	if (!Renderer::ImGuiLayer::IsInitialized() || !Display::SceneColorBuffer)
		return;

	Renderer::ImGuiLayer::BeginFrame();

	DrawPanels();

	if (m_ShowDemoWindow)
		ImGui::ShowDemoWindow(&m_ShowDemoWindow);

	// 必须传 Display::SceneColorBuffer：UI 画在 SceneColor 上，随后被
	// PreparePresentSDR 一起拷到 backbuffer（路线图 §6.2 方案 A）。
	// 这一句也必须是本命令列表最后一个绘制步骤。
	Renderer::ImGuiLayer::Render(context, *Display::SceneColorBuffer);
}

void RendererDemoApp::DrawPanels(void)
{
	DrawPerformancePanel();
	DrawRendererPanel();
	DrawDevToolsPanel();
	if (m_ActiveModule != nullptr)
	{
		DemoCommon::DemoHostContext ctx = BuildDemoContext();
		m_ActiveModule->DrawPanels(ctx);
	}
}

void RendererDemoApp::DrawPerformancePanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(330, 190), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Performance"))
	{
		ImGui::End();
		return;
	}

	// CPU 帧时间来自 Display::Present 之间的间隔（Display.cpp:349-351），
	// GPU 帧时间来自 TIMESTAMP 查询堆。两者必须分开显示 —— 路线图 §6.3 的硬要求。
	ImGui::Text("CPU frame: %.2f ms", m_CpuFrameMs);
	ImGui::Text("CPU FPS:   %.1f", m_CpuFrameMs > 0.0f ? 1000.0f / m_CpuFrameMs : 0.0f);
	ImGui::Separator();
	if (m_GpuTimer.IsInitialized())
	{
		ImGui::Text("GPU frame: %.3f ms", m_GpuFrameMs);
		const auto& passTimings = m_GpuTimer.GetPassTimings();
		for (const auto& pt : passTimings)
		{
			ImGui::Text("  %-18s: %.3f ms", pt.Name.c_str(), pt.GpuMs);
		}
		bool enabled = m_GpuTimer.IsEnabled();
		if (ImGui::Checkbox("GPU timer enabled", &enabled))
			m_GpuTimer.SetEnabled(enabled);
	}
	else
	{
		ImGui::TextDisabled("GPU frame: n/a (timer not initialized)");
	}
	ImGui::Separator();
	ImGui::Text("Frame index: %llu", (unsigned long long)Graphics::GetFrameCount());

	// 描述符池的 live 数是"切 Renderer 有没有持续泄漏"最直接的观测点：
	// 每切一次 renderer，RTV/SRV 的 live 数应该回到同一水平，而不是逐次上升。
	const uint32_t rtvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetLiveAllocated();
	const uint32_t srvLive = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetLiveAllocated();
	const uint32_t heapPool = Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetHeapPoolSize()
	                        + Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetHeapPoolSize();
	ImGui::Text("Desc live: RTV %u / SRV %u", rtvLive, srvLive);
	ImGui::Text("Desc heap pool: %u", heapPool);
	ImGui::Text("SetDescriptorHeaps/frame: %u", m_LastRenderSceneHeapBindCount);
	if (m_Host.GetServices().IsInitialized())
	{
		const auto& uheap = m_Host.GetServices().GetDescriptorHeap();
		ImGui::Text("UnifiedHeap A:%u B:%u C:%u D:%u",
			uheap.GetZoneALiveCount(), uheap.GetZoneBUsedInCurrentFrame(),
			uheap.GetZoneCLiveCount(), uheap.GetZoneDLiveCount());
		ImGui::Text("Bindless SM6.6: %s", m_Host.GetServices().IsBindlessSM66Ready() ? "ACTIVE" : "Fallback SM6.4");
	}
	{
		const Graphics::VramUsageInfo vram = Graphics::QueryVramUsage();
		const double curMB    = static_cast<double>(vram.CurrentUsageBytes) / (1024.0 * 1024.0);
		const double budgetMB = static_cast<double>(vram.BudgetBytes) / (1024.0 * 1024.0);
		ImGui::Text("VRAM (local): %.1f / %.1f MB", curMB, budgetMB);
	}

	ImGui::End();
}

void RendererDemoApp::DrawRendererPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(16, 620), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(360, 330), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Renderer"))
	{
		ImGui::End();
		return;
	}

	const char* activeModuleName = m_ActiveModule ? m_ActiveModule->GetDisplayName() : "<none>";
	ImGui::Text("Active Demo: %s", activeModuleName);
	ImGui::TextDisabled("Pipeline:    %s", Narrow(m_RendererName).c_str());

	std::vector<const char*> names;
	names.reserve(m_RendererNames.size());
	for (const std::string& name : m_RendererNames)
		names.push_back(name.c_str());
	if (!names.empty())
		ImGui::Combo("Available", &m_RendererComboIndex, names.data(), (int)names.size());

	const bool sameAsCurrent =
		m_ActiveModule != nullptr &&
		m_RendererComboIndex >= 0 && m_RendererComboIndex < (int)m_RendererNames.size() &&
		m_RendererNames[(size_t)m_RendererComboIndex] == m_ActiveModule->GetDisplayName();

	ImGui::BeginDisabled(sameAsCurrent || m_RendererNames.empty());
	if (ImGui::Button("Switch demo"))
		m_PendingRendererName = std::wstring(m_RendererNames[(size_t)m_RendererComboIndex].begin(),
		                                     m_RendererNames[(size_t)m_RendererComboIndex].end());
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("(next frame)");

	ImGui::Separator();
	if (m_Renderer)
	{
		ImGui::Text("Declared features:");
		ImGui::TextWrapped("%s", FeaturesToString(m_Renderer->GetFeatures()).c_str());
		const D3D12_VIEWPORT viewport = m_Renderer->GetMainViewport();
		ImGui::Text("Viewport: %.0f x %.0f", viewport.Width, viewport.Height);

		const Renderer::PrimarySurfaceDesc* surf = m_Renderer->GetPrimarySurface();
		if (surf != nullptr && surf->IsComplete())
		{
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
				"PrimarySurface: %s (%ux%u, Bindless=%s)",
				Renderer::PrimarySurfaceSourceName(surf->Source),
				surf->Width, surf->Height,
				surf->IsBindlessReady() ? "OK" : "OFF");
		}
		else
		{
			ImGui::TextDisabled("PrimarySurface: N/A (%s)", Narrow(m_RendererName).c_str());
		}
	}
	else
	{
		ImGui::TextDisabled("No renderer instance");
	}

	ImGui::Separator();
	const Graphics::RaytracingCapability cap = Graphics::QueryRaytracingCapability();
	if (cap.supported)
	{
		const char* tierText = (cap.tier >= D3D12_RAYTRACING_TIER_1_1) ? "1.1" : "1.0";
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
			"DXR: supported (tier %s, inline=%s)", tierText, cap.inlineRaytracing ? "yes" : "no");
	}
	else
	{
		ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "DXR: not supported on this device");
	}

	ImGui::End();
}

void RendererDemoApp::DrawDevToolsPanel(void)
{
	ImGui::SetNextWindowPos(ImVec2(390, 180), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 210), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("DevTools"))
	{
		ImGui::End();
		return;
	}

	// [WP-2C / 契约 5] Deferred 与 DXR 均已接入统一共享 TonemapPass，按真实 PostProcess 特征位开放曝光/ACES/sRGB 控件
	const bool postConnected = m_Renderer && Renderer::HasFeature(m_Renderer->GetFeatures(), Renderer::RendererFeature::PostProcess);
	ImGui::BeginDisabled(!postConnected);
	ImGui::SliderFloat("Exposure", &m_Params.Exposure, 0.05f, 8.0f);
	ImGui::Checkbox("Tonemap (ACES)", &m_Params.EnableTonemap);
	// R4：输出色彩空间。SceneColor / swapchain 都是 *_UNORM，
	// 关掉它就能直接看到"线性值被当作 gamma 值显示"的样子，用于 A/B 取证。
	ImGui::Checkbox("sRGB encode", &m_Params.EnableSRGBEncode);
	ImGui::EndDisabled();
	if (!postConnected)
		ImGui::TextDisabled("TonemapPass is bypassed on the unlit Forward path.");

	// R4：IBL 烘焙产物的真实 mip 上界。只读 —— 它不是用户参数，
	// 而是 PrefilterCube 的资产事实（渲染器烘焙后回填）。
	ImGui::Separator();
	ImGui::Text("Prefilter mip range: 0 .. %.0f", Scene::LightingSystem::GetPrefilterMaxMip());
	ImGui::TextDisabled("(derived from baked cube mip count)");

	ImGui::Separator();
	if (ImGui::Button("Capture SceneColor"))
		m_CaptureRequested = true;       // 截图必须在 RenderScene 的 Finish 之后
	ImGui::SameLine();
	ImGui::TextDisabled("(GPU readback)");

	if (ImGui::Button("Toggle ImGui demo"))
		m_ShowDemoWindow = !m_ShowDemoWindow;

	// [WP-3B / 契约 6] JSON Preset 保存 / 加载（手写精简 UI，零反射）
	ImGui::Separator();
	ImGui::Text("Preset (RenderConfig JSON):");
	static const wchar_t kDefaultPresetPath[] = L"preset_last.json";
	if (ImGui::Button("Save Preset"))
		SavePresetToFile(kDefaultPresetPath);
	ImGui::SameLine();
	if (ImGui::Button("Load Preset"))
		LoadPresetFromFile(kDefaultPresetPath, /*switchPipeline=*/true);
	if (!m_PresetStatus.empty())
		ImGui::TextWrapped("%s", m_PresetStatus.c_str());

	// [WP-3B / 契约 7.2] 开发期按键 Shader 热重载（F5 / Shift+F5）与红字报错条
	ImGui::Separator();
	ImGui::Text("Shader Hot Reload (dev):");
	if (m_Host.IsShaderHotReloadEnabled())
	{
		if (ImGui::Button("Reload Shaders (F5)"))
		{
			s_PendingHotReload = true;
			s_PendingHotReloadForceAll = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("Force All (Shift+F5)"))
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
		ImGui::TextDisabled("Disabled (batch/screenshot/shipping mode)");
	}

	ImGui::Separator();
	ImGui::Text("Captures: %d", m_CaptureCounter);
	const std::wstring dir = ScreenshotDirectory();
	ImGui::TextDisabled("Dir: %s", dir.empty() ? "<none, pass -Screenshot dir>" : Narrow(dir).c_str());

	ImGui::End();
}


// ============================================================
int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<RendererDemoApp>(L"RendererDemoApp", hInst, cmd);
}
