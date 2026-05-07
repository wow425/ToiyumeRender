#pragma once

#include "PCH.h"

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

        virtual void RenderUI(GraphicsContext&) {};
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
