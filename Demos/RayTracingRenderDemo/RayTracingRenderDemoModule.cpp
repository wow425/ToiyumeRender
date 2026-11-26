// ============================================================================
// Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/RayTracingRenderDemo/RayTracingRenderDemoModule.h"

#include "02_RHI/GraphicsCaps.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RendererHost.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "Demos/Common/DemoSceneHelpers.h"

#include "imgui.h"

namespace DemoModules
{
	void RayTracingRenderDemoModule::OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation)
	{
		DemoCommon::SetupStandardRightHandedCamera(ctx, isFirstActivation);
	}

	void RayTracingRenderDemoModule::Update(DemoCommon::DemoHostContext& ctx, float deltaT)
	{
		if (ctx.Camera == nullptr)
			return;
		if (ctx.WantCaptureKeyboard)
			ctx.Camera->Update();
		else if (ctx.CameraController && *ctx.CameraController)
			(*ctx.CameraController)->Update(deltaT);
	}

	void RayTracingRenderDemoModule::DrawPanels(DemoCommon::DemoHostContext& ctx)
	{
		DemoCommon::DrawSceneModelPanel(ctx);
		DrawRayTracingPanel(ctx);
		DrawSunLightingPanel(ctx);
	}

	void RayTracingRenderDemoModule::OnResetDefaults(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params != nullptr)
			ctx.Params->ResetToDefaults();
	}

	void RayTracingRenderDemoModule::DrawRayTracingPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr || ctx.Host == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(1564, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(340, 560), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Ray Tracing (R7)"))
		{
			ImGui::End();
			return;
		}

		Renderer::RenderParams& params = *ctx.Params;
		ImGui::TextDisabled("Active path: DispatchRays (hard shadow + 1-bounce reflection)");
		ImGui::Separator();

		ImGui::Checkbox("RT hard shadow", &params.EnableRTShadow);
		ImGui::Checkbox("RT reflection", &params.EnableRTReflection);

		ImGui::Separator();
		ImGui::TextDisabled("Material (R7): both feed b0 root constants");
		ImGui::Checkbox("Material textures", &params.EnableMaterialTextures);
		ImGui::Checkbox("GGX (Cook-Torrance)", &params.EnableGGX);

		int rtDebug = static_cast<int>(params.RTDebugView);
		static const char* kRTDebugNames[] = { "Shaded", "Normal", "Shadow", "Reflection", "HitDistance", "BaseColor" };
		if (ImGui::Combo("Debug channel", &rtDebug, kRTDebugNames, 6))
			params.RTDebugView = static_cast<Renderer::RayTracingDebugView>(rtDebug);

		ImGui::Separator();
		ImGui::TextDisabled("Bias: normal-only, single knob");
		ImGui::PushItemWidth(150.0f);
		ImGui::SliderFloat("Normal bias (m)", &params.RTShadowNormalBias, 0.0f, 0.05f, "%.4f");
		ImGui::SliderFloat("Shadow max dist", &params.RTShadowMaxDistance, 1.0f, 500.0f, "%.0f");
		ImGui::SliderFloat("Reflect max dist", &params.RTReflectionMaxDistance, 1.0f, 500.0f, "%.0f");
		ImGui::SliderFloat("Reflect intensity", &params.RTReflectionIntensity, 0.0f, 1.0f, "%.2f");
		ImGui::PopItemWidth();

		ImGui::Separator();
		Renderer::RayTracingFrameStats s = {};
		if (auto* pipe = ctx.Host->GetActivePipeline(); pipe != nullptr && pipe->QueryRayTracingStats(s))
		{
			ImGui::Text("Rays/frame (dispatched)");
			ImGui::TextDisabled("  primary %llu   shadow %llu",
				static_cast<unsigned long long>(s.PrimaryRays), static_cast<unsigned long long>(s.ShadowRays));
			ImGui::TextDisabled("  reflect %llu   total %llu",
				static_cast<unsigned long long>(s.ReflectionRays), static_cast<unsigned long long>(s.TotalRays));
			ImGui::Text("Geometry: %u tri / %u BLAS / %u inst",
				s.TriangleCount, s.BlasCount, s.InstanceCount);
			ImGui::Text("GPU frame: %.3f ms", ctx.GpuFrameMs);
		}
		else
		{
			ImGui::TextDisabled("Stats unavailable");
		}

		ImGui::End();
	}

	void RayTracingRenderDemoModule::DrawSunLightingPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(390, 400), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(300, 260), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Material / Lighting"))
		{
			ImGui::End();
			return;
		}

		Renderer::RenderParams& params = *ctx.Params;
		ImGui::TextDisabled("DXR consumes the SUN only (point light / IBL not wired).");
		ImGui::Separator();
		ImGui::Checkbox("Sun light", &params.EnableSunLight);
		ImGui::SliderFloat("Sun intensity", &params.SunIntensity, 0.0f, 10.0f);
		ImGui::ColorEdit3("Sun color", params.SunColor);
		ImGui::DragFloat3("Sun direction", params.SunDirection, 0.01f, -1.0f, 1.0f);

		ImGui::Separator();
		if (ImGui::Button("Reset to defaults"))
			OnResetDefaults(ctx);
		ImGui::Text("Light param uploads: %llu", static_cast<unsigned long long>(Scene::LightingSystem::GetUploadCount()));
		ImGui::End();
	}
}
