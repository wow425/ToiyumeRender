#pragma once

// GpuTimer.h 会被 Demo（不属于 TooiyumeEngine 目标，拿不到强制包含的 PCH）直接 include，
// 而 ReadbackBuffer.h -> GpuBuffer.h -> GpuResource.h 这条链本身不包含任何系统头。
// 因此这里显式补上 D3D12 与 ComPtr 的定义，保证本头文件可以第一个被 include。
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <vector>

#include "02_RHI/Resource/Heap/ReadbackBuffer.h"

class GraphicsContext;

// ============================================================
// GPU 帧与逐 Pass 多区间时间测量（R3-E -> WP-3A TD-11a 升级）
// ------------------------------------------------------------
// 为什么需要它：
//   路线图 §6.3 明确禁止把 CPU 帧时间（std::chrono 量到的 Update+Render 调用耗时）
//   冒充 GPU 时间写进 Performance 面板 —— GPU 成为瓶颈时，两者可以差出十几毫秒，
//   而「到底谁在拖后腿、是哪个 Pass 拖后腿」恰恰是性能面板与 Phase 4 A/B 计时唯一要回答的问题。
//
// 多区间查询布局（每帧槽固定 kQueriesPerFrame = 2 + 2 * kMaxPassesPerFrame 个时间戳）：
//   - base + 0 / base + 1             : 整帧 Begin / End 时间戳
//   - base + 2 + 2*p / base + 3 + 2*p : 第 p 个 Pass（p < kMaxPassesPerFrame）的 Begin / End 时间戳
//   - End() 时一次性 ResolveQueryData 把本帧有效时间戳拷入 READBACK 堆该槽专属偏移；
//   - 仅当 IsFenceComplete(m_SlotFence[slot]) 为真时才 Map 读取，零 CPU 停顿、零 Debug Layer 竞态。
// ============================================================
namespace Renderer
{
	class GpuTimer
	{
	public:
		// 单个 Pass 的最近一次已完成 GPU 耗时（毫秒）
		struct PassTiming
		{
			std::string Name;
			float       GpuMs = 0.0f;
		};

		GpuTimer(void) = default;
		~GpuTimer(void) { Shutdown(); }

		// 禁止拷贝：内部持有 query heap 与回读堆，复制会变成双重释放。
		GpuTimer(const GpuTimer&) = delete;
		GpuTimer& operator=(const GpuTimer&) = delete;

		// 创建时间戳查询堆与回读堆。可重复调用（内部先 Shutdown 复位）。
		void Initialize(ID3D12Device* device, uint32_t maxFramesInFlight = 3);

		// 释放全部 GPU 资源并复位状态。可重复调用。
		void Shutdown(void);

		bool IsInitialized(void) const;

		// 关闭后 Begin 直接早退（不再插入新打点）；已经 Begin 的那一帧仍会正常 End 收尾。
		void SetEnabled(bool enabled);
		bool IsEnabled(void) const { return m_Enabled; }

		// 在渲染命令列表内调用（整帧包络）。
		void Begin(GraphicsContext& context);
		void End(GraphicsContext& context);

		// [WP-3A / TD-11a] 帧内逐 Pass 区间打点（由 LinearPassList::Execute 自动配对调用）。
		void BeginPass(GraphicsContext& context, const char* passName);
		void EndPass(GraphicsContext& context);

		// 命令列表提交之后调用，参数是 CommandContext::Finish() 的返回值（围栏值）。
		void OnSubmitted(uint64_t fenceValue);

		// 取最近一个「已完成」槽的整帧 GPU 时间（毫秒）并同步刷新逐 Pass 明细。
		bool TryGetElapsedMs(float& outMilliseconds);

		// 最近一次成功采样的整帧耗时与逐 Pass 明细（毫秒）。
		float GetLastElapsedMs(void) const { return m_LastElapsedMs; }
		const std::vector<PassTiming>& GetPassTimings(void) const { return m_LastPassTimings; }

	private:
		static constexpr uint32_t kMaxSlots          = 8;
		static constexpr uint32_t kMaxPassesPerFrame = 32;
		static constexpr uint32_t kQueriesPerFrame   = 2 + 2 * kMaxPassesPerFrame;
		static constexpr uint32_t kInvalidSlot       = 0xFFFFFFFFu;

		struct FrameSlotData
		{
			bool     InFlight      = false;
			uint64_t Fence         = 0;
			uint32_t PassCount     = 0;
			std::array<std::string, kMaxPassesPerFrame> PassNames;
		};

		Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_QueryHeap;
		ReadbackBuffer m_Readback;              // 每槽 kQueriesPerFrame 个 uint64
		uint64_t m_TimestampFrequency = 0;      // DIRECT 队列 GPU 时钟频率
		uint32_t m_SlotCount = 0;               // 实际槽数（= maxFramesInFlight）
		uint32_t m_CurrentSlot = 0;             // 下一帧 Begin 要用的槽
		uint32_t m_ActiveSlot = kInvalidSlot;   // 本帧 Begin 选中的槽，End 必须用同一个
		uint32_t m_LastEndedSlot = kInvalidSlot;// 最近一次 End 的槽，等 OnSubmitted 认领 fence
		uint32_t m_ActivePassIdx = kInvalidSlot;// 当前已 BeginPass 尚未 EndPass 的下标
		FrameSlotData m_Slots[kMaxSlots];
		float m_LastElapsedMs = 0.0f;
		std::vector<PassTiming> m_LastPassTimings;
		bool m_Initialized = false;
		bool m_Enabled = true;
	};
}
