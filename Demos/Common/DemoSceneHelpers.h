// ============================================================================
// Demos/Common/DemoSceneHelpers.h
// 共享场景模型目录、RH 相机/控制器工具与通用 ImGui 面板辅助
// ============================================================================
#pragma once

#include "Demos/Common/IDemoModule.h"

#include <string>

namespace DemoCommon
{
	const ModelEntry* GetModelCatalog(uint32_t& outCount);

	void ClearAllModels(DemoHostContext& ctx);
	void AddModel(DemoHostContext& ctx, uint32_t catalogIndex, bool forceRebuild = false);
	void RemoveModel(DemoHostContext& ctx, int32_t sceneIndex);
	bool LoadModelByIndex(DemoHostContext& ctx, uint32_t index, bool forceRebuild);

	// 恢复标准右手系 (+Y Up, -Z Fwd, Standard-Z) 相机与 FlyingFPSCamera 控制器
	void SetupStandardRightHandedCamera(DemoHostContext& ctx, bool resetPose);

	// 供标准 RH 管线模块（Deferred / Forward / RayTracing）复用的 Scene/Model 面板
	void DrawSceneModelPanel(DemoHostContext& ctx);

	// ASCII 安全窄化辅助（ImGui 默认仅加载 ASCII 字形）
	std::string NarrowAscii(const std::wstring& text);
}
