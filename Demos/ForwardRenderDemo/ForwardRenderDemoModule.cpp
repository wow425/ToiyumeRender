// ============================================================================
// Demos/ForwardRenderDemo/ForwardRenderDemoModule.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/ForwardRenderDemo/ForwardRenderDemoModule.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"

#include "imgui.h"

namespace DemoModules
{
	void ForwardRenderDemoModule::OnActivate(Demo::HostContext& ctx, bool isFirstActivation)
	{
		Demo::SetupStandardRightHandedCamera(ctx, isFirstActivation);
	}

	void ForwardRenderDemoModule::Update(Demo::HostContext& ctx, float deltaT)
	{
		if (ctx.Camera == nullptr)
			return;
		if (ctx.WantCaptureKeyboard)
			ctx.Camera->Update();
		else if (ctx.CameraController && *ctx.CameraController)
			(*ctx.CameraController)->Update(deltaT);
	}

	void ForwardRenderDemoModule::DrawPanels(Demo::HostContext& ctx)
	{
		Demo::DrawSceneModelPanel(ctx);

		ImGui::SetNextWindowPos(ImVec2(390, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(340, 130), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Forward Pipeline"))
		{
			ImGui::Text("Active: ForwardRenderer (SM 6.4 unlit baseColor)");
			ImGui::TextDisabled("DefaultPS outputs baseColor directly and bypasses");
			ImGui::TextDisabled("lighting, shadow, SSAO and TonemapPass.");
		}
		ImGui::End();
	}
}


#ifdef TOOIYUME_DEMO_STANDALONE
int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int cmd)
{
	return Demo::RunStandalone<DemoModules::ForwardRenderDemoModule>(L"ForwardRenderDemo", hInst, cmd);
}
#endif
