// ============================================================================
// Demos/RendererDemoApp/DemoWorkstation.h
// 单一图形研究工作站底座：同时服务 4 个独立管线 Demo 与 RendererDemoApp 管理器
// ============================================================================
#pragma once

#include "00_Core/PCH.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Scene
{
	namespace Camera
	{
		class Camera;
		class CameraController;
	}
	namespace Model
	{
		class ModelInstance;
	}
}

namespace Renderer
{
	class GpuTimer;
	class RendererHost;
	struct RenderParams;
}

namespace Demo
{
	struct ModelEntry
	{
		const char*    name;
		const wchar_t* relativePath;
	};

	struct SceneModel
	{
		std::unique_ptr<Scene::Model::ModelInstance> instance;
		std::string catalogName;
		uint32_t    catalogIndex = 0;
		float       targetRadius = 1.0f;
	};

	struct HostContext
	{
		Renderer::RendererHost* Host     = nullptr;
		Renderer::RenderParams* Params   = nullptr;
		Scene::Camera::Camera*  Camera   = nullptr;
		std::unique_ptr<Scene::Camera::CameraController>* CameraController = nullptr;
		Renderer::GpuTimer*     GpuTimer = nullptr;

		std::vector<SceneModel>* SceneModels = nullptr;
		int32_t* SelectedModel         = nullptr;
		int32_t* CurrentModelIndex     = nullptr;
		float*   TargetRadius          = nullptr;
		bool*    ForceRebuildOnLoad    = nullptr;
		int32_t* PendingAddModelIndex  = nullptr;
		int32_t* PendingRemoveModel    = nullptr;
		int*     ModelComboIndex       = nullptr;

		bool     FreezeCameraForScreenshot = false;
		bool     WantCaptureKeyboard       = false;
		float    CpuFrameMs                = 0.0f;
		float    GpuFrameMs                = 0.0f;
		uint32_t LastHeapBindCount         = 0;
	};

	class IDemoModule
	{
	public:
		virtual ~IDemoModule() = default;

		virtual const char* GetDisplayName() const = 0;
		virtual std::wstring GetPipelineName() const = 0;

		virtual bool ShouldEnsureDefaultModelOnActivate() const { return true; }

		virtual void OnActivate(HostContext& ctx, bool isFirstActivation) = 0;
		virtual void OnDeactivate(HostContext& ctx) { (void)ctx; }

		virtual void Update(HostContext& ctx, float deltaT) = 0;
		virtual void DrawPanels(HostContext& ctx) = 0;
		virtual void OnResetDefaults(HostContext& ctx) { (void)ctx; }
	};

	const ModelEntry* GetModelCatalog(uint32_t& outCount);

	void ClearAllModels(HostContext& ctx);
	void AddModel(HostContext& ctx, uint32_t catalogIndex, bool forceRebuild = false);
	void RemoveModel(HostContext& ctx, int32_t sceneIndex);
	bool LoadModelByIndex(HostContext& ctx, uint32_t index, bool forceRebuild);

	// 恢复标准右手系 (+Y Up, -Z Fwd, Standard-Z) 相机与 FlyingFPSCamera 控制器
	void SetupStandardRightHandedCamera(HostContext& ctx, bool resetPose);

	// 供标准 RH 管线模块复用的 Scene / Model 面板
	void DrawSceneModelPanel(HostContext& ctx);

	// ASCII 安全窄化辅助（ImGui 默认仅加载 ASCII 字形）
	std::string NarrowAscii(const std::wstring& text);

	int RunWorkstationApp(
		const wchar_t* appTitle,
		std::vector<std::unique_ptr<IDemoModule>> modules,
		HINSTANCE hInst,
		int nCmdShow);

	template <typename TModule>
	int RunStandalone(const wchar_t* appTitle, HINSTANCE hInst, int nCmdShow)
	{
		std::vector<std::unique_ptr<IDemoModule>> mods;
		mods.push_back(std::make_unique<TModule>());
		return RunWorkstationApp(appTitle, std::move(mods), hInst, nCmdShow);
	}
}
