// ============================================================================
// Demos/DeferredRenderDemo/Main.cpp — 延迟渲染管线独立启动入口
// ============================================================================
#include "Demos/Common/DemoStandaloneApp.h"
#include "Demos/DeferredRenderDemo/DeferredRenderDemoModule.h"

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<DemoCommon::DemoStandaloneApp<DemoModules::DeferredRenderDemoModule>>(
		L"DeferredRenderDemo", hInst, cmd);
}
