#include "00_Core/PCH.h"
#include "04_Renderer/GpuTimer.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/Heap/ReadbackBuffer.h"
#include "00_Core/Utility/Utility.h"

// ============================================================
// GpuTimer 实现说明（R3-E -> WP-3A TD-11a 多区间逐 Pass 计时）
// ------------------------------------------------------------
// 一次采样由三段组成，缺一段都会读到垃圾数据：
//   1) Begin / [BeginPass..EndPass]* / End：
//      槽 s 的查询基址 base = s * kQueriesPerFrame：
//        - base + 0 / base + 1         : 整帧起止时间戳
//        - base + 2 + 2*p / +3 + 2*p   : 第 p 个 Pass 的起止时间戳
//      End() 一次性 ResolveQueryData 把 2 + 2*passCount 个 uint64 拷入回读堆该槽偏移。
//   2) OnSubmitted：命令列表提交后把 Finish() 返回的围栏值记到该槽。
//   3) TryGetElapsedMs：仅当 IsFenceComplete(fence) 为真时才 Map 回读堆，
//      同时解析整帧总耗时与每个 Pass 的毫秒明细，零 CPU 停顿、零 Debug Layer 竞态。
// ============================================================
namespace Renderer
{
	namespace
	{
		constexpr uint32_t kBytesPerQuery = static_cast<uint32_t>(sizeof(uint64_t));
	}

	void GpuTimer::Initialize(ID3D12Device* device, uint32_t maxFramesInFlight)
	{
		Shutdown();

		if (device == nullptr)
		{
			ASSERT(false, "GpuTimer::Initialize 收到空的 ID3D12Device，GPU 计时不可用");
			return;
		}

		if (maxFramesInFlight == 0)
			maxFramesInFlight = 1;
		if (maxFramesInFlight > kMaxSlots)
			maxFramesInFlight = kMaxSlots;

		const uint32_t totalQueries = kQueriesPerFrame * maxFramesInFlight;

		// 1) 时间戳查询堆：整帧 2 槽 + 最多 32 个 Pass × 2 槽
		D3D12_QUERY_HEAP_DESC QueryHeapDesc = {};
		QueryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		QueryHeapDesc.Count = totalQueries;
		QueryHeapDesc.NodeMask = 0;

		HRESULT hr = device->CreateQueryHeap(&QueryHeapDesc, TY_IID_PPV_ARGS(&m_QueryHeap));
		if (FAILED(hr))
		{
			ASSERT(false, "GpuTimer::Initialize 创建 TIMESTAMP 查询堆失败，GPU 计时不可用");
			Shutdown();
			return;
		}

		// 2) 回读堆：供 ResolveQueryData 落地与 CPU 侧 Map 读取
		m_Readback.Create(L"GpuTimer MultiRange Readback", totalQueries, kBytesPerQuery);

		// 3) GPU 时钟频率：只在 DIRECT 队列上有效
		UINT64 Frequency = 0;
		CommandQueue& GraphicsQueue = Graphics::g_CommandManager.GetGraphicsQueue();
		hr = GraphicsQueue.GetCommandQueue()->GetTimestampFrequency(&Frequency);
		if (FAILED(hr) || Frequency == 0)
		{
			ASSERT(false, "GpuTimer::Initialize 取 GPU 时间戳频率失败，GPU 计时不可用");
			Shutdown();
			return;
		}

		m_TimestampFrequency = Frequency;
		m_SlotCount = maxFramesInFlight;
		m_CurrentSlot = 0;
		m_ActiveSlot = kInvalidSlot;
		m_LastEndedSlot = kInvalidSlot;
		m_ActivePassIdx = kInvalidSlot;
		m_LastElapsedMs = 0.0f;
		m_LastPassTimings.clear();
		m_Initialized = true;
	}

