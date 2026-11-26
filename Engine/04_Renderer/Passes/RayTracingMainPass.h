// ============================================================================
// 模块：RayTracingMainPass（DXR 主射线 / 硬阴影 / 单次反射 DispatchRays Pass）
// 架构层：Engine/04_Renderer/Passes（WP-2B 全管线 Pass 化，零依赖 LinearPassList）
// 核心职责：独占 RayTracing.h DXIL 库字节码、DXR 全局根签名、RTPSO 与 Zone A 输出 UAV 槽；
//           消费 AccelerationStructureManager + FrameBlackboard 录制 DispatchRays。
// DX12 铁律：DispatchRays 走 Compute 根参数槽位，发射线前必须 FlushResourceBarriers()。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>

#include "02_RHI/Pipeline/RaytracingPSO.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

class ColorBuffer;
class GraphicsContext;

namespace Renderer
{
	class AccelerationStructureManager;
	class FrameBlackboard;
	class RenderServices;

	class RayTracingMainPass
	{
	public:
		RayTracingMainPass() = default;
		~RayTracingMainPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown(RenderServices* services);

		// 输出目标创建或 Resize 后刷新 Zone A 的 UAV 描述符。
		void RefreshOutputUav(ColorBuffer* outputColor, RenderServices& services);

		ID3D12StateObjectProperties* GetStateObjectProperties() const { return m_StateObject.GetProperties(); }
		bool IsReady() const { return m_Initialized && m_StateObjectReady; }
		uint32_t GetOutputUavHeapIndex() const { return m_OutputUavAlloc.HeapIndex; }
		uint32_t GetDispatchedFrameCount() const { return m_DispatchedFrames; }

		void Execute(GraphicsContext& context,
			const RenderFrameDesc& frame,
			AccelerationStructureManager& asManager,
			const FrameBlackboard& blackboard,
			uint32_t width,
			uint32_t height,
			RenderServices& services,
			RayTracingFrameStats& outStats);

	private:
		bool BuildRootSignature();
		bool BuildStateObject();

		RootSignature             m_GlobalRootSig;
		RayTracing::RaytracingPSO m_StateObject;
		BindlessAllocation        m_OutputUavAlloc;

		bool     m_Initialized = false;
		bool     m_StateObjectReady = false;
		uint32_t m_DispatchedFrames = 0;

		bool m_LastEnableShadow = false;
		bool m_LastEnableReflection = false;
		bool m_LastEnableMaterialTextures = false;
		bool m_LastEnableGGX = false;
	};
}
