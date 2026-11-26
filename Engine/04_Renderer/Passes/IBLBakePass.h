// ============================================================================
// 模块：IBLBakePass（环境图与 IBL 预计算 Pass 封装）
// 架构层：Engine/04_Renderer/Passes（WP-2B 全管线 Pass 化）
// 核心职责：以实例成员方式拥有 EnvironmentLightingManager（取代原 DeferredRenderer.cpp
//           文件作用域全局变量 s_EnvironmentLightingManager），负责一次性 HDR -> Cubemap /
//           Irradiance / SpecularPrefilter / BRDF LUT 烘焙，并把就绪产物登记到 FrameBlackboard。
// 所有权反序：必须在 RenderServices::Shutdown() / TextureManager::Shutdown() 之前 Shutdown()（TD-03a）。
// ============================================================================
#pragma once

#include <d3d12.h>
#include <string>

#include "04_Renderer/EnvironmentLightingManager.h"

class GraphicsContext;

namespace Renderer
{
	class FrameBlackboard;

	class IBLBakePass
	{
	public:
		IBLBakePass() = default;
		~IBLBakePass() = default;

		bool Initialize(DXGI_FORMAT sceneColorFormat, DXGI_FORMAT depthFormat, const std::wstring& hdrRelativeAssetPath);
		void Shutdown();

		// Update 阶段执行一次性 IBL 烘焙并同步 LightingSystem::SetPrefilterMaxMip()。
		void ExecuteBake(GraphicsContext& gfxContext);

		// Render 阶段将就绪的 EnvCube / Irradiance / Prefilter / BRDF LUT 登记到 FrameBlackboard。
		void PublishToBlackboard(FrameBlackboard& blackboard) const;

		bool IsReady() const { return m_EnvManager.IsReady(); }

	private:
		EnvironmentLighting::EnvironmentLightingManager m_EnvManager;
		bool m_Initialized = false;
		bool m_LoggedReady = false;
	};
}
