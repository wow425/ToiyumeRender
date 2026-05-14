#include "00_Core/PCH.h"
#include "BaseRenderer.h"
#include "03_RHI/CommandSystem/CommandContext.h"
#include "05_ResourceSystem/00_Buffers/ConstantBuffers.h"
#include "10_Scene/Model.h"
#include "12_Utility/Utility.h"
#include "13_Math/VectorMath.h"


namespace Renderer
{

	void MeshSorter::AddMesh(const Mesh& mesh, float distance,
		D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
		D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
		D3D12_GPU_VIRTUAL_ADDRESS bufferPtr)
	{
		SortKey key;
		key.value = m_SortObjects.size();
		// 位掩码提取材质属性
		bool alphaBlend = (mesh.psoFlags & PSOFlags::kAlphaBlend) == PSOFlags::kAlphaBlend;
		bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;
		uint64_t depthPSO = alphaTest ? 1 : 0;

		union float_or_int { float f; uint32_t u; } dist;
		dist.f = Math::Max(distance, 0.0f);

		if (m_BatchType == kShadows)
		{
			if (alphaBlend)
				return;

			key.passID = kZPass;
			key.psoIdx = depthPSO + 4;
			key.key = dist.u;
			m_SortKeys.push_back(key.value);
			m_PassCounts[kZPass]++;
		}
		else if (mesh.psoFlags & PSOFlags::kAlphaBlend)
		{
			key.passID = kTransparent;
			key.psoIdx = mesh.pso;
			key.key = ~dist.u;
			m_SortKeys.push_back(key.value);
			m_PassCounts[kTransparent]++;
		}
		else if (alphaTest)
		{
			key.passID = kZPass;
			key.psoIdx = depthPSO;
			key.key = dist.u;
			m_SortKeys.push_back(key.value);
			m_PassCounts[kZPass]++;

			key.passID = kOpaque;
			key.psoIdx = mesh.pso + 1;
			key.key = dist.u;
			m_SortKeys.push_back(key.value);
			m_PassCounts[kOpaque]++;
		}
		else
		{
			key.passID = kOpaque;
			key.psoIdx = mesh.pso;
			key.key = dist.u;
			m_SortKeys.push_back(key.value);
			m_PassCounts[kOpaque]++;
		}

		SortObject object = { &mesh, meshCBV, materialCBV, bufferPtr };
		m_SortObjects.push_back(object);
	}



	void MeshSorter::Sort()
	{
		struct { bool operator()(uint64_t a, uint64_t b) const { return a < b; } } Cmp;
		std::sort(m_SortKeys.begin(), m_SortKeys.end(), Cmp);
	}
} // namespace Renderer



