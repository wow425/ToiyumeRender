#pragma once
#pragma once

#include "PCH.h"

namespace GameCore
{
    // 程序最小化信号
    extern bool gIsSupending;

    class IGameApp
    {
    public:
        // 此函数用于初始化应用层状态，它会在必要的硬件资源分配完成后运行。
        //  某些不依赖于这些硬件资源的内部状态（如指针初始化和标志位 / Flag 设置），仍应在构造函数中进行初始化。
        virtual void Startup(void) = 0;
        virtual void Cleanup(void) = 0;

        // 决定是否app关闭，默认使用ESC
        virtual bool IsDone(void);

        // Update方法将会在每一帧被调用一次。状态更新与场景渲染逻辑都应当在这个方法中处理。
        virtual void Update(float deltaT) = 0;

        // 主rendering pass
        virtual void RenderScene(void) = 0;

        // 可选的UI rendering pass.此阶段处于 LDR（低动态范围）空间，且缓冲区（Buffer）已经预先清空。
        virtual void RenderUI(class GraphicsContext&) {};
    };
}

namespace GameCore
{
    int RunApplication(IGameApp& app, const wchar_t* className, HINSTANCE hInst, int nCmdShow);
}

#define CREATE_APPLICATION( app_class ) \
    int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE /*hPrevInstance*/, _In_ LPWSTR /*lpCmdLine*/, _In_ int nCmdShow) \
    { \
        return GameCore::RunApplication( app_class(), L#app_class, hInstance, nCmdShow ); \
    }
