// ============================================================================
// Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h
// DXR 光线追踪管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/RendererDemoApp/DemoWorkstation.h"

namespace DemoModules
{
	class RayTracingRenderDemoModule final : public Demo::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "RayTracingRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"RayTracingRenderer"; }

		void OnActivate(Demo::HostContext& ctx, bool isFirstActivation) override;
		void Update(Demo::HostContext& ctx, float deltaT) override;
		void DrawPanels(Demo::HostContext& ctx) override;
		void OnResetDefaults(Demo::HostContext& ctx) override;

	private:
		void DrawRayTracingPanel(Demo::HostContext& ctx);
		void DrawSunLightingPanel(Demo::HostContext& ctx);
	};
}
