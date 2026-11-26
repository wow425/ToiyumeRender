// ============================================================================
// 模块：AccelerationStructureManager（DXR 加速结构生命周期管理器 · 实现）
// 架构层：Engine/04_Renderer
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/AccelerationStructureManager.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "05_Scene/Model/Model.h"

namespace Renderer
{
	bool AccelerationStructureManager::GeometryChanged(
		const std::vector<Scene::Model::ModelInstance*>& models) const
	{
		std::vector<uint64_t> signature;
		signature.reserve(models.size() * 3);

		for (const Scene::Model::ModelInstance* instance : models)
		{
			if (instance == nullptr || instance->IsNull())
				continue;

			const Scene::Model::Model* model = instance->GetModel();
			if (model == nullptr)
				continue;

			signature.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model)));
			signature.push_back(static_cast<uint64_t>(model->m_DataBuffer.GetVersionID()));
			signature.push_back(static_cast<uint64_t>(model->m_NumMeshes));
		}

		return signature != m_GeometrySignature;
	}

	void AccelerationStructureManager::Clear(bool idleGpu)
	{
		if (idleGpu && Graphics::g_Device != nullptr && (m_GeometryReady || m_Scene.IsValid()))
		{
			Graphics::g_CommandManager.IdleGPU();
		}
		m_Scene.Destroy();
		m_GeometryReady = false;
		m_GeometrySignature.clear();
	}

	void AccelerationStructureManager::SyncScene(GraphicsContext& gfxContext,
		const std::vector<Scene::Model::ModelInstance*>& models,
		ID3D12StateObjectProperties* stateObjectProps)
	{
		// [TD-09c] 场景模型被清空时释放旧加速结构，避免继续引用已销毁模型
		if (models.empty())
		{
			if (m_GeometryReady || m_Scene.IsValid())
			{
				Clear(/*idleGpu=*/true);
				LOG_INFO(L"[DXR] 场景模型为空：已释放旧加速结构，下一帧将清屏输出\n");
			}
			return;
		}

		if (GeometryChanged(models) || !m_GeometryReady)
		{
			if (!m_Scene.RebuildGeometry(gfxContext, models))
			{
				LOG_INFO(L"[DXR] RebuildGeometry 失败，本帧跳过 DispatchRays\n");
				m_GeometryReady = false;
				return;
			}

			m_GeometrySignature.clear();
			for (const Scene::Model::ModelInstance* instance : models)
			{
				if (instance == nullptr || instance->IsNull())
					continue;
				const Scene::Model::Model* model = instance->GetModel();
				if (model == nullptr)
					continue;
				m_GeometrySignature.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model)));
				m_GeometrySignature.push_back(static_cast<uint64_t>(model->m_DataBuffer.GetVersionID()));
				m_GeometrySignature.push_back(static_cast<uint64_t>(model->m_NumMeshes));
			}
			m_GeometryReady = true;

			if (stateObjectProps != nullptr)
			{
				m_Scene.BuildShaderBindingTable(stateObjectProps, L"RayGen", L"Miss", L"ShadowMiss", L"HitGroup");
			}

			m_Scene.UpdateInstances(gfxContext, models);

			LOG_INFO(L"[DXR] 加速结构就绪：BLAS=%u TLAS实例=%u 三角=%u\n",
				m_Scene.GetBlasCount(), m_Scene.GetInstanceCount(), m_Scene.GetTriangleCount());
		}
		else
		{
			m_Scene.UpdateInstances(gfxContext, models);
		}
	}
}
