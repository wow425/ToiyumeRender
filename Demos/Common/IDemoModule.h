// ============================================================================
// Demos/Common/IDemoModule.h
// 渲染管线 Demo 模块抽象接口：解耦主控管理器 (RendererDemoApp) 与各具体管线实现
// ============================================================================
#pragma once

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

namespace DemoCommon
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

	struct DemoHostContext
	{
		Renderer::RendererHost* Host   = nullptr;
		Renderer::RenderParams* Params = nullptr;
		Scene::Camera::Camera*  Camera = nullptr;
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

		virtual void OnActivate(DemoHostContext& ctx, bool isFirstActivation) = 0;
		virtual void OnDeactivate(DemoHostContext& ctx) { (void)ctx; }

		virtual void Update(DemoHostContext& ctx, float deltaT) = 0;
		virtual void DrawPanels(DemoHostContext& ctx) = 0;
		virtual void OnResetDefaults(DemoHostContext& ctx) { (void)ctx; }
	};
}
