// ============================================================================
// Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h
// DXR 光线追踪管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/Common/IDemoModule.h"

namespace DemoModules
{
	class RayTracingRenderDemoModule final : public DemoCommon::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "RayTracingRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"RayTracingRenderer"; }

		void OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation) override;
		void Update(DemoCommon::DemoHostContext& ctx, float deltaT) override;
		void DrawPanels(DemoCommon::DemoHostContext& ctx) override;
		void OnResetDefaults(DemoCommon::DemoHostContext& ctx) override;

	private:
		void DrawRayTracingPanel(DemoCommon::DemoHostContext& ctx);
		void DrawSunLightingPanel(DemoCommon::DemoHostContext& ctx);
	};
}
