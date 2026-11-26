// ============================================================================
// 模块：RasterDrawHelper（光栅绘制排序与提交辅助器）
// 架构层：Engine/04_Renderer（契约 4 / TD-01d / TD-09d）
// 核心职责：托管 MeshSorter 的每帧场景收集、排序与 DrawIndexed 录制循环，
//           将光栅管线专用状态从 BaseRenderer 中剥离，使 Forward / Deferred / ShadowPass
//           可复用同一套绘制骨架而不污染 DXR 等非光栅管线。
// 线程前提：仅单线程主循环 Update/Render 阶段调用。
// ============================================================================
#pragma once

#include <functional>
#include <vector>

#include "04_Renderer/Renderer/Base/BaseRenderer.h"

namespace Renderer
{
	class RasterDrawHelper
	{
	public:
		using ResolvePsoFn    = std::function<const GraphicsPSO&(const PipelineDesc&)>;
		using BindMaterialFn  = std::function<void(GraphicsContext&, const Scene::Material::Material&)>;

		explicit RasterDrawHelper(BatchType batchType = BatchType::kDefault);

		// 每帧清空并重新收集场景内全部有效模型的渲染项（仅 CPU 遍历与排序，零 GPU 开销）。
		void Reset();
		void AddModel(const Scene::Model::ModelInstance& model);
		void Sort();

		// 一次性完成 Reset -> 遍历全部模型收集 -> Sort 的便捷入口。
		void SortScene(const std::vector<Scene::Model::ModelInstance*>& models);

		// 按排序结果录制直到 maxPass（含）的全部 DrawIndexed 调用；
		// 通过回调由调用方解析本 Pass 专属 PSO 与材质槽绑定，保持零硬编码耦合。
		void Draw(GraphicsContext& context, DrawPass maxPass,
			const ResolvePsoFn& resolvePso,
			const BindMaterialFn& bindMaterial);

		const MeshSorter& GetSorter() const { return m_Sorter; }

	private:
		MeshSorter m_Sorter;
	};
}
