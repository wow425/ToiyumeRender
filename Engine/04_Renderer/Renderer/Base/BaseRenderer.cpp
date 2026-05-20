#include "00_Core/PCH.h"
#include "BaseRenderer.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "04_Renderer/Material/Material.h"
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include "04_Renderer/Pipeline/PipelineStateCache.h"
#include "05_Scene/Model/Model.h"

namespace Renderer
{
	DescriptorHeap s_TextureHeap;  // texture堆。存放CBV/SRV/UAV描述符堆
	DescriptorHeap s_SamplerHeap;  // sampler堆
}



namespace Renderer
{
	void  BaseRenderer::ModelSort(const Scene::Model::ModelInstance& model)
	{

		model.GatherRenderables(DefaultSorter);
		DefaultSorter.Sort();
	}

	void MeshSorter::Reset()
	{
		m_SortObjects.clear();
		m_SortKeys.clear();
		std::memset(m_PassCounts, 0, sizeof(m_PassCounts));
		m_CurrentPass = kZPass;
		m_CurrentDraw = 0;
	}

	void MeshSorter::Sort()
	{
		struct { bool operator()(uint64_t a, uint64_t b) const { return a < b; } } Cmp;
		std::sort(m_SortKeys.begin(), m_SortKeys.end(), Cmp);
	}

	// 根据网格和材质信息生成排序key，并分类到不同的渲染pass中
	void MeshSorter::AddMesh(const Scene::Model::Mesh& mesh, const Scene::Material::Material& material, float distance,
		D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
		D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
		D3D12_GPU_VIRTUAL_ADDRESS bufferPtr)
	{
		SortKey key = {};
		key.value = 0;
		key.objectIdx = static_cast<uint64_t>(m_SortObjects.size());
		// 根据几何信息和材质信息生成PSO索引
		PipelineDesc desc = {};
		desc.VertexFlags = mesh.vertexFlags;
		desc.MaterialFlags = material.Flags;
		desc.PassType = (m_BatchType == kShadows) ? RenderPassType::Shadow : RenderPassType::Forward; //

		const uint16_t psoIdx = PipelineStateCache::GetPipelineIndex(desc);
		const uint16_t depthPSO = PipelineStateCache::GetPipelineIndex(
			PipelineDesc{ desc.VertexFlags, desc.MaterialFlags, RenderPassType::Depth });

		union FloatOrInt { float f; uint32_t u; } dist;
		dist.f = distance > 0.0f ? distance : 0.0f;

		// 根据材质属性和批次类型分类
		{
			if (m_BatchType == kShadows)
			{
				if (material.IsAlphaBlend()) return;
				key.passID = kZPass;
				key.psoIdx = psoIdx;
				key.key = dist.u;
				m_SortKeys.push_back(key.value);
				m_PassCounts[kZPass]++;
			}
			else if (material.IsAlphaBlend())
			{
				key.passID = kTransparent;
				key.psoIdx = psoIdx;
				key.key = ~dist.u;
				m_SortKeys.push_back(key.value);
				m_PassCounts[kTransparent]++;
			}
			else if (material.IsAlphaTest())
			{
				key.passID = kZPass;
				key.psoIdx = depthPSO;
				key.key = dist.u;
				m_SortKeys.push_back(key.value);
				m_PassCounts[kZPass]++;

				key.passID = kOpaque;
				key.psoIdx = psoIdx;
				key.key = dist.u;
				m_SortKeys.push_back(key.value);
				m_PassCounts[kOpaque]++;
			}
			else
			{
				key.passID = kOpaque;
				key.psoIdx = psoIdx;
				key.key = dist.u;
				m_SortKeys.push_back(key.value);
				m_PassCounts[kOpaque]++;
			}
			SortObject object = { &mesh, &material, meshCBV, bufferPtr };
			m_SortObjects.push_back(object);
		}
	}


} // Renderer






