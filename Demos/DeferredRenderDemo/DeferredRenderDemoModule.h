// ============================================================================
// Demos/DeferredRenderDemo/DeferredRenderDemoModule.h
// 延迟渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/RendererDemoApp/DemoWorkstation.h"

namespace DemoModules
{
	class DeferredRenderDemoModule final : public Demo::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "DeferredRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"DeferredRenderer"; }

		void OnActivate(Demo::HostContext& ctx, bool isFirstActivation) override;
		void OnDeactivate(Demo::HostContext& ctx) override;
		void Update(Demo::HostContext& ctx, float deltaT) override;
		void DrawPanels(Demo::HostContext& ctx) override;
		void OnResetDefaults(Demo::HostContext& ctx) override;

	private:
		void DrawDeferredFeaturesPanel(Demo::HostContext& ctx);
		void DrawLightingPanel(Demo::HostContext& ctx);
		void DrawDebugViewPanel(Demo::HostContext& ctx);
	};
}
