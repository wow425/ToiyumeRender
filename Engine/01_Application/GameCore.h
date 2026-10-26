#pragma once

#include "00_Core/PCH.h"

class GraphicsContext;

namespace GameCore
{
	// 程序最小化信号
	extern bool gIsSupending;

	class IGameApp
	{
	public:
		virtual void Startup(void) = 0;
		virtual void Cleanup(void) = 0;

		virtual bool IsDone(void);

		virtual void Update(float deltaT) = 0;

		virtual void RenderScene(void) = 0;

		// ============================================================
		// UI 绘制回调（R3）
		// ------------------------------------------------------------
		// 调用时机：RenderScene 之后、Display::Present 之前，
		// 目标缓冲是 Display::SceneColorBuffer（已处于 RENDER_TARGET 状态，
		// 视口/Scissor 已按当前尺寸设置）。
		//
		// 注意方向：UI 画在 SceneColor 上，随后由 PreparePresentSDR 整体
		// Copy 到 backbuffer。这样 Display 的 swapchain 状态机完全不用改，
		// 也避免了「先 Copy 后画 UI」导致 ImGui 被覆盖的经典错误。
		// ============================================================
		virtual void RenderUI(GraphicsContext&) {};

		// 是否需要引擎为其准备 UI 绘制环境（R3）。
		// 默认 false：不用 UI 的 App 不必每帧多付一次 GraphicsContext
		// 开销与一次资源状态转换。要用 UI 的 App 覆写为 true。
		virtual bool HasRenderUI() const { return false; }

		// ============================================================
		// 窗口尺寸变化回调（R2）
		// ------------------------------------------------------------
		// 调用时机：swapchain 及其 backbuffer 已经按新尺寸重建**之后**。
		// 因此实现方可以安全地按新尺寸重建自己的渲染目标，
		// 不必再操心 swapchain 本身。
		//
		// width/height 保证非 0（最小化时 Display 不会调用本函数）。
		// 默认空实现：不需要跟随尺寸的 App 可以不覆写。
		// ============================================================
		virtual void OnResize(uint32_t /*width*/, uint32_t /*height*/) {}

		virtual bool RequiresRaytracingSupport() const { return false; }
	};

	// 当前活动 App（R2 引入）。Display::Resize 通过它转发尺寸变化。
	// 声明必须放在 IGameApp 定义之后——它是指向该类型对象的指针。
	extern IGameApp* g_pActiveApp;

	// 主窗口句柄访问器（R2 引入）。
	// g_hWnd 本身留在 GameCore.cpp 内部，避免外层代码直接改写它；
	// App 侧只在验收自测里需要它（把真实窗口尺寸同步到目标分辨率）。
	HWND GetWindowHandle(void);

	// ============================================================
	// 消息钩子（R3）
	// ------------------------------------------------------------
	// 让 UI 层（Dear ImGui Win32 后端）在不被 GameCore 直接依赖的前提下
	// 拿到原始窗口消息。
	//
	// 语义约定（与 imgui_impl_win32 官方注释一致）：
	//   返回值**不能**用来判断「消息是否已被消费」——官方 handler 对
	//   它处理过的消息一律 return 0。正确做法是「无条件把消息喂给 UI」，
	//   再由 App 侧读 io.WantCaptureMouse / WantCaptureKeyboard 决定
	//   是否响应相机输入。
	//
	// 因此本钩子的返回值只作为调试信息，不影响消息分发。
	// 传 nullptr 注销。
	// ============================================================
	using WndProcHook = bool (*)(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam);
	void SetWndProcHook(WndProcHook hook);
}

namespace GameCore
{
	int RunApplication(IGameApp& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow);

	template<typename T>
	int WinRun(const wchar_t* className, HINSTANCE hInstance, int nCmdShow)
	{
		static_assert(std::is_base_of_v<IGameApp, T>, "T must inherit from IGameApp");
		T app;
		return RunApplication(app, className, hInstance, nCmdShow);
	}
}


