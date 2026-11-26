// ============================================================================
// Demos/ForwardRenderDemo/ForwardRenderDemoModule.h
// 前向渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/RendererDemoApp/DemoWorkstation.h"

namespace DemoModules
{
	class ForwardRenderDemoModule final : public Demo::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "ForwardRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"ForwardRenderer"; }

		void OnActivate(Demo::HostContext& ctx, bool isFirstActivation) override;
		void Update(Demo::HostContext& ctx, float deltaT) override;
		void DrawPanels(Demo::HostContext& ctx) override;
	};
}
