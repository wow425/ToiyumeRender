// ============================================================================
// Demos/GFL2RenderDemo/GFL2RenderDemoModule.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/GFL2RenderDemo/GFL2RenderDemoModule.h"

#include "01_Application/Display.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RendererHost.h"
#include "04_Renderer/Renderer/GFL2/GFL2CharacterPipeline.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/GFL2OrbitFPSCamera.h"
#include "05_Scene/Lighting/LightingSystem.h"
#include "Demos/Common/DemoSceneHelpers.h"

#include "imgui.h"

namespace DemoModules
{
	void GFL2RenderDemoModule::OnActivate(DemoCommon::DemoHostContext& ctx, bool isFirstActivation)
	{
		if (ctx.Params != nullptr)
		{
			if (!isFirstActivation)
			{
				for (int i = 0; i < 3; ++i)
				{
					m_SavedPrevSunDir[i]   = ctx.Params->SunDirection[i];
					m_SavedPrevSunColor[i] = ctx.Params->SunColor[i];
				}
				m_SavedPrevSunIntensity = ctx.Params->SunIntensity;
				m_HasSavedPreviousSun   = true;
			}
			for (int i = 0; i < 3; ++i)
			{
				ctx.Params->SunDirection[i] = m_Gfl2SunDir[i];
				ctx.Params->SunColor[i]     = m_Gfl2SunColor[i];
			}
			ctx.Params->SunIntensity = m_Gfl2SunIntensity;
		}

		if (ctx.Camera != nullptr && ctx.CameraController != nullptr)
		{
			const float aspect = (Graphics::g_DisplayWidth > 0)
				? (static_cast<float>(Graphics::g_DisplayHeight) / static_cast<float>(Graphics::g_DisplayWidth))
				: (9.0f / 16.0f);
			auto orbit = std::make_unique<Scene::Camera::GFL2OrbitFPSCamera>(*ctx.Camera);
			orbit->ApplyPreset(Scene::Camera::GFL2CameraPreset::Frame7206, aspect);
			*ctx.CameraController = std::move(orbit);
		}
	}

