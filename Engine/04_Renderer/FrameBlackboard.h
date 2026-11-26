// ============================================================================
// 模块：FrameBlackboard（CoreFrameTargets + 零堆分配强类型帧资源黑板）
// 架构层：Engine/04_Renderer（契约 3）
// 核心职责：在各独立 Pass 之间传递常驻核心渲染目标与可选中间通道，彻底切断 Pass 间
//           的硬编码成员变量耦合；为 Phase 3 三阶段 LinearPassList 零重写接入做好契约准备。
// 线程与性能前提：仅单线程帧录制期使用；固定数组映射，零堆分配、零 RTTI、O(1) 存取。
// ============================================================================
#pragma once

#include <array>
#include <cstdint>

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "04_Renderer/PrimarySurfaceDesc.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

namespace Renderer
{
	// 常驻核心帧目标：三条主渲染管线（Forward / Deferred / DXR）共享的输出契约。
	// 注：WP-2A/2B 保持 0 像素差，LinearHDRColor 尚未激活；WP-2C 起统一激活 R16G16B16A16_FLOAT。
	struct CoreFrameTargets
	{
		DepthBuffer* SceneDepth     = nullptr;
		ColorBuffer* LinearHDRColor = nullptr;
		ColorBuffer* SceneColorLDR  = nullptr;
	};

	// 可选帧内中间通道强类型键（契约 3：零字符串哈希、零动态分配）
	enum class FrameResourceKey : uint8_t
	{
		GBuffer_BaseColor = 0,
		GBuffer_Normal,
		GBuffer_Material,
		GBuffer_Emission,

		SceneDepth,
		LinearHDRColor,
		SceneColorLDR,
		Velocity,

		EnvCube,
		IBL_Irradiance,
		IBL_Prefilter,
		IBL_BRDFLUT,

		ShadowMap,
		SSAORaw,
		SSAOMap,
		TAAHistoryPrev,
		TAAOutput,
		IndirectLinearHDR,
		DenoiseHistory,

		Count
	};

	// 黑板单条记录：同时携带裸资源指针与统一单可见堆中的 Bindless 下标（面向 WP-3A SM 6.6）
	struct FrameResourceEntry
	{
		GpuResource* Resource       = nullptr;
		uint32_t     SrvBindlessIdx = kInvalidBindlessIndex;
		uint32_t     UavBindlessIdx = kInvalidBindlessIndex;

		bool IsValid() const { return Resource != nullptr; }
	};

	// 每帧开始 Reset() 后由管线和上游 Pass 逐一登记，下游 Pass 仅按枚举键按需读取。
	class FrameBlackboard
	{
	public:
		CoreFrameTargets   Core;
		// [契约 4] 光栅 GBufferPass 与 DXR PrimaryRayPass 统一产出的主可见性与表面几何契约
		PrimarySurfaceDesc PrimarySurface;

		void Reset()
		{
			Core = {};
			PrimarySurface.Reset();
			m_Entries.fill(FrameResourceEntry{});
		}

		void Set(FrameResourceKey key, GpuResource* resource,
			uint32_t srvBindlessIdx = kInvalidBindlessIndex,
			uint32_t uavBindlessIdx = kInvalidBindlessIndex)
		{
			const size_t idx = static_cast<size_t>(key);
			if (idx < m_Entries.size())
			{
				m_Entries[idx] = FrameResourceEntry{ resource, srvBindlessIdx, uavBindlessIdx };
			}
		}

		const FrameResourceEntry& Get(FrameResourceKey key) const
		{
			const size_t idx = static_cast<size_t>(key);
			return m_Entries[idx];
		}

		bool Has(FrameResourceKey key) const
		{
			const size_t idx = static_cast<size_t>(key);
			return idx < m_Entries.size() && m_Entries[idx].Resource != nullptr;
		}

		ColorBuffer* GetColorBuffer(FrameResourceKey key) const
		{
			const size_t idx = static_cast<size_t>(key);
			return idx < m_Entries.size() ? static_cast<ColorBuffer*>(m_Entries[idx].Resource) : nullptr;
		}

		DepthBuffer* GetDepthBuffer(FrameResourceKey key) const
		{
			const size_t idx = static_cast<size_t>(key);
			return idx < m_Entries.size() ? static_cast<DepthBuffer*>(m_Entries[idx].Resource) : nullptr;
		}

	private:
		std::array<FrameResourceEntry, static_cast<size_t>(FrameResourceKey::Count)> m_Entries{};
	};
}
