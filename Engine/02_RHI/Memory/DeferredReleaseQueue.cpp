// ============================================================================
// 模块：DeferredReleaseQueue（帧尾盖章延迟释放队列 · 实现）
// 架构层：Engine/02_RHI/Memory
// 核心职责：在整帧最后一次 DIRECT 提交后统一盖章，并按 FIFO 顺序在 GPU 完成对应 Fence 后
//           安全释放 COM 引用及执行描述符归还回调。
// ============================================================================
#include "00_Core/PCH.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"

namespace Graphics
{
	void DeferredReleaseQueue::Enqueue(Microsoft::WRL::ComPtr<IUnknown> obj)
	{
		if (obj == nullptr)
			return;

		PendingItem item;
		item.Obj = std::move(obj);
		m_Pending.push_back(std::move(item));
	}

	void DeferredReleaseQueue::EnqueueCallback(std::function<void()> onGpuComplete)
	{
		if (!onGpuComplete)
			return;

		PendingItem item;
		item.Fn = std::move(onGpuComplete);
		m_Pending.push_back(std::move(item));
	}

	bool DeferredReleaseQueue::IsStampComplete(const FrameFenceStamp& stamp)
	{
		// 所有非零队列 Fence 均达到完成态时，方可认为 GPU 不再引用该帧任何资源。
		if (stamp.DirectFence != 0 && !g_CommandManager.IsFenceComplete(stamp.DirectFence))
			return false;
		if (stamp.ComputeFence != 0 && !g_CommandManager.IsFenceComplete(stamp.ComputeFence))
			return false;
		if (stamp.CopyFence != 0 && !g_CommandManager.IsFenceComplete(stamp.CopyFence))
			return false;
		return true;
	}

	void DeferredReleaseQueue::FlushFrame(const FrameFenceStamp& endOfFrameStamp)
	{
		// 1) 为本帧新入队的所有待释放项统一盖上帧尾印章
		for (auto& pending : m_Pending)
		{
			m_Stamped.push_back(StampedItem{ endOfFrameStamp, std::move(pending) });
		}
		m_Pending.clear();

		// 2) 因印章按帧单调递增，从队头依次回收已越过 GPU Fence 的项即可
		while (!m_Stamped.empty() && IsStampComplete(m_Stamped.front().Stamp))
		{
			PendingItem item = std::move(m_Stamped.front().Item);
			m_Stamped.pop_front();
			// 先释放 COM 资源，再执行归还槽位回调（保证槽位复写前原资源已彻底解绑）
			item.Obj.Reset();
			if (item.Fn)
			{
				item.Fn();
			}
		}
	}

	void DeferredReleaseQueue::DrainAll()
	{
		// 调用前置契约：调用方必须已执行 IdleGPU()，确保 GPU 无任何在途引用。
		while (!m_Stamped.empty())
		{
			PendingItem item = std::move(m_Stamped.front().Item);
			m_Stamped.pop_front();
			item.Obj.Reset();
			if (item.Fn)
			{
				item.Fn();
			}
		}

		for (auto& pending : m_Pending)
		{
			pending.Obj.Reset();
			if (pending.Fn)
			{
				pending.Fn();
			}
		}
		m_Pending.clear();
	}
}
