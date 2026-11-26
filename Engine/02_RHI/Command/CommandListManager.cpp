#include "00_Core/PCH.h"
#include "02_RHI/Command/CommandListManager.h"

CommandQueue::CommandQueue(D3D12_COMMAND_LIST_TYPE Type) :
	m_Type(Type),
	m_CommandQueue(nullptr),
	m_pFence(nullptr),
	/*Type为8位，转为64位，再左移56位，使得Type的8位处于前8位，再按位或将最低位设为1
	解决多队列调试与追踪问题。
	全局唯一性：通过将类型放在高位，Graphics 队列的 Fence 值永远以 0x00...开头，Compute 队列以 0x02...开头，Copy 队列以 0x03...开头。
	防止混淆：在调试工具（如 PIX 或 Visual Studio Graphics Analyzer）中，如果你看到一个 Fence 值为 0x0200000000000005，你一眼就能断定这是 Compute Queue 的第 5 个任务，而不会将其与 Graphics 队列混淆。
	底层溯源(Rule 4)：在 GPU 硬件架构中，不同引擎（Engine）是独立执行的。这种赋值方式在软件层面模拟了硬件的“通道”概念，确保每个队列的计数器在 64 位地址空间内互不重叠。
	*/
	m_NextFenceValue((uint64_t)Type << 56 | 1),
	m_LastCompletedFenceValue((uint64_t)Type << 56),
	m_AllocatorPool(Type)
{
}

CommandQueue::~CommandQueue() { Shutdown(); }

void CommandQueue::Shutdown()
{
	// 关分配器池，关围栏值事件句柄，关围栏，关命令队列
	if (m_CommandQueue == nullptr)
		return;

	m_AllocatorPool.Shutdown();
	if (m_FenceEventHandle != nullptr)
	{
		CloseHandle(m_FenceEventHandle);
		m_FenceEventHandle = nullptr;
	}
	if (m_pFence != nullptr)
	{
		m_pFence->Release();
		m_pFence = nullptr;
	}
	m_CommandQueue->Release();
	m_CommandQueue = nullptr;

	// [TD-08b] 复位 Fence 计数至初始基值，防止同进程二次 Init 时新 Fence 从 0 起步却被旧缓存误判已完成。
	m_NextFenceValue = ((uint64_t)m_Type << 56) | 1;
	m_LastCompletedFenceValue = ((uint64_t)m_Type << 56);
}

void CommandQueue::Create(ID3D12Device* pDevice)
{
	// 确认有设备，无队列，无分配器池
	ASSERT(pDevice != nullptr);
	ASSERT(!IsReady());
	ASSERT(m_AllocatorPool.Size() == 0);

	D3D12_COMMAND_QUEUE_DESC QueueDesc{};
	QueueDesc.Type = m_Type;
	QueueDesc.NodeMask = 1;
	// [TD-04e] 严格检查 CreateCommandQueue 与 Signal 的 HRESULT，避免静默忽略设备失败
	ASSERT_SUCCEEDED(pDevice->CreateCommandQueue(&QueueDesc, TY_IID_PPV_ARGS(&m_CommandQueue)));
	m_CommandQueue->SetName(L"CommandListManager::m_CommandQueue");

	ASSERT_SUCCEEDED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, TY_IID_PPV_ARGS(&m_pFence)));
	m_pFence->SetName(L"CommandListManager::m_pFence");
	ASSERT_SUCCEEDED(m_pFence->Signal((uint64_t)m_Type << 56));  // 值更新

	m_FenceEventHandle = CreateEvent(nullptr, false, false, nullptr);
	ASSERT(m_FenceEventHandle != NULL);

	m_AllocatorPool.Create(pDevice);
	ASSERT(IsReady());
}



CommandListManager::CommandListManager() :
	m_Device(nullptr),
	m_GraphicsQueue(D3D12_COMMAND_LIST_TYPE_DIRECT),
	m_ComputeQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE),
	m_CopyQueue(D3D12_COMMAND_LIST_TYPE_COPY)
{
}

CommandListManager::~CommandListManager()
{
	Shutdown();
}

void CommandListManager::Shutdown()
{
	m_GraphicsQueue.Shutdown();
	m_ComputeQueue.Shutdown();
	m_CopyQueue.Shutdown();
	m_Device = nullptr;
}

void CommandListManager::Create(ID3D12Device* pDevice)
{
	ASSERT(pDevice != nullptr);
	m_Device = pDevice;
	m_GraphicsQueue.Create(pDevice);
	m_ComputeQueue.Create(pDevice);
	m_CopyQueue.Create(pDevice);
}

void CommandListManager::CreateNewCommandList(D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator** Allocator, REFIID riid, void** ppList)
{
	ASSERT(Type != D3D12_COMMAND_LIST_TYPE_BUNDLE, "Bundles are not yet supported");
	switch (Type)
	{
	case D3D12_COMMAND_LIST_TYPE_DIRECT: *Allocator = m_GraphicsQueue.RequestAllocator(); break;
	case D3D12_COMMAND_LIST_TYPE_BUNDLE: break;
	case D3D12_COMMAND_LIST_TYPE_COMPUTE: *Allocator = m_ComputeQueue.RequestAllocator(); break;
	case D3D12_COMMAND_LIST_TYPE_COPY: *Allocator = m_CopyQueue.RequestAllocator(); break;
	}

	ASSERT_SUCCEEDED(m_Device->CreateCommandList(1, Type, *Allocator, nullptr, riid, ppList));
	if (ppList && *ppList)
	{
		static_cast<ID3D12Object*>(*ppList)->SetName(L"CommandList");
	}
}

