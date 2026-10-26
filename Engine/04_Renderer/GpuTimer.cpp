#include "00_Core/PCH.h"
#include "04_Renderer/GpuTimer.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/Heap/ReadbackBuffer.h"
#include "00_Core/Utility/Utility.h"

// ============================================================
// GpuTimer 实现说明（R3-E）
// ------------------------------------------------------------
// 一次采样由三段组成，缺一段都会读到垃圾数据：
//   1) Begin/End：在渲染命令列表里各插一个 TIMESTAMP 查询（槽 i 用 2i / 2i+1）；
//      End 里顺手 ResolveQueryData 把两个 uint64 拷到 READBACK 堆的第 i*16 字节。
//   2) OnSubmitted：命令列表提交后，把 CommandContext::Finish() 返回的围栏值记到该槽。
//   3) TryGetElapsedMs：只有当 IsFenceComplete(该槽围栏值) 为真时才 Map 回读堆，
//      否则 GPU 可能还在往这块内存里写 —— Debug Layer 会直接报错，读到的是半成品。
//
// 为什么用 ResolveQueryData 而不是 CommandContext::ResolveTimeStamps：
//   后者的目标偏移被写死成 0，只适合「堆里只有一对时间戳」的场景；这里要多槽轮转，
//   每个槽必须落到自己的 16 字节区间，所以直接调底层的 ResolveQueryData 传偏移。
// ============================================================
namespace Renderer
{
	namespace
	{
		// 单个槽在回读堆里的字节数：起始 / 结束各一个 uint64。
		constexpr uint32_t kBytesPerSlot = (uint32_t)(2 * sizeof(uint64_t));
	}

	void GpuTimer::Initialize(ID3D12Device* device, uint32_t maxFramesInFlight)
	{
		// 允许重复初始化：先整体复位，避免上一次的堆/回读资源泄漏。
		Shutdown();

		if (device == nullptr)
		{
			ASSERT(false, "GpuTimer::Initialize 收到空的 ID3D12Device，GPU 计时不可用");
			return;
		}

		// 槽数夹紧到 [1, kMaxSlots]：0 会让取模除零，超过上限会写穿定长数组。
		if (maxFramesInFlight == 0)
		{
			maxFramesInFlight = 1;
		}
		if (maxFramesInFlight > kMaxSlots)
		{
			maxFramesInFlight = kMaxSlots;
		}

		// 1) 时间戳查询堆：2 个 query / 槽（帧开始、帧结束）。
		D3D12_QUERY_HEAP_DESC QueryHeapDesc = {};
		QueryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		QueryHeapDesc.Count = 2 * maxFramesInFlight;
		QueryHeapDesc.NodeMask = 0;

		HRESULT hr = device->CreateQueryHeap(&QueryHeapDesc, TY_IID_PPV_ARGS(&m_QueryHeap));
		if (FAILED(hr))
		{
			ASSERT(false, "GpuTimer::Initialize 创建 TIMESTAMP 查询堆失败，GPU 计时不可用");
			Shutdown();
			return;
		}

		// 2) 回读堆：每槽 16 字节，CPU 侧 Map 读取；GPU 侧是 ResolveQueryData 的落地目标。
		m_Readback.Create(L"GpuTimer Readback", 2 * maxFramesInFlight, (uint32_t)sizeof(uint64_t));

		// 3) GPU 时钟频率：一次取好，之后换算毫秒都靠它。
		//    必须问图形队列 —— 时间戳查询只在 DIRECT 队列上有效，换别的队列频率单位可能不同。
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
		m_LastElapsedMs = 0.0f;
		m_Initialized = true;
	}

	void GpuTimer::Shutdown(void)
	{
		// 顺序：先放回读堆（普通资源），再放查询堆。
		// GpuBuffer::Destroy 内部会把 SRV/UAV 描述符句柄归还分配器，重复调用也安全。
		m_Readback.Destroy();
		m_QueryHeap.Reset();

		m_TimestampFrequency = 0;
		m_SlotCount = 0;
		m_CurrentSlot = 0;
		m_ActiveSlot = kInvalidSlot;
		m_LastEndedSlot = kInvalidSlot;
		m_LastElapsedMs = 0.0f;
		m_Initialized = false;
		for (uint32_t i = 0; i < kMaxSlots; ++i)
		{
			m_SlotInFlight[i] = false;
			m_SlotFence[i] = 0;
		}

		// 注意：m_Enabled 是「面板要不要显示/采样」的用户偏好，不随资源生命周期复位。
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
			return;
		}