	void GFL2RenderDemoModule::OnDeactivate(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params != nullptr)
		{
			for (int i = 0; i < 3; ++i)
			{
				m_Gfl2SunDir[i]   = ctx.Params->SunDirection[i];
				m_Gfl2SunColor[i] = ctx.Params->SunColor[i];
			}
			m_Gfl2SunIntensity = ctx.Params->SunIntensity;

			if (m_HasSavedPreviousSun)
			{
				for (int i = 0; i < 3; ++i)
				{
					ctx.Params->SunDirection[i] = m_SavedPrevSunDir[i];
					ctx.Params->SunColor[i]     = m_SavedPrevSunColor[i];
				}
				ctx.Params->SunIntensity = m_SavedPrevSunIntensity;
			}
			if (static_cast<int>(ctx.Params->Debug) >= 8)
				ctx.Params->Debug = Renderer::DebugView::Final;
		}
	}

	void GFL2RenderDemoModule::Update(DemoCommon::DemoHostContext& ctx, float deltaT)
	{
		if (ctx.Camera == nullptr || ctx.CameraController == nullptr || ctx.Params == nullptr)
			return;

		if (auto* gfl2Cam = dynamic_cast<Scene::Camera::GFL2OrbitFPSCamera*>(ctx.CameraController->get()))
		{
			const auto targetMode = (ctx.Params->GFL2CameraMode == 1)
				? Scene::Camera::GFL2CameraMode::FreeFly
				: Scene::Camera::GFL2CameraMode::Orbit;
			if (gfl2Cam->GetMode() != targetMode)
				gfl2Cam->SetMode(targetMode);
		}

		if (ctx.WantCaptureKeyboard || ctx.FreezeCameraForScreenshot)
			ctx.Camera->Update();
		else if (*ctx.CameraController)
			(*ctx.CameraController)->Update(deltaT);
	}

	void GFL2RenderDemoModule::DrawPanels(DemoCommon::DemoHostContext& ctx)
	{
		DrawCharacterPipelinePanel(ctx);
		DrawLightingPanel(ctx);
		DrawDebugViewPanel(ctx);
	}

	void GFL2RenderDemoModule::OnResetDefaults(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;
		ctx.Params->ResetToDefaults();
		ctx.Params->SunDirection[0] =  0.738529f;
		ctx.Params->SunDirection[1] = -0.560575f;
		ctx.Params->SunDirection[2] = -0.374607f;
		ctx.Params->SunColor[0] = ctx.Params->SunColor[1] = ctx.Params->SunColor[2] = 0.90f;
		ctx.Params->SunIntensity = 1.0f;
	}

	void GFL2RenderDemoModule::DrawCharacterPipelinePanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Host == nullptr || ctx.Params == nullptr || ctx.Camera == nullptr)
			return;
		const auto* pipe = dynamic_cast<const Renderer::GFL2::GFL2CharacterPipeline*>(ctx.Host->GetActivePipeline());
		if (pipe == nullptr)
			return;

		Renderer::RenderParams& params = *ctx.Params;

		ImGui::SetNextWindowPos(ImVec2(1544, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(360, 720), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("GFL2 Character Pipeline (AK-12)"))
		{
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("UE5 LH Z-Up (+X Fwd/+Y Right/+Z Up) | Row-Vec | Reverse-Z");
		ImGui::PushItemWidth(160.0f);

		if (ImGui::CollapsingHeader("UE5 Camera & Presets", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const Math::Vector3 camPos = ctx.Camera->GetPosition();
			ImGui::Text("Cam Pos (UE5): (%.2f, %.2f, %.2f)",
				static_cast<float>(camPos.GetX()), static_cast<float>(camPos.GetY()), static_cast<float>(camPos.GetZ()));
			static const char* kCamModes[] = { "UE5 Orbit (LMB/RMB/Wheel)", "UE5 FreeFly (WASD+LMB)" };
			ImGui::Combo("Camera Mode##gfl2", &params.GFL2CameraMode, kCamModes, 2);

			if (ctx.CameraController)
			{
				if (auto* gfl2Cam = dynamic_cast<Scene::Camera::GFL2OrbitFPSCamera*>(ctx.CameraController->get()))
				{
					if (ImGui::Button("Frame 7206"))
						gfl2Cam->ApplyPreset(Scene::Camera::GFL2CameraPreset::Frame7206, ctx.Camera->GetAspectRatio());
					ImGui::SameLine();
					if (ImGui::Button("Face"))
						gfl2Cam->ApplyPreset(Scene::Camera::GFL2CameraPreset::FaceCloseUp, ctx.Camera->GetAspectRatio());
					ImGui::SameLine();
					if (ImGui::Button("Side"))
						gfl2Cam->ApplyPreset(Scene::Camera::GFL2CameraPreset::SideProfile, ctx.Camera->GetAspectRatio());
				}
			}
		}

		if (ImGui::CollapsingHeader("GFL2 Feature Toggles (M0-M6)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Enable Character Pipeline", &params.EnableGFL2CharacterPipeline);

			ImGui::Separator();
			ImGui::Checkbox("Shadow Map (4096 Reverse-Z)##gfl2", &params.GFL2EnableShadow);
			ImGui::SliderFloat("Shadow Strength##gfl2", &params.GFL2ShadowStrength, 0.0f, 1.0f);
			ImGui::SliderFloat("Shadow NormalBias##gfl2", &params.GFL2ShadowNormalBias, 0.0f, 0.03f, "%.4f");
			ImGui::SliderFloat("Shadow DepthBias##gfl2", &params.GFL2ShadowDepthBias, 0.0f, 0.01f, "%.5f");
			ImGui::SliderFloat("Shadow PCF Radius##gfl2", &params.GFL2ShadowPCFRadius, 0.25f, 4.0f);
			if (ImGui::Button("Reset Frame 7206 Light"))
			{
				params.SunDirection[0] =  0.738529f;
				params.SunDirection[1] = -0.560575f;
				params.SunDirection[2] = -0.374607f;
				params.SunColor[0] = params.SunColor[1] = params.SunColor[2] = 0.90f;
				params.SunIntensity = 1.0f;
			}
			ImGui::Checkbox("ReflectionProbe6 (5682 IBL)##gfl2", &params.GFL2EnableIBL);
			ImGui::Checkbox("Ramp Shading (4-Row NPR)##gfl2", &params.GFL2EnableRamp);
			ImGui::Checkbox("Skin SH DC Norm (cb2[30].w)##gfl2", &params.GFL2EnableSkinSHDCNorm);

			ImGui::Separator();
			ImGui::Checkbox("[M4] Face SDF##gfl2", &params.GFL2EnableFaceSDF);
			ImGui::SliderFloat("Face SDF Smooth##gfl2", &params.GFL2FaceSdfSmooth, 0.01f, 0.40f);
			ImGui::Checkbox("[M4] Hair AngelRing##gfl2", &params.GFL2EnableHairAniso);
			ImGui::SliderFloat("Hair Spec Intensity##gfl2", &params.GFL2HairSpecIntensity, 0.0f, 5.0f);
			ImGui::SliderFloat("Hair ViewZ Shift##gfl2", &params.GFL2HairViewZShift, 0.0f, 0.20f);
			ImGui::Checkbox("[M4] Eye 3-Layer##gfl2", &params.GFL2EnableEye);
			ImGui::Checkbox("[M4] Bangs Stencil Shadow (201)##gfl2", &params.GFL2EnableBangsStencilShadow);
			ImGui::Checkbox("[M4] Bangs Over-Eye (204)##gfl2", &params.GFL2EnableBangsOverEye);
			ImGui::SliderFloat("Bangs Over-Eye Alpha##gfl2", &params.GFL2BangsOverEyeAlpha, 0.0f, 1.0f);

			ImGui::Separator();
			ImGui::Checkbox("[M5] SmoothNormal Outline##gfl2", &params.GFL2EnableOutline);
			ImGui::SliderFloat("Outline Width Scale##gfl2", &params.GFL2OutlineWidthScale, 0.0f, 3.0f);
			ImGui::SliderFloat("Outline Z Bias##gfl2", &params.GFL2OutlineZBias, 0.0f, 10.0f);
			ImGui::Checkbox("[M5] 5x Double-Sided Transparent##gfl2", &params.GFL2EnableTransparent);

			ImGui::Separator();
			ImGui::Checkbox("[M6] Env Scene + Skybox##gfl2", &params.GFL2EnableEnvScene);
			ImGui::Checkbox("[M6] Floor Planar Reflection##gfl2", &params.GFL2EnableFloorReflection);
			ImGui::Checkbox("[M6] Additional Lights (0/1)##gfl2", &params.GFL2EnableAdditionalLights);
			ImGui::SliderFloat("AddLight Scale##gfl2", &params.GFL2AdditionalLightScale, 0.0f, 3.0f);
			ImGui::Checkbox("[M6] Reverse-Z SSAO (Char Exempt)##gfl2", &params.GFL2EnableSSAO);
			ImGui::Checkbox("Bypass SSAO Exempt##gfl2", &params.GFL2BypassSSAOExempt);
			ImGui::SliderFloat("SSAO Intensity##gfl2", &params.GFL2SSAOIntensity, 0.0f, 1.5f);
			ImGui::Checkbox("[M6] Reverse-Z TAA##gfl2", &params.GFL2EnableTAA);
			ImGui::Checkbox("[M6] Bloom Pyramid##gfl2", &params.GFL2EnableBloom);
			ImGui::SliderFloat("Bloom Threshold##gfl2", &params.GFL2BloomThreshold, 0.1f, 3.0f);
			ImGui::SliderFloat("Bloom Intensity##gfl2", &params.GFL2BloomIntensity, 0.0f, 3.0f);
			ImGui::Checkbox("[M6] 7281 3D LUT##gfl2", &params.GFL2Enable3DLUT);
			ImGui::SliderFloat("LUT Intensity##gfl2", &params.GFL2LUTIntensity, 0.0f, 1.0f);
			ImGui::SliderFloat("Vignette##gfl2", &params.GFL2VignetteIntensity, 0.0f, 1.5f);
		}

		if (ImGui::CollapsingHeader("AK-12 Sidecar Asset Inspector"))
		{
			const auto& assets = pipe->GetCharacterAssets();
			const auto& st     = assets.GetStats();
			ImGui::Text("Asset Dir: %s", DemoCommon::NarrowAscii(st.AssetDir).c_str());
			ImGui::Text("Draws: %u | Submeshes: %u | Tris: %u", st.DrawCallCount, st.UniqueSubmeshes, st.TotalTriangles);
			ImGui::Text("Mesh: %s | Verts: %u | Indices: %u",
				st.MeshLoaded ? "LOADED" : "MISSING", st.VertexCount, st.IndexCount);
			ImGui::Text("UE5 BBox: [%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f]",
				st.WorldBoundsMin.x, st.WorldBoundsMin.y, st.WorldBoundsMin.z,
				st.WorldBoundsMax.x, st.WorldBoundsMax.y, st.WorldBoundsMax.z);
			ImGui::Text("Zone A Tex: %u (sRGB=%u, Linear=%u)", st.LoadedTextureCount, st.SrgbTextureCount, st.LinearTextureCount);
			ImGui::Text("SSBO SRVs: Draw=%u | Material=%u", st.DrawDataSrvIdx, st.MaterialDataSrvIdx);

			const auto& metas = assets.GetDrawMeta();
			if (ImGui::BeginChild("GFL2DrawList", ImVec2(0, 140), true))
			{
				for (size_t i = 0; i < metas.size(); ++i)
				{
					const auto& m = metas[i];
					ImGui::Text("%02zu EID %u %-18s vs=%u fs=%u tris=%u",
						i, m.EventId, m.Slug.c_str(), m.VertexShaderId, m.FragmentShaderId, m.TriangleCount);
				}
			}
			ImGui::EndChild();
		}

		ImGui::PopItemWidth();
		ImGui::End();
	}

	void GFL2RenderDemoModule::DrawLightingPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(390, 400), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(300, 310), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Material / Lighting"))
		{
			ImGui::End();
			return;
		}

		Renderer::RenderParams& params = *ctx.Params;
		ImGui::TextDisabled("GFL2CharacterPipeline consumes UE5 Sun + 5682 IBL.");
		ImGui::Separator();
		ImGui::Checkbox("Sun light", &params.EnableSunLight);
		ImGui::SliderFloat("Sun intensity", &params.SunIntensity, 0.0f, 10.0f);
		ImGui::ColorEdit3("Sun color", params.SunColor);
		ImGui::DragFloat3("Sun direction", params.SunDirection, 0.01f, -1.0f, 1.0f);

		ImGui::Separator();
		ImGui::Checkbox("IBL", &params.EnableIBL);
		ImGui::SliderFloat("IBL intensity", &params.IBLIntensity, 0.0f, 4.0f);

		ImGui::Separator();
		if (ImGui::Button("Reset to defaults"))
			OnResetDefaults(ctx);
		ImGui::Text("Light param uploads: %llu", static_cast<unsigned long long>(Scene::LightingSystem::GetUploadCount()));
		ImGui::End();
	}

	void GFL2RenderDemoModule::DrawDebugViewPanel(DemoCommon::DemoHostContext& ctx)
	{
		if (ctx.Params == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(390, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(320, 150), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Debug View"))
		{
			ImGui::End();
			return;
		}

		static const char* kViewNames[] = {
			"0: Final", "1: BaseColor", "2: Normal", "3: Metallic", "4: Roughness", "5: AO", "6: Emission", "7: Reverse-Z Depth",
			"8: FaceSDF", "9: StencilMask", "10: SmoothNormal", "11: OutlineWeight",
			"12: RMO_Channel", "13: ShadingModelID", "14: UV0_UV1",
			"15: DirectDiffuseRamp", "16: SpecularGGXRamp", "17: RimAndIBL", "18: ShadowVisibility"
		};
		static_assert(static_cast<int>(sizeof(kViewNames) / sizeof(kViewNames[0])) == static_cast<int>(Renderer::DebugView::Count),
			"Debug View name table must match Renderer::DebugView");

		int idx = static_cast<int>(ctx.Params->Debug);
		if (ImGui::Combo("View", &idx, kViewNames, static_cast<int>(Renderer::DebugView::Count)))
			ctx.Params->Debug = static_cast<Renderer::DebugView>(idx);
		ImGui::TextWrapped("GFL2 X-Ray Debug Views (0..18).");
		ImGui::End();
	}
}
