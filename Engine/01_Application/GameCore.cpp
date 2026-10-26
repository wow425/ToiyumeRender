#include "00_Core/PCH.h"
#include "00_Core/Utility/Utility.h"
#include "GameCore.h"
#include "02_RHI/GraphicsCore.h"
#include "00_Core/SystemTime.h"
#include "01_Application/GameInput.h"
#include "04_Renderer/BufferManager.h"
#include "02_RHI/Command/CommandContext.h"
#include "Display.h"
#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/CommandLineArg.h"
#include "02_RHI/Resource/ColorBuffer.h"

#include <shellapi.h>

#pragma comment(lib, "runtimeobject.lib") //





namespace GameCore
{
	using namespace Graphics;

	bool gIsSupending = false;

	HWND g_hWnd = nullptr;

	// 当前活动 App 的指针（R2）。
	// Display::Resize 需要回调它来重建上层渲染目标，但 Display 层
	// 不应该依赖具体的 App 类型，所以用 IGameApp* 中转。
	// 由 RunApplication 设置，生命周期覆盖整个消息循环。
	IGameApp* g_pActiveApp = nullptr;

	// R2：只读访问主窗口句柄
	HWND GetWindowHandle(void)
	{
		return g_hWnd;
	}

	// R3：窗口消息钩子。
	// UI 层（Dear ImGui Win32 后端）需要原始消息，但 GameCore 不应该
	// 反向依赖 04_Renderer 里的 ImGuiLayer——所以只存一个函数指针。
	// 约定见 GameCore.h：返回值不代表「已消费」。
	static WndProcHook s_WndProcHook = nullptr;

	void SetWndProcHook(WndProcHook hook)
	{
		s_WndProcHook = hook;
	}

	void InitializeApplication(IGameApp& game)
	{
		// ============================================================
		// R0.2：资源根解析必须在任何资产加载之前完成。
		// 顺序说明：命令行 → AssetsRoot → 图形设备 → 应用 Startup()。
		// 之所以放在 Graphics::Initialize 之前，是因为 Startup() 里第一件事
		// 往往就是加载模型，而解析资源根不依赖任何 GPU 资源。
		// ============================================================
		CommandLineArgs::InitializeFromSystemCommandLine();
		if (!Asset::Initialize())
		{
			Utility::Printf(L"[Asset] 致命：无法解析 Assets 目录，后续所有模型/贴图加载都会失败。\n");
		}
		Asset::LogResolution();

		Graphics::Initialize(game.RequiresRaytracingSupport());// 图形模块初始化
		LOG_TRACE(L"[Boot] Graphics::Initialize returned\n");

		SystemTime::Initialize();// 计时器模块
		LOG_TRACE(L"[Boot] SystemTime::Initialize done\n");

		GameInput::Initialize();// IO输入还没啃
		LOG_TRACE(L"[Boot] GameInput::Initialize done\n");
		//EngineTuning::Initialize();

		LOG_INFO(L"[Boot] calling game.Startup()\n");
		game.Startup();
		LOG_INFO(L"[Boot] game.Startup() returned\n");
	}

	bool UpdateApplication(IGameApp& game)
	{
		// ============================================================
		// 最小化挂起（R2）
		// ------------------------------------------------------------
		// 最小化后没有可呈现的 backbuffer，必须彻底跳过 GPU 工作。
		// 这里连 Update 也一起跳过：避免相机时间推进、
		// 避免模型常量缓冲上传等无意义开销。
		// 只是简单让出一小段时间片再回来检查窗口状态。
		// ============================================================
		if (gIsSupending)
		{
			Sleep(16);
			return !game.IsDone();
		}

		// 更新
		{
			float DeltaTime = Graphics::GetFrameTime();				// 获取每帧时间
			GameInput::Update(DeltaTime);
			game.Update(DeltaTime);									// 每帧数据更新
		}


		// 渲染
		{
			game.RenderScene();										// 绘制

			// ========================================================
			// UI 绘制（R3）
			// --------------------------------------------------------
			// 时机：RenderScene 之后、Present 之前。
			//
			// 路线图 §6.2 明确警告过：不能把 UI 插到 Display::Present
			// 内部去「画在 backbuffer 上」——那条路径要自己接管
			// COPY_DEST → RENDER_TARGET → PRESENT 的状态机，容易写错。
			// 这里采用另一条更稳的路：UI 画在 SceneColor 上，
			// 随后由 PreparePresentSDR 整体 Copy 到 backbuffer。
			//
			// 代价：每帧多一次 GraphicsContext 与一次状态转换；
			// 只有显式声明 HasRenderUI() 的 App 才付这个代价。
			// ========================================================
			if (game.HasRenderUI() && Display::SceneColorBuffer != nullptr &&
				Display::SceneColorBuffer->GetResource() != nullptr)
			{
				// 状态转换 / RTV 绑定 / 视口设置都由 UI 层自己做
				// （它比 GameCore 更清楚 ImGui 后端的需要），这里只负责
				// 「给一个独立的、已收尾的 Context」。
				GraphicsContext& uiContext = GraphicsContext::Begin(L"Render UI");
				game.RenderUI(uiContext);
				uiContext.Finish();
			}

			Display::Present();
		}


		return !game.IsDone();
	}

