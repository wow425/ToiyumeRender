#pragma once

// 这是一个专为 DX12 设计的动态图形内存分配器（動的グラフィックスメモリ同期アロケータ / Dynamic Graphics Memory Allocator）。
// 它的设计初衷是与 CommandContext 类协同工作，并确保以线程安全（スレッドセーフ / Thread - safe）的方式运行。
// 系统中可能存在多个命令上下文（コマンドコンテキスト / Command Context），每个上下文都拥有自己的线性分配器（リニアアロケータ / Linear Allocator）。
// 这些分配器通过预留上下文局部内存页（コンテキスト局所メモリページ / Context - local memory page），充当通往全局内存池（グローバルメモリプール / Global memory pool）的窗口。
// 请求新内存页的操作是通过互斥锁（ミューテックスロック / Mutex lock）进行保护的，以确保线程安全。
//当命令上下文执行完毕后，它会收到一个 フェンスID（Fence ID），用于指示何时可以安全地回收已使用的资源。
// 此时必须调用 CleanupUsedPages() 方法，以便在 フェンス（Fence）清除（GPU处理完成）后，将这些已使用的内存页重新安排进入复用队列喵。


// Description:  This is a dynamic graphics memory allocator for DX12.  It's designed to work in concert
// with the CommandContext class and to do so in a thread-safe manner.  There may be many command contexts,
// each with its own linear allocators.  They act as windows into a global memory pool by reserving a
// context-local memory page.  Requesting a new page is done in a thread-safe manner by guarding accesses
// with a mutex lock.
//
// When a command context is finished, it will receive a fence ID that indicates when it's safe to reclaim
// used resources.  The CleanupUsedPages() method must be invoked at this time so that the used pages can be
// scheduled for reuse after the fence has cleared.



#include "GpuResource.h"
#include <vector>
#include <queue>
#include <mutex>

// 常量块必须是256字节对齐的. 常量块是16常量的倍数，每常量4字节
// Constant blocks must be multiples of 16 constants @ 16 bytes each
#define DEFAULT_ALIGNMENT 256

struct DynAlloc
{
	DynAlloc(GpuResource& BaseResource, size_t ThisOffset, size_t ThisSize)
		: Buffer(BaseResource), Offset(ThisOffset), Size(ThisSize) {}

	GpuResource& Buffer; // 资源对象 The D3D buffer associated with this memory
	size_t Offset;		// 资源相对于起始位置的偏移 Offset from the start of the resource
	size_t Size;        // 内存大小 Reserved size of this allocation
	void* DataPtr;      // CPU端可写地址 the CPU_writeable address
	D3D12_GPU_VIRTUAL_ADDRESS GpuAddress; // GPU端地址 The GPU-visible address
};

enum LinearAllocatorType
{
	kInvalidAllocator = -1,

	kGpuExclusive = 0,		// DEFAULT   GPU-writeable (via UAV)
	kCpuWritable = 1,		// UPLOAD CPU-writeable (but write combined)

	kNumAllocatorTypes
};

enum
{
	kGpuAllocatorPageSize = 0x10000,	// 64K
	kCpuAllocatorPageSize = 0x200000	// 2MB
};

class LinearAllocationPage : public GpuResource
{
public:
	LinearAllocationPage(ID3D12Resource* pResource, D3D12_RESOURCE_STATES Usage) : GpuResource()
	{
		m_pResource.Attach(pResource); // 转移一个原始指针的所有权，而不增加其引用计数。
		m_UsageState = Usage;
		m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();
		m_pResource->Map(0, nullptr, &m_CpuVirtualAddress);
	}
	~LinearAllocationPage() { Unmap(); }
	
	void Map(void)
	{
		if (m_CpuVirtualAddress == nullptr)
		{
			m_pResource->Map(0, nullptr, &m_CpuVirtualAddress);
		}
	}
	
	void Unmap(void)
	{
		if (m_CpuVirtualAddress != nullptr)
		{
			m_pResource->Unmap(0, nullptr);
			m_CpuVirtualAddress = nullptr;
		}
	}
	void* m_CpuVirtualAddress;
	D3D12_GPU_VIRTUAL_ADDRESS m_GpuVirtualAddress;
};

class LinearAllocatorPageManager
{
	LinearAllocatorPageManager();
	LinearAllocationPage* RequestPage(void);
	LinearAllocationPage* CreateNewPage(size_t PageSize = 0);

	// 丢弃的内存页回收，用于固定大小的内存页
	// Discarded pages will get recycled.  This is for fixed size pages.
	void DiscardPages(uint64_t FenceID, const std::vector<LinearAllocationPage*>& Pages);

	// 释放的内存页将在其对应的 Fence（栅栏）信号通过后被销毁。这适用于单次使用的“大”尺寸内存页。
	// Freed pages will be destroyed once their fence has passed.  This is for single-use,
	// "large" pages.
	void FreeLargePages(uint64_t FenceID, const std::vector<LinearAllocationPage*>& Pages);

	void Destroy(void) { m_PagePool.clear(); }

private:

	LinearAllocatorType m_AllocationType;
	std::vector<std::unique_ptr<LinearAllocationPage> > m_PagePool;
	std::queue<std::pair<uint64_t, LinearAllocationPage*> > m_RetiredPages;
	std::queue<std::pair<uint64_t, LinearAllocationPage*> > m_DeletionQueue;
	std::queue<LinearAllocationPage*> m_AvailablePages;
	std::mutex m_Mutex;

};

class LinearAllocator
{

};