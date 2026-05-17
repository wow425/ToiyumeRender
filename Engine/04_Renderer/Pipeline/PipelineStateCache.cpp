#include "00_Core/PCH.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "00_Core/Base/assert.h"

namespace Renderer
{
	std::vector<PipelineDesc> PipelineStateCache::s_PipelineDescs;

	uint16_t PipelineStateCache::GetPipelineIndex(const PipelineDesc& desc)
	{
		ASSERT(s_PipelineDescs.size() < 0xffff, "Ran out of room for runtime pipeline descriptions");

		for (uint32_t i = 0; i < s_PipelineDescs.size(); ++i)
		{
			if (s_PipelineDescs[i] == desc)
				return static_cast<uint16_t>(i);
		}

		s_PipelineDescs.push_back(desc);
		return static_cast<uint16_t>(s_PipelineDescs.size() - 1);
	}

	const PipelineDesc& PipelineStateCache::GetPipelineDesc(uint16_t index)
	{
		ASSERT(index < s_PipelineDescs.size());
		return s_PipelineDescs[index];
	}

	void PipelineStateCache::Reset()
	{
		s_PipelineDescs.clear();
	}
} // namespace Renderer