		if (m_ActiveSlot != kInvalidSlot)
		{
			// ============================================================
			// R4.5：这里**不能**断言。
			// ------------------------------------------------------------
			// 原实现用 ASSERT(false) 终止进程，触发条件是「上一次 Begin
			// 之后没有配对的 End」。而这种情况在正常运行时**会发生**：
			//
			//   RenderScene 里 Begin(gfxContext) 插入了起始时间戳，
			//   但若该帧后半段被跳过（例如 Resize 期间 Present 早退、
			//   或异常路径直接 return），End() 就不会被调用，
			//   槽永远停在"已 Begin 未 End"。
			//   下一帧再 Begin 就命中这条断言 —— 表现为
			//   「拉伸窗口几次后闪退」，退出码 2170（CRT abort）。
			//
			// 正确语义：GPU 计时是可选的诊断功能，缺一个配对就**跳过本帧采样**，
			// 绝不能因此让渲染器崩溃。这里直接放弃本帧、把槽标记为可复用。
			// ============================================================
			LOG_TRACE(L"[GpuTimer] 上一帧 Begin 未配对 End，本帧跳过采样\n");
			m_SlotInFlight[m_ActiveSlot] = false;
			m_SlotFence[m_ActiveSlot] = 0;
			m_ActiveSlot = kInvalidSlot;
			return;
		}

		const uint32_t slot = m_CurrentSlot;

		// 槽要被复用了：上一轮的数据若还没读走，先确认 GPU 已经写完。
		// - 未完成：这一槽的 query/回读内存 GPU 还在用，本帧必须放弃采样（否则就是飞行中写）；
		// - 已完成：正常流程里 TryGetElapsedMs 每帧都会把样本读掉，走到这里说明面板没取，
		//   直接丢弃旧样本，让 GPU 内存可以安全复用。
		if (m_SlotInFlight[slot])
		{
			if (!Graphics::g_CommandManager.GetGraphicsQueue().IsFenceComplete(m_SlotFence[slot]))
			{
				return;
			}
			m_SlotInFlight[slot] = false;
			m_SlotFence[slot] = 0;
		}

		context.InsertTimeStamp(m_QueryHeap.Get(), slot * 2);

