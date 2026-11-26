// ============================================================================
// Demos/ForwardRenderDemo/ForwardRenderDemoModule.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/ForwardRenderDemo/ForwardRenderDemoModule.h"

#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "Demos/Common/DemoSceneHelpers.h"

#include "imgui.h"

namespace DemoModules
{
	void ForwardRenderDemoModule::OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation)
	{
		DemoCommon::SetupStandardRightHandedCamera(ctx, isFirstActivation);
	}

	void ForwardRenderDemoModule::Update(DemoCommon::DemoHostContext& ctx, float deltaT)
	{
		if (ctx.Camera == nullptr)
			return;
		if (ctx.WantCaptureKeyboard)
			ctx.Camera->Update();
		else if (ctx.CameraController && *ctx.CameraController)
			(*ctx.CameraController)->Update(deltaT);
	}

	void ForwardRenderDemoModule::DrawPanels(DemoCommon::DemoHostContext& ctx)
	{
		DemoCommon::DrawSceneModelPanel(ctx);

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
