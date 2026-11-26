// ============================================================================
// Demos/GFL2RenderDemo/GFL2RenderDemoModule.h
// 《少女前线2：追放》AK-12 渲染管线 Demo 模块（可独立启动，也可由 RendererDemoApp 管理器挂载切换）
// ============================================================================
#pragma once

#include "Demos/RendererDemoApp/DemoWorkstation.h"

namespace DemoModules
{
	class GFL2RenderDemoModule final : public Demo::IDemoModule
	{
	public:
		const char* GetDisplayName() const override { return "GFL2RenderDemo"; }
		std::wstring GetPipelineName() const override { return L"GFL2CharacterPipeline"; }
		bool ShouldEnsureDefaultModelOnActivate() const override { return false; }

		void OnActivate(Demo::HostContext& ctx, bool isFirstActivation) override;
		void OnDeactivate(Demo::HostContext& ctx) override;
		void Update(Demo::HostContext& ctx, float deltaT) override;
		void DrawPanels(Demo::HostContext& ctx) override;
		void OnResetDefaults(Demo::HostContext& ctx) override;

	private:
		void DrawCharacterPipelinePanel(Demo::HostContext& ctx);
		void DrawLightingPanel(Demo::HostContext& ctx);
		void DrawDebugViewPanel(Demo::HostContext& ctx);

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
