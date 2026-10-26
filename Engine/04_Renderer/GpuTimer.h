#pragma once

// GpuTimer.h 会被 Demo（不属于 TooiyumeEngine 目标，拿不到强制包含的 PCH）直接 include，
// 而 ReadbackBuffer.h -> GpuBuffer.h -> GpuResource.h 这条链本身不包含任何系统头。
// 因此这里显式补上 D3D12 与 ComPtr 的定义，保证本头文件可以第一个被 include。
#include <d3d12.h>
#include <wrl/client.h>

#include "02_RHI/Resource/Heap/ReadbackBuffer.h"

class GraphicsContext;

// ============================================================
// GPU 帧时间测量（R3-E）
// ------------------------------------------------------------
// 为什么需要它：
//   路线图 §6.3 明确禁止把 CPU 帧时间（std::chrono 量到的 Update+Render 调用耗时）
//   冒充 GPU 时间写进 Performance 面板 —— GPU 成为瓶颈时，两者可以差出十几毫秒，
//   而「到底谁在拖后腿」恰恰是性能面板唯一要回答的问题。
//   真正的 GPU 时间只能用 D3D12 的 TIMESTAMP 查询堆在 GPU 侧打点，本组件就是这套设施。
//
// 做法：
//   - 查询堆固定 2*N 个槽（N = maxFramesInFlight），第 i 槽用 2i / 2i+1 两个 query
//     分别记录「帧开始」「帧结束」的时间戳；
//   - End 时在命令列表里 ResolveQueryData，把这两个 uint64 拷到 READBACK 堆的第 i*16 字节
//     （不用 CommandContext::ResolveTimeStamps：它把目标偏移硬编码成 0，无法分槽）；
//   - CPU 侧每个槽各记一个 fence 值（OnSubmitted 传入 CommandContext::Finish 的返回值），
//     只有 IsFenceComplete 通过的槽才允许 Map —— 在 GPU 还在写这块内存时 Map，
//     Debug Layer 会直接报错，读到的也是垃圾数据。
//
// 限制：
//   - 必须严格按 Begin -> End -> OnSubmitted 的顺序调用：Begin/End 在渲染命令列表内，
//     OnSubmitted 在 Finish() 之后、下一帧 Begin 之前。
//   - 结果天然滞后 1~2 帧：刚 End 的那一槽要等 GPU 执行完才能读，这是无法绕开的。
//   - 时间戳查询堆只在图形队列（DIRECT）上有效，不要拿计算/拷贝上下文来调 Begin/End。
// ============================================================
namespace Renderer
{
	class GpuTimer
	{
	public:
		GpuTimer(void) = default;
		~GpuTimer(void) { Shutdown(); }

		// 禁止拷贝：内部持有 query heap 与回读堆，复制会变成双重释放。
		GpuTimer(const GpuTimer&) = delete;
		GpuTimer& operator=(const GpuTimer&) = delete;

		// 创建时间戳查询堆与回读堆。可重复调用（内部先 Shutdown 复位）。
		// 失败时断言并保持「未初始化」，不会抛异常。
		void Initialize(ID3D12Device* device, uint32_t maxFramesInFlight = 3);

		// 释放全部 GPU 资源并复位状态。可重复调用。
		void Shutdown(void);

		bool IsInitialized(void) const;

		// 关闭后 Begin 直接早退（不再插入新打点）；已经 Begin 的那一帧仍会正常 End 收尾。
		// 此前已经提交过的采样仍可以读出来。
		void SetEnabled(bool enabled);
		bool IsEnabled(void) const { return m_Enabled; }

		// 在渲染命令列表内调用。
		void Begin(GraphicsContext& context);
		void End(GraphicsContext& context);

		// 命令列表提交之后调用，参数是 CommandContext::Finish() 的返回值（围栏值）。
		void OnSubmitted(uint64_t fenceValue);

		// 取最近一个「已完成」槽的 GPU 时间（毫秒）；没有可用样本时返回 false 且不改 outMilliseconds。
		bool TryGetElapsedMs(float& outMilliseconds);

		// 最近一次成功采样的值，默认 0。
		float GetLastElapsedMs(void) const { return m_LastElapsedMs; }

	private:
		// 槽数上限：3 帧在飞是常规配置，留出余量后按 8 封顶，避免调用方传大数把定长数组写穿。
		static constexpr uint32_t kMaxSlots = 8;
		static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

		Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_QueryHeap;
		ReadbackBuffer m_Readback;              // 每槽两个 uint64：起始 / 结束时间戳
		uint64_t m_TimestampFrequency = 0;      // 图形队列的 GPU 时钟频率，Initialize 时取一次
		uint32_t m_SlotCount = 0;               // 实际槽数（= maxFramesInFlight）
		uint32_t m_CurrentSlot = 0;             // 下一帧 Begin 要用的槽
		uint32_t m_ActiveSlot = kInvalidSlot;   // 本帧 Begin 选中的槽，End 必须用同一个
		uint32_t m_LastEndedSlot = kInvalidSlot;// 最近一次 End 的槽，等 OnSubmitted 认领 fence
		bool m_SlotInFlight[kMaxSlots] = {};    // 该槽已 Resolve、还没读回
		uint64_t m_SlotFence[kMaxSlots] = {};   // 该槽提交后的围栏值；0 表示还没 OnSubmitted
		float m_LastElapsedMs = 0.0f;
		bool m_Initialized = false;             // Initialize 成功且尚未 Shutdown
		bool m_Enabled = true;                  // Performance 面板可临时关掉，省掉两次 EndQuery
	};
}