// ==========================================

//  执行命令列表，设置signal，返回m_NextFenceValue
uint64_t CommandQueue::ExecuteCommandList(ID3D12CommandList* List)
{
	std::lock_guard<std::mutex> LockGuard(m_FenceMutex);

	ASSERT_SUCCEEDED(((ID3D12GraphicsCommandList*)List)->Close()); // 向下转换

	m_CommandQueue->ExecuteCommandLists(1, &List);
	const uint64_t fenceValue = m_NextFenceValue.load(std::memory_order_relaxed);
	ASSERT_SUCCEEDED(m_CommandQueue->Signal(m_pFence, fenceValue));
	m_NextFenceValue.store(fenceValue + 1, std::memory_order_release);

	return fenceValue;
}

uint64_t CommandQueue::IncrementFence(void)
{
	std::lock_guard<std::mutex> LockGuard(m_FenceMutex);
	const uint64_t fenceValue = m_NextFenceValue.load(std::memory_order_relaxed);
	ASSERT_SUCCEEDED(m_CommandQueue->Signal(m_pFence, fenceValue)); // m_pFence更新为m_NextFenceValue
	m_NextFenceValue.store(fenceValue + 1, std::memory_order_release);
	return fenceValue;
}

// 查询围栏是否击中
bool CommandQueue::IsFenceComplete(uint64_t FenceValue)
{
	// [TD-04e] 原子单调最大更新：无锁快速路径零数据竞争，检测设备移除返回 UINT64_MAX。
	uint64_t cached = m_LastCompletedFenceValue.load(std::memory_order_acquire);
	if (FenceValue > cached)
	{
		const uint64_t completed = m_pFence->GetCompletedValue();
		if (completed == UINT64_MAX)
		{
			LOG_ERROR(L"[RHI][TD-04e] ID3D12Fence::GetCompletedValue() 返回 UINT64_MAX（设备已移除/TDR）\n");
			return true;
		}
		while (completed > cached &&
			!m_LastCompletedFenceValue.compare_exchange_weak(
				cached, completed, std::memory_order_acq_rel, std::memory_order_acquire))
		{
		}
		cached = m_LastCompletedFenceValue.load(std::memory_order_acquire);
	}

	return FenceValue <= cached;
}

namespace Graphics
{
	extern CommandListManager g_CommandManager;
}

// 跨队列同步
void CommandQueue::StallForFence(uint64_t FenceValue)
{
	// 根据围栏值获取对应的生产者队列类型
	CommandQueue& Producer = Graphics::g_CommandManager.GetQueue((D3D12_COMMAND_LIST_TYPE)(FenceValue >> 56));
	// 让GPU的一个队列等待另一个队列，不阻塞CPU。将当前消费者的命令处理器挂起stall，直到生产者m_pFence值更新，再继续处理数据
	ASSERT_SUCCEEDED(m_CommandQueue->Wait(Producer.m_pFence, FenceValue));
}

// 待优化
void CommandQueue::WaitForFence(uint64_t FenceValue)
{
	if (IsFenceComplete(FenceValue))
		return;

	{
		std::lock_guard<std::mutex> LockGuard(m_EveneMutex);

		const HRESULT hr = m_pFence->SetEventOnCompletion(FenceValue, m_FenceEventHandle);
		if (FAILED(hr))
		{
			LOG_ERROR(L"[RHI][TD-04e] SetEventOnCompletion 失败 hr=0x%08X (fence=%llu)\n",
				(unsigned)hr, (unsigned long long)FenceValue);
			ASSERT_SUCCEEDED(hr);
			return;
		}
		const DWORD waitRes = WaitForSingleObject(m_FenceEventHandle, INFINITE);
		if (waitRes != WAIT_OBJECT_0)
		{
			LOG_ERROR(L"[RHI][TD-04e] WaitForSingleObject 失败 result=0x%08X lastErr=%lu\n",
				(unsigned)waitRes, (unsigned long)GetLastError());
		}

		uint64_t cached = m_LastCompletedFenceValue.load(std::memory_order_acquire);
		while (FenceValue > cached &&
			!m_LastCompletedFenceValue.compare_exchange_weak(
				cached, FenceValue, std::memory_order_acq_rel, std::memory_order_acquire))
		{
		}
	}
}

// 让CPU强制等待FenceValue对应的队列类型执行完任务
void CommandListManager::WaitForFence(uint64_t FenceValue)
{
	CommandQueue& Producer = Graphics::g_CommandManager.GetQueue((D3D12_COMMAND_LIST_TYPE)(FenceValue >> 56));
	Producer.WaitForFence(FenceValue);
}

ID3D12CommandAllocator* CommandQueue::RequestAllocator()
{
	uint64_t CompletedFence = m_pFence->GetCompletedValue();

	return m_AllocatorPool.RequestAllocator(CompletedFence);
}

void CommandQueue::DiscardAllocator(uint64_t FenceValue, ID3D12CommandAllocator* Allocator)
{
	m_AllocatorPool.DiscardAllocator(FenceValue, Allocator);
}
