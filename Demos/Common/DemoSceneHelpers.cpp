// ============================================================================
// Demos/Common/DemoSceneHelpers.cpp
// ============================================================================
#include "00_Core/PCH.h"
#include "Demos/Common/DemoSceneHelpers.h"

#include "00_Core/Utility/AssetRoot.h"
#include "00_Core/Utility/Utility.h"
#include "01_Application/Display.h"
#include "02_RHI/Command/CommandListManager.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Camera/CameraController.h"
#include "05_Scene/Model/Model.h"
#include "05_Scene/Model/ModelLoader.h"

#include "imgui.h"

#include <cstdio>

namespace DemoCommon
{
	namespace
	{
		const ModelEntry kCatalog[] = {
			{ "merlin",              L"Model/Merlin/merlin.gltf" },
			{ "leimiaier",           L"Model/leimiaier/leimiaierALL.gltf" },
			{ "saber_emiya",         L"Model/Saber/saber_emiya.glb" },
			{ "saberCaster3NoArmor", L"Model/saberCaster/saberCaster3NoArmor.gltf" },
			{ "ChosenSword",         L"Model/saberCaster/ChosenSword.gltf" },
			{ "KyomachiyaA",         L"Model/Kyomachiya/KyomachiyaA.gltf" },
			{ "toneriko1",           L"Model/toneriko/toneriko1.gltf" },
		};
	}

	const ModelEntry* GetModelCatalog(uint32_t& outCount)
	{
		outCount = static_cast<uint32_t>(sizeof(kCatalog) / sizeof(kCatalog[0]));
		return kCatalog;
	}

	std::string NarrowAscii(const std::wstring& text)
	{
		std::string result;
		result.reserve(text.size());
		for (wchar_t ch : text)
			result.push_back((ch < 0x80) ? static_cast<char>(ch) : '?');
		return result;
	}

	void ClearAllModels(DemoHostContext& ctx)
	{
		if (ctx.SceneModels == nullptr || ctx.SceneModels->empty())
			return;

		Graphics::g_CommandManager.IdleGPU();
		ctx.SceneModels->clear();
		if (ctx.SelectedModel)     *ctx.SelectedModel = -1;
		if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = -1;
		LOG_INFO(L"[Scene] 已清空所有模型\n");
	}

	void AddModel(DemoHostContext& ctx, uint32_t catalogIndex, bool forceRebuild)
	{
		uint32_t count = 0;
		const ModelEntry* catalog = GetModelCatalog(count);
		if (ctx.SceneModels == nullptr || catalogIndex >= count)
			return;

		const std::wstring path = Asset::Resolve(catalog[catalogIndex].relativePath);
		std::shared_ptr<Scene::Model::Model> source = Scene::Model::LoadModel(path, forceRebuild);
		if (source == nullptr)
		{
			LOG_INFO(L"[Scene] 模型加载失败：%ws\n", path.c_str());
			return;
		}

		const float radius = source->m_BoundingSphere.GetRadius();
		const float x = static_cast<float>(ctx.SceneModels->size()) * radius * 2.5f;

		ctx.SceneModels->emplace_back();
		SceneModel& entry = ctx.SceneModels->back();
		entry.catalogIndex = catalogIndex;
		entry.catalogName  = catalog[catalogIndex].name;
		entry.targetRadius = radius;
		entry.instance     = std::make_unique<Scene::Model::ModelInstance>(source);
		entry.instance->SetWorldPosition(Math::Vector3(x, 0.0f, 0.0f));

		if (ctx.SelectedModel)     *ctx.SelectedModel = static_cast<int32_t>(ctx.SceneModels->size()) - 1;
		if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = static_cast<int32_t>(catalogIndex);

		LOG_INFO(L"[Scene] 已添加模型：%hs（第 %d 个，半径 %.3f）\n",
			catalog[catalogIndex].name, static_cast<int>(ctx.SceneModels->size()), radius);
	}

	void RemoveModel(DemoHostContext& ctx, int32_t sceneIndex)
	{
		if (ctx.SceneModels == nullptr || sceneIndex < 0 || sceneIndex >= static_cast<int32_t>(ctx.SceneModels->size()))
			return;

		Graphics::g_CommandManager.IdleGPU();
		const std::string name = (*ctx.SceneModels)[static_cast<size_t>(sceneIndex)].catalogName;
		ctx.SceneModels->erase(ctx.SceneModels->begin() + sceneIndex);

		if (ctx.SelectedModel && *ctx.SelectedModel >= static_cast<int32_t>(ctx.SceneModels->size()))
			*ctx.SelectedModel = static_cast<int32_t>(ctx.SceneModels->size()) - 1;

		LOG_INFO(L"[Scene] 已删除模型：%hs（剩余 %d 个）\n", name.c_str(), static_cast<int>(ctx.SceneModels->size()));
	}

	bool LoadModelByIndex(DemoHostContext& ctx, uint32_t index, bool forceRebuild)
	{
		uint32_t count = 0;
		GetModelCatalog(count);
		if (index >= count)
			return false;

		ClearAllModels(ctx);
		AddModel(ctx, index, forceRebuild);

		if (ctx.SceneModels == nullptr || ctx.SceneModels->empty())
		{
			if (ctx.CurrentModelIndex) *ctx.CurrentModelIndex = -1;
			return false;
		}

		if (ctx.TargetRadius)    *ctx.TargetRadius = ctx.SceneModels->back().targetRadius;
		if (ctx.ModelComboIndex) *ctx.ModelComboIndex = static_cast<int>(index);
		return true;
	}

