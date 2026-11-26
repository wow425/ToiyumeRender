// ============================================================================
// Demos/GFL2RenderDemo/GFL2RenderDemoModule.h
// 《少女前线2：追放》AK-12 渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/Common/IDemoModule.h"

namespace DemoModules
{
	class GFL2RenderDemoModule final : public DemoCommon::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "GFL2RenderDemo"; }
		std::wstring GetPipelineName() const override { return L"GFL2CharacterPipeline"; }
		bool ShouldEnsureDefaultModelOnActivate() const override { return false; }

		void OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation) override;
		void OnDeactivate(DemoCommon::DemoHostContext& ctx) override;
		void Update(DemoCommon::DemoHostContext& ctx, float deltaT) override;
		void DrawPanels(DemoCommon::DemoHostContext& ctx) override;
		void OnResetDefaults(DemoCommon::DemoHostContext& ctx) override;

	private:
		void DrawCharacterPipelinePanel(DemoCommon::DemoHostContext& ctx);
		void DrawLightingPanel(DemoCommon::DemoHostContext& ctx);
		void DrawDebugViewPanel(DemoCommon::DemoHostContext& ctx);

	private:
		bool  m_HasSavedPreviousSun  = false;
		float m_SavedPrevSunDir[3]   = { 0.3f, -1.0f, -0.5f };
		float m_SavedPrevSunColor[3] = { 1.0f, 1.0f, 1.0f };
		float m_SavedPrevSunIntensity = 3.0f;

		float m_Gfl2SunDir[3]   = { 0.738529f, -0.560575f, -0.374607f };
		float m_Gfl2SunColor[3] = { 0.90f, 0.90f, 0.90f };
		float m_Gfl2SunIntensity = 1.0f;
	};
}
