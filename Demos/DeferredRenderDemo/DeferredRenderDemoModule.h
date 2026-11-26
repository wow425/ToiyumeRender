// ============================================================================
// Demos/DeferredRenderDemo/DeferredRenderDemoModule.h
// 延迟渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/Common/IDemoModule.h"

namespace DemoModules
{
	class DeferredRenderDemoModule final : public DemoCommon::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "DeferredRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"DeferredRenderer"; }

		void OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation) override;
		void OnDeactivate(DemoCommon::DemoHostContext& ctx) override;
		void Update(DemoCommon::DemoHostContext& ctx, float deltaT) override;
		void DrawPanels(DemoCommon::DemoHostContext& ctx) override;
		void OnResetDefaults(DemoCommon::DemoHostContext& ctx) override;

	private:
		void DrawDeferredFeaturesPanel(DemoCommon::DemoHostContext& ctx);
		void DrawLightingPanel(DemoCommon::DemoHostContext& ctx);
		void DrawDebugViewPanel(DemoCommon::DemoHostContext& ctx);
	};
}
