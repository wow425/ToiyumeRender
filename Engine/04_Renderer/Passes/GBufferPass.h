// ============================================================================
// 模块：GBufferPass（延迟渲染几何与材质 MRT 写入 Pass）
// 架构层：Engine/04_Renderer/Passes（WP-2B 全管线 Pass 化，零依赖 LinearPassList）
// 核心职责：独占 StaticMeshVS / GBufferPS 字节码、GBuffer RootSignature 与动态 PSO 缓存；
//           每帧清屏并向 4 张 GBuffer MRT + SceneDepth 写入场景几何、法线、材质与自发光。
// 线程与同步前提：单线程主循环调用；依赖 RenderServices 的 4 分区单可见堆（整帧绑堆 <= 1）。
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
namespace Math { class Matrix4; }

namespace Renderer
{
	class FrameBlackboard;
	class RasterDrawHelper;
	class RenderServices;

	class GBufferPass
	{
	public:
		GBufferPass() = default;
		~GBufferPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown(RenderServices* services);

		// 执行 GBuffer 清屏、绑定 MRT+DSV、上传 GlobalConstants 并通过 drawHelper 录制全部绘制。
		void Execute(GraphicsContext& context,
			const RenderFrameDesc& frame,
			RasterDrawHelper& drawHelper,
			const FrameBlackboard& blackboard,
			DrawPass maxPass,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			RenderServices& services,
			const Math::Matrix4* viewProjOverride = nullptr);

		const GraphicsPSO& GetOrCreatePSO(const PipelineDesc& desc);
		void BindMaterial(GraphicsContext& context, const Scene::Material::Material& material, RenderServices& services) const;

		bool IsInitialized() const { return m_Initialized; }

	private:
		void BuildRootSignature();
		void BuildDefaultPSO();
		uint8_t GetPSOIndex(const PipelineDesc& desc);

		RootSignature            m_RootSig;
		GraphicsPSO              m_DefaultPSO;
		std::vector<GraphicsPSO> m_PSOCache;
		BindlessAllocation       m_CommonTextures;
		bool                     m_Initialized = false;
	};
}
