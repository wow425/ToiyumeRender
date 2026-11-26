// ============================================================================
// 模块：DeferredReleaseQueue（帧尾盖章延迟释放队列）
// 架构层：Engine/02_RHI/Memory（纯单向 RHI 层，禁止反向依赖 Display/Renderer）
// 核心职责：分离 CPU 逻辑删除时机与 GPU 显存/描述符/PSO 真实安全回收时机（DX12 时钟 1）。
// 线程与队列前提：仅单线程主循环调用；当前绑定 DIRECT 队列，预留 Compute/Copy 槽位。
// 关键铁律：FlushFrame() 必须在 RenderScene、ImGui UI 与 Display::Present 拷贝
//           全部提交之后调用，严禁在 RenderScene 结束时提前盖章！
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include <wrl/client.h>
#include <unknwn.h>

namespace Graphics
{
	// 帧尾多队列 Fence 印章：当前业务全走 DIRECT 队列，Compute/Copy 槽位预留给未来异步队列。
	struct FrameFenceStamp
	{
		uint64_t DirectFence  = 0;
		uint64_t ComputeFence = 0;
		uint64_t CopyFence    = 0;
	};

	// 所有权归 Graphics 全局 RHI 子系统；在 Shutdown 期间必须先 IdleGPU() 再调用 DrainAll()。
	class DeferredReleaseQueue
	{
	public:
		DeferredReleaseQueue() = default;
		~DeferredReleaseQueue() { DrainAll(); }

		DeferredReleaseQueue(const DeferredReleaseQueue&) = delete;
		DeferredReleaseQueue& operator=(const DeferredReleaseQueue&) = delete;

		// 入队待释放的 DX12 COM 对象（ID3D12Resource / ID3D12PipelineState / ID3D12StateObject 等）。
		void Enqueue(Microsoft::WRL::ComPtr<IUnknown> obj);

		// 入队与 COM 同寿命的 GPU 完成回调（用于归还 CPU 描述符槽 / Zone A/C BindlessIndex）。
		void EnqueueCallback(std::function<void()> onGpuComplete);

		// 【硬铁律】唯一合法调用点：GameCore 主循环中 RenderScene、ImGui UI 列表
		// 与 Display::Present（含 PreparePresentSDR Copy）全部提交之后！
		// 严禁在 RenderScene 结束时提前盖章，否则 UI/拷贝/在途 PSO 会被提前释放。
		void FlushFrame(const FrameFenceStamp& endOfFrameStamp);

		// 仅在 IdleGPU() 之后调用（Resize 全排空 / Shutdown），无条件释放全部队列项。
		void DrainAll();

		size_t GetPendingCount() const { return m_Pending.size(); }
		size_t GetStampedCount() const { return m_Stamped.size(); }

	private:
		struct PendingItem
		{
			Microsoft::WRL::ComPtr<IUnknown> Obj;
			std::function<void()> Fn;
		};

		struct StampedItem
		{
			FrameFenceStamp Stamp;
			PendingItem Item;
		};

		static bool IsStampComplete(const FrameFenceStamp& stamp);

		std::vector<PendingItem> m_Pending;
		std::deque<StampedItem>  m_Stamped;
	};
}
