#include "PCH.h"
#include "GameCore.h"
#include "../RHI/GraphicsCore.h"
#include "SystemTime.h"
#include "GameInput.h"
#include "../Resource/ResourceManager/BufferManager.h"
#include "../RHI/Command/CommandContext.h"
// #include "PostEffects.h"
#include "Display.h"
// #include "Util/CommandLineArg.h"
#include <shellapi.h>

#pragma comment(lib, "runtimeobject.lib") //

namespace GameCore
{
    using namespace Graphics;

    bool gIsSupending = false;

    void InitializeApplication(IGameApp& game)
    {
        // 图形模块初始化
        Graphics::Initialize();
        // 计时器模块
        SystemTime::Initialize();
        //GameInput::Initialize();
        //EngineTuning::Initialize();

        game.Startup();


    }

    void TerminateApplication(IGameApp& game)
    {

    }

    bool UpdateApplication(IGameApp& game)
    {
        // 获取每帧时间
        float DelataTime = Graphics::GetFrameTime();

        // GameInput::Update(DeltaTime); 输入模块必须没写，必须写，不然没法实现最小可运行渲染器
        game.Update(DelataTime);
        game.RenderScene(); // 没写

        Display::Present(); // 没完成需看看

        return !game.IsDone();
    }

    // Default implementation to be overridden by the application
    bool IGameApp::IsDone(void)
    {
        return 1;
    }

    HWND g_hWnd = nullptr;

    LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    int RunApplication(IGameApp& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow)
    {
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

        // 初始化应用
        InitializeApplication(app);

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

    //--------------------------------------------------------------------------------------
    // 应用每次接收到消息时调用
    //--------------------------------------------------------------------------------------
    LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_SIZE:
            Display::Resize((UINT)(UINT64)lParam & 0xFFFF, (UINT)(UINT64)lParam >> 16);
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

