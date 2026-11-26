// ============================================================================
// Demos/RayTracingRenderDemo/Main.cpp — 光线追踪管线独立启动入口
// ============================================================================
#include "Demos/Common/DemoStandaloneApp.h"
#include "Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h"

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return GameCore::WinRun<DemoCommon::DemoStandaloneApp<DemoModules::RayTracingRenderDemoModule>>(
		L"RayTracingRenderDemo", hInst, cmd);
}
