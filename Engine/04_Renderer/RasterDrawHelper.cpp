// ============================================================================
// 模块：RasterDrawHelper（光栅绘制排序与提交辅助器 · 实现）
// 架构层：Engine/04_Renderer（契约 4）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/RasterDrawHelper.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "05_Scene/Model/Model.h"

namespace Renderer
{
	RasterDrawHelper::RasterDrawHelper(BatchType batchType)
		: m_Sorter(batchType)
	{
	}

	void RasterDrawHelper::Reset()
	{
		m_Sorter.Reset();
	}

	void RasterDrawHelper::AddModel(const Scene::Model::ModelInstance& model)
	{
		model.GatherRenderables(m_Sorter);
	}

	void RasterDrawHelper::Sort()
	{
		m_Sorter.Sort();
	}

	void RasterDrawHelper::SortScene(const std::vector<Scene::Model::ModelInstance*>& models)
	{
		m_Sorter.Reset();
		for (const Scene::Model::ModelInstance* model : models)
		{
			if (model != nullptr)
			{
				model->GatherRenderables(m_Sorter);
			}
		}
		m_Sorter.Sort();
	}

	void RasterDrawHelper::Draw(GraphicsContext& context, DrawPass maxPass,
		const ResolvePsoFn& resolvePso,
		const BindMaterialFn& bindMaterial)
	{
		DrawPass currentPass = m_Sorter.GetCurrentPass();
		const uint32_t* passCounts = m_Sorter.GetPassCounts();
		uint32_t currentDraw = m_Sorter.GetCurrentDraw();
		const std::vector<uint64_t> sortKeys = m_Sorter.GetSortkeys();
		const std::vector<MeshSorter::SortObject> sortObjects = m_Sorter.GetSortObjects();

		for (; currentPass <= maxPass; currentPass = static_cast<DrawPass>(currentPass + 1))
		{
			const uint32_t passCount = passCounts[currentPass];
			if (passCount == 0)
				continue;

			const uint32_t lastDraw = currentDraw + passCount;
			while (currentDraw < lastDraw)
			{
				MeshSorter::SortKey key;
				key.value = sortKeys[currentDraw];
				const MeshSorter::SortObject& object = sortObjects[key.objectIdx];
				const Scene::Model::Mesh& mesh = *object.mesh;
				const Scene::Material::Material& material = *object.material;
				const PipelineDesc& desc = PipelineStateCache::GetPipelineDesc(static_cast<uint16_t>(key.psoIdx));

				context.SetConstantBuffer(kMeshConstants, object.meshCBV);
				bindMaterial(context, material);
				context.SetPipelineState(resolvePso(desc));

				context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
				context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, static_cast<DXGI_FORMAT>(mesh.ibFormat) });

				for (uint32_t i = 0; i < mesh.numDraws; ++i)
				{
					context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
				}
				++currentDraw;
			}
		}
	}
}
