// ============================================================================
// Demos/ForwardRenderDemo/ForwardRenderDemoModule.h
// 前向渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/Common/IDemoModule.h"

namespace DemoModules
{
	class ForwardRenderDemoModule final : public DemoCommon::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "ForwardRenderDemo"; }
		std::wstring GetPipelineName() const override { return L"ForwardRenderer"; }

		void OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation) override;
		void Update(DemoCommon::DemoHostContext& ctx, float deltaT) override;
		void DrawPanels(DemoCommon::DemoHostContext& ctx) override;
	};
}
