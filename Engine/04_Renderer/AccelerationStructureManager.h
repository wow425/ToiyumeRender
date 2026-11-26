// ============================================================================
// 模块：AccelerationStructureManager（DXR 加速结构生命周期与签名变更管理器）
// 架构层：Engine/04_Renderer（契约 4 / WP-2B）
// 核心职责：托管 RaytracingScene 的 BLAS/TLAS/SBT 生命周期，按模型指针+几何缓冲版本号+Mesh 数
//           检测静态几何变更（仅变更时重建 BLAS+SBT，常规帧仅更新 TLAS 实例变换），并在场景清空时
//           安全释放旧加速结构（TD-09c）。
// 同步不变量：销毁加速结构前必须 IdleGPU()，确保 GPU 无在途 TraceRay/BuildRaytracingAS。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "04_Renderer/Renderer/RayTracing/RaytracingScene.h"

class GraphicsContext;
struct ID3D12StateObjectProperties;

namespace Scene::Model { class ModelInstance; }

namespace Renderer
{
	class AccelerationStructureManager
	{
	public:
		AccelerationStructureManager() = default;
		~AccelerationStructureManager() = default;

		// 每帧 Update 阶段调用：检测几何签名并在需要时重建 BLAS + SBT，或仅刷新 TLAS 实例变换。
		void SyncScene(GraphicsContext& gfxContext,
			const std::vector<Scene::Model::ModelInstance*>& models,
			ID3D12StateObjectProperties* stateObjectProps);

		// 清空加速结构与几何签名（退出或场景全部模型被删除时调用）
		void Clear(bool idleGpu = true);

		bool IsReady() const { return m_GeometryReady && m_Scene.IsValid(); }
		const RayTracing::RaytracingScene& GetScene() const { return m_Scene; }
		RayTracing::RaytracingScene& GetScene() { return m_Scene; }

	private:
		bool GeometryChanged(const std::vector<Scene::Model::ModelInstance*>& models) const;

		RayTracing::RaytracingScene m_Scene;
		std::vector<uint64_t>       m_GeometrySignature;
		bool                        m_GeometryReady = false;
	};
}
