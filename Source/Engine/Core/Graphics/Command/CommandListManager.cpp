#include "PCH.h"
#include "CommandListManager.h"

CommandQueue::CommandQueue(D3D12_COMMAND_LIST_TYPE Type) :
	m_Type(Type),
	m_CommandQueue(nullptr),
	m_pFence(nullptr),
	/*Type为8位，转为64位，再左移56位，使得Type的8位处于前8位，再按位或将最低位设为1
	解决多队列调试与追踪问题。
	全局唯一性：通过将类型放在高位，Graphics 队列的 Fence 值永远以 0x00...开头，Compute 队列以 0x02...开头，Copy 队列以 0x03...开头。
	防止混淆：在调试工具（如 PIX 或 Visual Studio Graphics Analyzer）中，如果你看到一个 Fence 值为 0x0200000000000005，你一眼就能断定这是 Compute Queue 的第 5 个任务，而不会将其与 Graphics 队列混淆。
	底层溯源(Rule 4)：在 GPU 硬件架构中，不同引擎（Engine）是独立执行的。这种赋值方式在软件层面模拟了硬件的“通道”概念，确保每个队列的计数器在 64 位地址空间内互不重叠。*/
	m_NextFenceValue((uint64_t)Type << 56 | 1),
	m_LastCompletedFenceValue((uint64_t)Type << 56),
	m_AllocatorPool(Type)
{ }

CommandQueue::~CommandQueue() { Shutdown(); }

void CommandQueue::Shutdown()
{
	// 关分配器池，关围栏值事件句柄，关围栏，关命令队列
	if (m_CommandQueue == nullptr)
		return;

	m_AllocatorPool.Shutdown();
	CloseHandle(m_FenceEventHandle);
	m_pFence->Release();
	m_pFence = nullptr;
	m_CommandQueue->Release();
	m_CommandQueue = nullptr;
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
	pDevice->CreateCommandQueue(&QueueDesc, TY_IID_PPV_ARGS(&m_CommandQueue));
	m_CommandQueue->SetName(L"CommandListManager::m_CommandQueue");
	
	ASSERT_SUCCEEDED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, TY_IID_PPV_ARGS(&m_pFence)));
	m_pFence->SetName(L"CommandListManager::m_pFence");
	m_pFence->Signal((uint64_t)m_Type << 56);  // 值更新

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
{ }

CommandListManager::~CommandListManager()
{
	Shutdown();
}

void CommandListManager::Shutdown()
{
	m_GraphicsQueue.Shutdown();
	m_ComputeQueue.Shutdown();
	m_CopyQueue.Shutdown();
}

void CommandListManager::Create(ID3D12Device* pDevice)
{
	ASSERT(pDevice != nullptr);
	m_Device = pDevice;
	m_GraphicsQueue.Create(pDevice);
	m_ComputeQueue.Create(pDevice);
	m_CopyQueue.Create(pDevice);
}

void CommandListManager::CreateNewCommandList(D3D12_COMMAND_LIST_TYPE Type, ID3D12GraphicsCommandList** List, ID3D12CommandAllocator** Allocator)
{
	ASSERT(Type != D3D12_COMMAND_LIST_TYPE_BUNDLE, "Bundles are not yet supported");
	switch (Type)
	{
	case D3D12_COMMAND_LIST_TYPE_DIRECT: *Allocator = m_GraphicsQueue.RequestAllocator(); break;
	case D3D12_COMMAND_LIST_TYPE_BUNDLE: break;
	case D3D12_COMMAND_LIST_TYPE_COMPUTE: *Allocator = m_ComputeQueue.RequestAllocator(); break;
	case D3D12_COMMAND_LIST_TYPE_COPY: *Allocator = m_CopyQueue.RequestAllocator(); break;
	}

	ASSERT_SUCCEEDED(m_Device->CreateCommandList(1, Type, *Allocator, nullptr, TY_IID_PPV_ARGS(List)));
	(*List)->SetName(L"CommandList");
}

// ==========================================

uint64_t CommandQueue::ExecuteCommandList(ID3D12CommandList* List)
{
	std::lock_guard<std::mutex> LockGuard(m_FenceMutex);

	ASSERT_SUCCEEDED( ( (ID3D12GraphicsCommandList*)List)->Close()); // 向下转换

	m_CommandQueue->ExecuteCommandLists(1, &List);
	m_CommandQueue->Signal(m_pFence, m_NextFenceValue);

	return m_NextFenceValue++;
}

uint64_t CommandQueue::IncrementFence(void)
{
	std::lock_guard<std::mutex> LockGuard(m_FenceMutex);
	m_CommandQueue->Signal(m_pFence, m_NextFenceValue); // m_pFence更新为m_NextFenceValue
	return m_NextFenceValue++;
}

// 查询围栏是否击中
bool CommandQueue::IsFenceComplete(uint64_t FenceValue)
{
	// 为了性能考虑，我们优先比对本地缓存的围栏值，以减少对硬件 API 的昂贵查询。
	// 这里强制使用 max() 是一种防御性编程，用来规避极罕见的竞态条件(硬件导致的数值抖动）——确保观测到的 GPU 进度只会前进，绝不会出现数值倒退的异常。
	// “永远不要完全信任硬件返回的非原子快照” 是一条准则。

	if (FenceValue > m_LastCompletedFenceValue)
		m_LastCompletedFenceValue = std::max(m_LastCompletedFenceValue, m_pFence->GetCompletedValue());

	return FenceValue <= m_LastCompletedFenceValue;
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
	m_CommandQueue->Wait(Producer.m_pFence, FenceValue); 
}

// 待优化
void CommandQueue::WaitForFence(uint64_t FenceValue)
{
	//“待优化：注意多线程并发时的同步粒度。如果线程 A 抢先绑定了目标值为 100 的唤醒事件，而随后线程 B 只需要等待目标值 99，
	// 由于我们底层只有一个共用的唤醒事件句柄，导致线程 B 被迫‘连坐’，不得不跟着等 GPU 跑到 100 才能被唤醒。后续可以考虑引入事件队列（多句柄）来解决这个延迟瓶颈。”
	// 上述被称为线程饥饿 (Thread Starvation)。 需要事件池 (Event Pool)优化
	if (IsFenceComplete(FenceValue))
		return;

	{
		std::lock_guard<std::mutex> LockGuard(m_EveneMutex);

		m_pFence->SetEventOnCompletion(FenceValue, m_FenceEventHandle);
		WaitForSingleObject(m_FenceEventHandle, INFINITE);
		m_LastCompletedFenceValue = FenceValue;
	}
}

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