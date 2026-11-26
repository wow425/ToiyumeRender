// ============================================================================
// Demos/DeferredRenderDemo/DeferredRenderDemoModule.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/DeferredRenderDemo/DeferredRenderDemoModule.h"

#include "00_Core/Utility/Utility.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RendererHost.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "Demos/Common/DemoSceneHelpers.h"

#include "imgui.h"

namespace DemoModules
{
	void DeferredRenderDemoModule::OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation)
	{
		DemoCommon::SetupStandardRightHandedCamera(ctx, isFirstActivation);
	}

	void DeferredRenderDemoModule::OnDeactivate(DemoCommon::DemoHostContext& ctx)
	{
		(void)ctx;
	}

	void DeferredRenderDemoModule::Update(DemoCommon::DemoHostContext& ctx, float deltaT)
	{
		if (ctx.Camera == nullptr)
			return;
		if (ctx.WantCaptureKeyboard)
			ctx.Camera->Update();
		else if (ctx.CameraController && *ctx.CameraController)
			(*ctx.CameraController)->Update(deltaT);
	}

	void DeferredRenderDemoModule::DrawPanels(DemoCommon::DemoHostContext& ctx)
	{
		DemoCommon::DrawSceneModelPanel(ctx);
		DrawDeferredFeaturesPanel(ctx);
		DrawLightingPanel(ctx);
		DrawDebugViewPanel(ctx);
	}

	void DeferredRenderDemoModule::OnResetDefaults(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params != nullptr)
			ctx.Params->ResetToDefaults();
	}

	void DeferredRenderDemoModule::DrawDeferredFeaturesPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr || ctx.Host == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(1544, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(360, 595), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Deferred Features (WP-3C)"))
		{
			ImGui::End();
			return;
		}

		Renderer::RenderParams& params = *ctx.Params;
		ImGui::TextDisabled("SM 6.6 Bindless passes registered in LinearPassList.");
		ImGui::PushItemWidth(160.0f);

		if (ImGui::CollapsingHeader("Shadow Map (Directional)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Enable Shadow##wp3c", &params.EnableShadow);
			ImGui::BeginDisabled(!params.EnableShadow);

			int alg = static_cast<int>(params.ShadowAlgorithm);
			static const char* kShadowAlgs[] = { "SimpleShadowMap (1x)", "CSM (Cascaded)" };
			if (ImGui::Combo("Algorithm##sh", &alg, kShadowAlgs, 2))
				params.ShadowAlgorithm = static_cast<uint32_t>(alg);

			static const uint32_t kResValues[] = { 1024u, 2048u, 4096u };
			static const char* kResLabels[] = { "1024", "2048", "4096" };
			int resIdx = (params.ShadowResolution <= 1024u) ? 0 : ((params.ShadowResolution <= 2048u) ? 1 : 2);
			if (ImGui::Combo("Resolution##sh", &resIdx, kResLabels, 3))
				params.ShadowResolution = kResValues[resIdx];

			ImGui::BeginDisabled(params.ShadowAlgorithm != 1u);
			int cascades = static_cast<int>(params.ShadowCascadeCount);
			if (ImGui::SliderInt("Cascades##sh", &cascades, 2, 4))
				params.ShadowCascadeCount = static_cast<uint32_t>(cascades);
			ImGui::EndDisabled();

			ImGui::SliderFloat("Normal bias##sh", &params.ShadowNormalBias, 0.0f, 0.025f, "%.4f");
			ImGui::SliderFloat("Depth bias##sh",  &params.ShadowDepthBias,  0.0f, 0.005f, "%.4f");
			ImGui::SliderFloat("PCF radius##sh",  &params.ShadowPCFRadius,  0.5f, 3.0f,   "%.2f");
			ImGui::SliderFloat("Strength##sh",    &params.ShadowStrength,   0.0f, 1.0f,   "%.2f");
			ImGui::EndDisabled();
		}

		if (ImGui::CollapsingHeader("SSAO (Screen-Space AO)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Enable SSAO##wp3c", &params.EnableSSAO);
			ImGui::BeginDisabled(!params.EnableSSAO);
			ImGui::SliderFloat("Radius (m)##ssao", &params.SSAORadius, 0.05f, 2.0f, "%.2f");
			ImGui::SliderFloat("Bias (m)##ssao",   &params.SSAOBias,   0.001f, 0.1f, "%.3f");
			ImGui::SliderFloat("Power##ssao",      &params.SSAOPower,  0.5f, 4.0f, "%.2f");
			int samples = static_cast<int>(params.SSAOSampleCount);
			if (ImGui::SliderInt("Samples##ssao", &samples, 4, 32))
				params.SSAOSampleCount = static_cast<uint32_t>(samples);
			ImGui::Checkbox("Bilateral blur (5x5)##ssao", &params.SSAOEnableBlur);
			ImGui::TextDisabled("Tip: select Debug View = AO to inspect.");
			ImGui::EndDisabled();
		}

		if (ImGui::CollapsingHeader("TAA (Temporal Anti-Aliasing)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Enable TAA##wp3c", &params.EnableTAA);
			ImGui::BeginDisabled(!params.EnableTAA);
			ImGui::SliderFloat("Blend alpha##taa",    &params.TAABlendFactor,   0.02f, 0.50f, "%.2f");
			ImGui::SliderFloat("Variance gamma##taa", &params.TAAVarianceGamma, 0.50f, 2.50f, "%.2f");
			ImGui::SliderFloat("Jitter scale##taa",   &params.TAAJitterScale,   0.00f, 1.50f, "%.2f");
			if (ImGui::Button("Reset TAA History"))
			{
				if (auto* pipe = ctx.Host->GetActivePipeline())
					pipe->ResetHistory();
			}
			ImGui::EndDisabled();
		}

		ImGui::PopItemWidth();
		ImGui::End();
	}

	void DeferredRenderDemoModule::DrawLightingPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(390, 400), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Material / Lighting"))
		{
			ImGui::End();
			return;
		}

		Renderer::RenderParams& params = *ctx.Params;
		ImGui::Checkbox("Sun light", &params.EnableSunLight);
		ImGui::SliderFloat("Sun intensity", &params.SunIntensity, 0.0f, 10.0f);
		ImGui::ColorEdit3("Sun color", params.SunColor);
		ImGui::DragFloat3("Sun direction", params.SunDirection, 0.01f, -1.0f, 1.0f);

		ImGui::Separator();
		ImGui::Checkbox("Point light", &params.EnablePointLight);
		ImGui::SliderFloat("Point intensity", &params.PointLightIntensity, 0.0f, 20.0f);
		ImGui::SliderFloat("Point range", &params.PointLightRange, 0.1f, 100.0f);
		ImGui::DragFloat3("Point position", params.PointLightPosition, 0.05f, -50.0f, 50.0f);
		ImGui::ColorEdit3("Point color", params.PointLightColor);

		ImGui::Separator();
		ImGui::Checkbox("IBL", &params.EnableIBL);
		ImGui::SliderFloat("IBL intensity", &params.IBLIntensity, 0.0f, 4.0f);

		ImGui::Separator();
		if (ImGui::Button("Reset to defaults"))
			OnResetDefaults(ctx);
		ImGui::SameLine();
		ImGui::TextDisabled("(restores lighting params)");

		ImGui::Separator();
		ImGui::Text("Light param uploads: %llu", static_cast<unsigned long long>(Scene::LightingSystem::GetUploadCount()));
		ImGui::End();
	}

	void DeferredRenderDemoModule::DrawDebugViewPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(390, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(300, 150), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Debug View"))
		{
			ImGui::End();
			return;
		}

		static const char* kDeferredViews[] = {
			"Final", "BaseColor", "Normal", "Metallic", "Roughness", "AO", "Emission", "Depth"
		};
		int idx = static_cast<int>(ctx.Params->Debug);
		if (idx < 0 || idx >= 8) idx = 0;
		if (ImGui::Combo("View", &idx, kDeferredViews, 8))
			ctx.Params->Debug = static_cast<Renderer::DebugView>(idx);
		ImGui::TextWrapped("Debug view replaces lighting output in LightingPS.");
		ImGui::End();
	}
}
