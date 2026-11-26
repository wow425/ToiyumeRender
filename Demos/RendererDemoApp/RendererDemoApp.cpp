// ============================================================================
// Demos/RendererDemoApp/RendererDemoApp.cpp
// 图形研究工作站统一管理入口：挂载 4 个独立渲染管线模块并交由 DemoWorkstation 运行
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/RendererDemoApp/DemoWorkstation.h"
#include "Demos/DeferredRenderDemo/DeferredRenderDemoModule.h"
#include "Demos/ForwardRenderDemo/ForwardRenderDemoModule.h"
#include "Demos/GFL2RenderDemo/GFL2RenderDemoModule.h"
#include "Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h"

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	std::vector<std::unique_ptr<Demo::IDemoModule>> modules;
	modules.push_back(std::make_unique<DemoModules::DeferredRenderDemoModule>());
	modules.push_back(std::make_unique<DemoModules::ForwardRenderDemoModule>());
	modules.push_back(std::make_unique<DemoModules::RayTracingRenderDemoModule>());
	modules.push_back(std::make_unique<DemoModules::GFL2RenderDemoModule>());
	return Demo::RunWorkstationApp(L"RendererDemoApp", std::move(modules), hInst, cmd);
}