		m_ActiveSlot = slot;
		m_CurrentSlot = (slot + 1) % m_SlotCount;
	}

	void GpuTimer::End(GraphicsContext& context)
	{
		if (!m_Initialized)
		{
			m_ActiveSlot = kInvalidSlot;
			return;
		}

		if (m_ActiveSlot == kInvalidSlot)
		{
			// Begin 没采（未初始化 / 被关闭 / 槽忙），这里什么也不能做：
			// 缺了起始时间戳的查询，Resolve 出来的是无意义数值。
			return;
		}

		// 注意这里不再看 m_Enabled：关闭开关可能正好发生在 Begin 之后，
		// 那一帧的起始时间戳已经插进命令列表，必须补上结束时间戳并 Resolve 收尾，
		// 否则该槽永远停在「已 Begin 未 End」状态，下一帧 Begin 会一直被断言拦下。

		const uint32_t slot = m_ActiveSlot;
		m_ActiveSlot = kInvalidSlot;

		context.InsertTimeStamp(m_QueryHeap.Get(), slot * 2 + 1);

		// 把这一槽的两个 uint64 拷到回读堆的 slot*16 字节处（slot * kBytesPerSlot）。
		// 不传 slot*16 而是 kBytesPerSlot 的乘积，是为了让「每槽多少字节」只有一个出处。
		context.ResolveQueryData(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
			slot * 2, 2, m_Readback.GetResource(), (UINT64)(slot * kBytesPerSlot));

		// 这里还不知道围栏值，等 OnSubmitted 来认领。
		m_LastEndedSlot = slot;
	}

	void GpuTimer::OnSubmitted(uint64_t fenceValue)
	{
		if (!m_Initialized || m_LastEndedSlot == kInvalidSlot)
		{
			return;
		}

		const uint32_t slot = m_LastEndedSlot;
		m_LastEndedSlot = kInvalidSlot;

		// 调用方传了 0（没有提交 / 传错），这个样本永远等不到完成，直接丢弃。
		if (fenceValue == 0)
		{
			return;
		}

		m_SlotFence[slot] = fenceValue;
		m_SlotInFlight[slot] = true;
	}

	bool GpuTimer::TryGetElapsedMs(float& outMilliseconds)
	{
		if (!m_Initialized || m_TimestampFrequency == 0 || m_SlotCount == 0)
		{
			return false;
		}

		// 先在所有槽里挑「已提交且 GPU 已执行完」中围栏值最大的那一槽 ——
		// 围栏值单调递增，最大即最近一帧。不能只按槽号顺序猜：某帧可能因为槽忙
		// 而被跳过，槽号与时间先后并不总是一致。
		uint32_t slot = kInvalidSlot;
		for (uint32_t i = 0; i < m_SlotCount; ++i)
		{
			if (!m_SlotInFlight[i])
			{
				continue;
			}

			// 关键的一步：GPU 没写完就 Map 是非法访问（Debug Layer 必报），必须跳过。
			if (!Graphics::g_CommandManager.GetGraphicsQueue().IsFenceComplete(m_SlotFence[i]))
			{
				continue;
			}

			if (slot == kInvalidSlot || m_SlotFence[i] > m_SlotFence[slot])
			{
				slot = i;
			}
		}

		if (slot == kInvalidSlot)
		{
			// 一帧都还没跑完，正常现象（刚启动或 GPU 落后）。
			return false;
		}

		const uint64_t* Timestamps = static_cast<const uint64_t*>(m_Readback.Map());
		if (Timestamps == nullptr)
		{
			// ============================================================
			// R4.4 修复：这里**不能**断言。
			// ------------------------------------------------------------
			// 原实现用 ASSERT(false, ...) 在这终止整个进程，导致
			// 「Deferred 下拉伸窗口直接闪退」—— 崩溃栈是：
			//     GpuTimer::TryGetElapsedMs
			//       -> RendererDemoApp::Update
			//       -> GameCore::UpdateApplication
			// 与渲染资源生命周期毫无关系。
			//
			// 为什么会 Map 失败：
			//   D3D12 的回读堆在 GPU 仍在读写映射区间时 Map 会失败。
			//   本函数虽然先检查了 IsFenceComplete，但 Resize 会主动调用
			//   IdleGPU()，打乱了「围栏已完成 == 本槽可安全读」这个推断；
			//   槽的 InFlight 记录与真实 GPU 状态在那一个瞬间不一致。
			//
			// 正确的语义：GPU 计时是**可选的诊断功能**。拿不到样本就
			// 沿用上一次的值（调用方本来就这么处理 false），
			// 绝不能因为"测不到帧时间"而让渲染器崩溃。
			// 同时把这一槽标记为可复用，避免它永远卡在 InFlight 状态。
			// ============================================================
			LOG_TRACE(L"[GpuTimer] 回读堆 Map 失败（GPU 仍在写入），本帧跳过采样\n");
			m_SlotInFlight[slot] = false;
			m_SlotFence[slot] = 0;
			return false;
		}

		const uint64_t BeginTimestamp = Timestamps[slot * 2];
		const uint64_t EndTimestamp = Timestamps[slot * 2 + 1];
		m_Readback.Unmap();

		// 读完就释放这一槽，允许后续帧复用。
		m_SlotInFlight[slot] = false;
		m_SlotFence[slot] = 0;

		// 空查询在部分驱动上会解析成两个相等的值，甚至倒序；这种样本直接丢弃。
		// 该槽的数据已经读过并释放，本次返回 false 让调用方沿用上一次的值。
		if (EndTimestamp <= BeginTimestamp)
		{
			return false;
		}

		m_LastElapsedMs = (float)((double)(EndTimestamp - BeginTimestamp) * 1000.0 / (double)m_TimestampFrequency);
		outMilliseconds = m_LastElapsedMs;
		return true;
	}
}