	void GpuTimer::Shutdown(void)
	{
		m_Readback.Destroy();
		m_QueryHeap.Reset();

		m_TimestampFrequency = 0;
		m_SlotCount = 0;
		m_CurrentSlot = 0;
		m_ActiveSlot = kInvalidSlot;
		m_LastEndedSlot = kInvalidSlot;
		m_ActivePassIdx = kInvalidSlot;
		m_LastElapsedMs = 0.0f;
		m_LastPassTimings.clear();
		m_Initialized = false;
		for (uint32_t i = 0; i < kMaxSlots; ++i)
		{
			m_Slots[i].InFlight = false;
			m_Slots[i].Fence = 0;
			m_Slots[i].PassCount = 0;
			for (auto& name : m_Slots[i].PassNames)
				name.clear();
		}
	}

	bool GpuTimer::IsInitialized(void) const
	{
		return m_Initialized;
	}

	void GpuTimer::SetEnabled(bool enabled)
	{
		m_Enabled = enabled;
	}

	void GpuTimer::Begin(GraphicsContext& context)
	{
		if (!m_Initialized || !m_Enabled)
		{
			m_ActiveSlot = kInvalidSlot;
			m_ActivePassIdx = kInvalidSlot;
			return;
		}

		if (m_ActiveSlot != kInvalidSlot)
		{
			// R4.5：GPU 计时是可选诊断功能，若上一帧因 Resize/异常跳过 End，本帧直接复位该槽而非崩溃
			LOG_TRACE(L"[GpuTimer] 上一帧 Begin 未配对 End，本帧跳过采样\n");
			m_Slots[m_ActiveSlot].InFlight = false;
			m_Slots[m_ActiveSlot].Fence = 0;
			m_Slots[m_ActiveSlot].PassCount = 0;
			m_ActiveSlot = kInvalidSlot;
			m_ActivePassIdx = kInvalidSlot;
			return;
		}

		const uint32_t slot = m_CurrentSlot;
		if (m_Slots[slot].InFlight)
		{
			if (!Graphics::g_CommandManager.GetGraphicsQueue().IsFenceComplete(m_Slots[slot].Fence))
			{
				return;
			}
			m_Slots[slot].InFlight = false;
			m_Slots[slot].Fence = 0;
		}

		m_Slots[slot].PassCount = 0;
		const uint32_t baseQuery = slot * kQueriesPerFrame;
		context.InsertTimeStamp(m_QueryHeap.Get(), baseQuery + 0);

		m_ActiveSlot = slot;
		m_ActivePassIdx = kInvalidSlot;
		m_CurrentSlot = (slot + 1) % m_SlotCount;
	}

	void GpuTimer::BeginPass(GraphicsContext& context, const char* passName)
	{
		if (!m_Initialized || m_ActiveSlot == kInvalidSlot)
			return;

		// 不支持嵌套 Pass 打点；若意外嵌套则静默跳过内层避免覆盖外层查询下标
		if (m_ActivePassIdx != kInvalidSlot)
			return;

		FrameSlotData& slotData = m_Slots[m_ActiveSlot];
		if (slotData.PassCount >= kMaxPassesPerFrame)
			return;

		const uint32_t passIdx = slotData.PassCount;
		slotData.PassNames[passIdx] = (passName != nullptr) ? passName : "UnnamedPass";
		const uint32_t queryIdx = m_ActiveSlot * kQueriesPerFrame + 2 + passIdx * 2;
		context.InsertTimeStamp(m_QueryHeap.Get(), queryIdx);
		m_ActivePassIdx = passIdx;
	}

	void GpuTimer::EndPass(GraphicsContext& context)
	{
		if (!m_Initialized || m_ActiveSlot == kInvalidSlot || m_ActivePassIdx == kInvalidSlot)
			return;

		const uint32_t passIdx = m_ActivePassIdx;
		m_ActivePassIdx = kInvalidSlot;

		const uint32_t queryIdx = m_ActiveSlot * kQueriesPerFrame + 2 + passIdx * 2 + 1;
		context.InsertTimeStamp(m_QueryHeap.Get(), queryIdx);
		++m_Slots[m_ActiveSlot].PassCount;
	}

