#pragma once


#include "04_Renderer/Pipeline/PipelineDesc.h"


namespace Renderer
{
	// 该类存储了所有运行时生成的 PipelineDesc，并为每个 PipelineDesc 分配一个 uint16_t 的索引。
	// Pipeline Manager (PSO Cache) 通过这个索引来查询对应的 PipelineDesc，从而知道如何创建 PSO。
	class PipelineStateCache
	{
	public:
		static uint16_t GetPipelineIndex(const PipelineDesc& desc);
		static const PipelineDesc& GetPipelineDesc(uint16_t index);
		static void Reset();

	private:
		// 存储pipelinedesc数组
		static std::vector<PipelineDesc> s_PipelineDescs;
	};
}  // namespace Renderer
