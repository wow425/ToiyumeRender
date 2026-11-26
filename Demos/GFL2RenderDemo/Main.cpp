// ============================================================================
// Demos/GFL2RenderDemo/Main.cpp — 《少女前线2：追放》AK-12 管线独立启动入口
// ============================================================================
#include "Demos/Common/DemoStandaloneApp.h"
#include "Demos/GFL2RenderDemo/GFL2RenderDemoModule.h"

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<DemoCommon::DemoStandaloneApp<DemoModules::GFL2RenderDemoModule>>(
		L"GFL2RenderDemo", hInst, cmd);
}