	void GpuTimer::End(GraphicsContext& context)
	{
		if (!m_Initialized)
		{
			m_ActiveSlot = kInvalidSlot;
			m_ActivePassIdx = kInvalidSlot;
			return;
		}

		if (m_ActiveSlot == kInvalidSlot)
			return;

		// 若有遗漏未关闭的 Pass，强制收口防止查询悬空
		if (m_ActivePassIdx != kInvalidSlot)
			EndPass(context);

		const uint32_t slot = m_ActiveSlot;
		m_ActiveSlot = kInvalidSlot;

		const uint32_t baseQuery = slot * kQueriesPerFrame;
		const uint32_t queryCount = 2 + m_Slots[slot].PassCount * 2;
		context.InsertTimeStamp(m_QueryHeap.Get(), baseQuery + 1);

		const UINT64 destOffsetBytes = static_cast<UINT64>(baseQuery) * kBytesPerQuery;
		context.ResolveQueryData(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
			baseQuery, queryCount, m_Readback.GetResource(), destOffsetBytes);

		m_LastEndedSlot = slot;
	}

	void GpuTimer::OnSubmitted(uint64_t fenceValue)
	{
		if (!m_Initialized || m_LastEndedSlot == kInvalidSlot)
			return;

		const uint32_t slot = m_LastEndedSlot;
		m_LastEndedSlot = kInvalidSlot;

		if (fenceValue == 0)
			return;

		m_Slots[slot].Fence = fenceValue;
		m_Slots[slot].InFlight = true;
	}

	bool GpuTimer::TryGetElapsedMs(float& outMilliseconds)
	{
		if (!m_Initialized || m_TimestampFrequency == 0 || m_SlotCount == 0)
			return false;

		uint32_t slot = kInvalidSlot;
		for (uint32_t i = 0; i < m_SlotCount; ++i)
		{
			if (!m_Slots[i].InFlight)
				continue;

			if (!Graphics::g_CommandManager.GetGraphicsQueue().IsFenceComplete(m_Slots[i].Fence))
				continue;

			if (slot == kInvalidSlot || m_Slots[i].Fence > m_Slots[slot].Fence)
				slot = i;
		}

		if (slot == kInvalidSlot)
			return false;

		const uint64_t* Timestamps = static_cast<const uint64_t*>(m_Readback.Map());
		if (Timestamps == nullptr)
		{
			LOG_TRACE(L"[GpuTimer] 回读堆 Map 失败（GPU 仍在写入），本帧跳过采样\n");
			m_Slots[slot].InFlight = false;
			m_Slots[slot].Fence = 0;
			return false;
		}

		const uint32_t baseQuery = slot * kQueriesPerFrame;
		const uint64_t BeginTimestamp = Timestamps[baseQuery + 0];
		const uint64_t EndTimestamp   = Timestamps[baseQuery + 1];

		std::vector<PassTiming> parsedPasses;
		const uint32_t passCount = m_Slots[slot].PassCount;
		parsedPasses.reserve(passCount);
		const double msScale = 1000.0 / static_cast<double>(m_TimestampFrequency);
		for (uint32_t p = 0; p < passCount; ++p)
		{
			const uint64_t pBegin = Timestamps[baseQuery + 2 + p * 2];
			const uint64_t pEnd   = Timestamps[baseQuery + 2 + p * 2 + 1];
			float ms = 0.0f;
			if (pEnd > pBegin)
				ms = static_cast<float>(static_cast<double>(pEnd - pBegin) * msScale);
			parsedPasses.push_back(PassTiming{ m_Slots[slot].PassNames[p], ms });
		}

		m_Readback.Unmap();

		m_Slots[slot].InFlight = false;
		m_Slots[slot].Fence = 0;

		if (EndTimestamp <= BeginTimestamp)
			return false;

		m_LastElapsedMs = static_cast<float>(static_cast<double>(EndTimestamp - BeginTimestamp) * msScale);
		m_LastPassTimings = std::move(parsedPasses);
		outMilliseconds = m_LastElapsedMs;
		return true;
	}
}