	void SetupStandardRightHandedCamera(DemoHostContext& ctx, bool resetPose)
	{
		if (ctx.Camera == nullptr || ctx.CameraController == nullptr)
			return;

		const float aspect = (Graphics::g_DisplayWidth > 0)
			? (static_cast<float>(Graphics::g_DisplayHeight) / static_cast<float>(Graphics::g_DisplayWidth))
			: (9.0f / 16.0f);

		ctx.Camera->SetLeftHanded(false);
		ctx.Camera->ReverseZ(false);
		if (resetPose || ctx.CameraController->get() == nullptr)
		{
			ctx.Camera->SetEyeAtUp(
				Math::Vector3(0.0f, 0.0f, 5.0f),
				Math::Vector3(0.0f, 0.0f, -5.0f),
				Math::Vector3(Math::kYUnitVector));
			ctx.Camera->SetZRange(0.1f, 1000.0f);
		}
		ctx.Camera->SetAspectRatio(aspect);
		ctx.Camera->Update();

		if (resetPose || dynamic_cast<Scene::Camera::FlyingFPSCamera*>(ctx.CameraController->get()) == nullptr)
		{
			ctx.CameraController->reset(new Scene::Camera::FlyingFPSCamera(*ctx.Camera, Math::Vector3(Math::kYUnitVector)));
		}
	}

	void DrawSceneModelPanel(DemoHostContext& ctx)
	{
		if (ctx.SceneModels == nullptr || ctx.ModelComboIndex == nullptr ||
			ctx.ForceRebuildOnLoad == nullptr || ctx.PendingAddModelIndex == nullptr ||
			ctx.PendingRemoveModel == nullptr || ctx.SelectedModel == nullptr)
			return;

		ImGui::SetNextWindowPos(ImVec2(16, 214), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Scene / Model"))
		{
			ImGui::End();
			return;
		}

		uint32_t modelCount = 0;
		const ModelEntry* catalog = GetModelCatalog(modelCount);
		std::vector<const char*> names;
		names.reserve(modelCount);
		for (uint32_t i = 0; i < modelCount; ++i)
			names.push_back(catalog[i].name);

		ImGui::Combo("Catalog", ctx.ModelComboIndex, names.data(), static_cast<int>(names.size()));
		ImGui::Checkbox("Force rebuild (.ty)", ctx.ForceRebuildOnLoad);

		if (ImGui::Button("Add to scene"))
			*ctx.PendingAddModelIndex = *ctx.ModelComboIndex;
		ImGui::SameLine();
		ImGui::TextDisabled("(adds at frame start)");

		ImGui::Separator();
		ImGui::Text("Scene models: %d", static_cast<int>(ctx.SceneModels->size()));

		if (ctx.SceneModels->empty())
		{
			ImGui::TextDisabled("(empty - use Add to scene)");
		}
		else
		{
			for (int i = 0; i < static_cast<int>(ctx.SceneModels->size()); ++i)
			{
				const SceneModel& item = (*ctx.SceneModels)[static_cast<size_t>(i)];
				char label[128] = {};
				snprintf(label, sizeof(label), "%d: %s##sm%d", i, item.catalogName.c_str(), i);
				if (ImGui::Selectable(label, *ctx.SelectedModel == i))
					*ctx.SelectedModel = i;
			}

			ImGui::BeginDisabled(*ctx.SelectedModel < 0 || *ctx.SelectedModel >= static_cast<int>(ctx.SceneModels->size()));
			if (ImGui::Button("Remove selected"))
				*ctx.PendingRemoveModel = *ctx.SelectedModel;
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Clear all"))
				*ctx.PendingRemoveModel = -2;
		}

		ImGui::Separator();
		if (*ctx.SelectedModel >= 0 && *ctx.SelectedModel < static_cast<int>(ctx.SceneModels->size()))
		{
			SceneModel& item = (*ctx.SceneModels)[static_cast<size_t>(*ctx.SelectedModel)];
			ImGui::Text("Selected: %s", item.catalogName.c_str());
			ImGui::Text("Bounding radius: %.3f", item.instance ? static_cast<float>(item.instance->GetRadius()) : 0.0f);

			Math::Vector3 pos = item.instance ? item.instance->GetWorldPosition() : Math::Vector3(Math::kOrigin);
			float xyz[3] = { static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()) };
			if (ImGui::DragFloat3("Position", xyz, 0.02f, -100.0f, 100.0f))
			{
				if (item.instance)
					item.instance->SetWorldPosition(Math::Vector3(xyz[0], xyz[1], xyz[2]));
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset pos") && item.instance)
				item.instance->SetWorldPosition(Math::Vector3(Math::kOrigin));

			if (ImGui::DragFloat("Target radius", &item.targetRadius, 0.01f, 0.05f, 50.0f))
			{
				if (item.instance)
					item.instance->Resize(item.targetRadius);
			}
		}
		else
		{
			ImGui::TextDisabled("Select a model above to edit position/scale.");
		}

		ImGui::End();
	}
}