	void TerminateApplication(IGameApp& game)
	{
		g_CommandManager.IdleGPU();

		game.Cleanup();

		GameInput::Shutdown();
	}

	// Default implementation to be overridden by the application
	bool IGameApp::IsDone(void)
	{
		return GameInput::IsFirstPressed(GameInput::kKey_escape);
	}


	LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);


	int RunApplication(IGameApp& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow)
	{
		// R2：登记活动 App，供 Display::Resize 回调。
		// 必须在创建窗口之前设置：WM_SIZE 在窗口创建阶段
		// 就可能触发，而 Display 尚未初始化时会自行跳过。
		g_pActiveApp = &app;

		// 检查是否支持SSE2（Streaming SIMD Extensions 2）指令集
		if (!XMVerifyCPUSupport())
			return 1;
		// 
		Microsoft::WRL::Wrappers::RoInitializeWrapper InitializeWinRT(RO_INIT_MULTITHREADED);
		ASSERT_SUCCEEDED(InitializeWinRT);

		// Register class
		WNDCLASSEX wcex;
		wcex.cbSize = sizeof(WNDCLASSEX);
		wcex.style = CS_HREDRAW | CS_VREDRAW;
		wcex.lpfnWndProc = WndProc;
		wcex.cbClsExtra = 0;
		wcex.cbWndExtra = 0;
		wcex.hInstance = hInst;
		wcex.hIcon = LoadIcon(hInst, IDI_APPLICATION);
		wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wcex.lpszMenuName = nullptr;
		wcex.lpszClassName = className;
		wcex.hIconSm = LoadIcon(hInst, IDI_APPLICATION);
		ASSERT(0 != RegisterClassEx(&wcex), "Unable to register a window");

		// Create window
		RECT rc = { 0, 0, (LONG)g_DisplayWidth, (LONG)g_DisplayHeight };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

		g_hWnd = CreateWindow(className, className, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
			rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
		ASSERT(g_hWnd != 0);
		// ===========================================================================================================
		// ===========================================================================================================
		// ===========================================================================================================   
		InitializeApplication(app);  // 初始化应用

		ShowWindow(g_hWnd, nCmdShow/*SW_SHOWDEFAULT*/);

		do
		{
			MSG msg = {};
			bool done = false;
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);

				if (msg.message == WM_QUIT)
					done = true;
			}
			if (done)
				break;
		} while (UpdateApplication(app));	// 更新应用

		TerminateApplication(app); // 终止应用
		Graphics::Shutdown(); // 没完成
		return 0;
	}


	LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		// ============================================================
		// R3：先无条件把消息喂给 UI 层。
		// ------------------------------------------------------------
		// 必须是**无条件**的，且不能依赖返回值：
		// imgui_impl_win32.cpp 的 handler 对它处理过的消息一律 return 0，
		// 官方注释也建议「总是把全部输入交给 Dear ImGui，然后用
		// io.WantCaptureMouse / WantCaptureKeyboard 决定应用是否响应」。
		// 返回值这里仅丢弃，不参与消息分发。
		// ============================================================
		if (s_WndProcHook != nullptr)
		{
			s_WndProcHook((void*)hWnd, (unsigned)message, (uint64_t)wParam, (int64_t)lParam);
		}

		switch (message)
		{
		// ============================================================
		// WM_SIZE（R2）
		// ------------------------------------------------------------
		// 最小化时 wParam == SIZE_MINIMIZED，lParam 的宽高是 0。
		// 若直接透传给 Display::Resize，虽然 Display 内部已有 0 尺寸守卫，
		// 但**渲染循环仍会继续跑**：没有可用的 backbuffer，
		// 而 SceneColorBuffer 已被 Destroy，于是必崩。
		//
		// 因此这里分两条路：
		//   - 最小化：置挂起标志，渲染循环跳过绘制与 Present
		//   - 其它（含还原）：清标志，并正常触发 Resize
		// ============================================================
		case WM_SIZE:
			if (wParam == SIZE_MINIMIZED)
			{
				gIsSupending = true;
				LOG_INFO(L"[Window] Minimized, suspending render loop\n");
			}
			else
			{
				gIsSupending = false;
				Display::Resize((UINT)(UINT64)lParam & 0xFFFF, (UINT)(UINT64)lParam >> 16);
			}
			break;

		// R1：键盘消息转发给 GameInput。
		// DirectInput 在部分环境不可用，若只依赖它，WASD 会完全无响应。
		// WM_KEYDOWN/WM_KEYUP 无论输入来自真实键盘还是 SendInput 都能收到。
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			GameInput::NotifyWin32Key((unsigned int)wParam, true);
			break;

		case WM_KEYUP:
		case WM_SYSKEYUP:
			GameInput::NotifyWin32Key((unsigned int)wParam, false);
			break;

		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}

		return 0;
	}


}