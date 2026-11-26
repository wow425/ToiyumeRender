// ============================================================================
// 模块：ForwardMainPass（前向管线主几何绘制 Pass）
// 架构层：Engine/04_Renderer/Passes（WP-2B 全管线 Pass 化，零依赖 LinearPassList）
// 核心职责：独占 DefaultVS / DefaultPS 字节码、Forward RootSignature 与 PSO 缓存，
//           消费 FrameBlackboard 核心目标与 RasterDrawHelper 排序列表完成前向绘制。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>

#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

class GraphicsContext;

namespace Renderer
{
	class FrameBlackboard;
	class RasterDrawHelper;
	class RenderServices;

	class ForwardMainPass
	{
	public:
		ForwardMainPass() = default;
		~ForwardMainPass() = default;

		bool Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, RenderServices& services);
		void Shutdown(RenderServices* services);

		void Execute(GraphicsContext& context,
			const RenderFrameDesc& frame,
			RasterDrawHelper& drawHelper,
			const FrameBlackboard& blackboard,
			DrawPass maxPass,
			BatchType batchType,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			RenderServices& services);

		const GraphicsPSO& GetOrCreatePSO(const PipelineDesc& desc);
		void BindMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const;

		bool IsInitialized() const { return m_Initialized; }

	private:
		void BuildRootSignature();
		void BuildDefaultPSO(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
		uint8_t GetPSOIndex(const PipelineDesc& desc);

		RootSignature            m_RootSig;
		GraphicsPSO              m_DefaultPSO;
		std::vector<GraphicsPSO> m_PSOCache;
		BindlessAllocation       m_CommonTextures;
		bool                     m_Initialized = false;
	};
}
