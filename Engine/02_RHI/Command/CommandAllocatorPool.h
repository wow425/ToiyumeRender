#pragma once



#include <vector>
#include <queue>
#include <mutex>
#include <stdint.h>

// 命令分配器封装成命令分配池
class CommandAllocatorPool
{
public:
	CommandAllocatorPool(D3D12_COMMAND_LIST_TYPE Type);
	~CommandAllocatorPool();

	void Create(ID3D12Device* pDevice);
	void Shutdown();

	ID3D12CommandAllocator* RequestAllocator(uint64_t CompletedFenceValue);
	void DiscardAllocator(uint64_t FenceValue, ID3D12CommandAllocator* Allocator);

	inline size_t Size() { return m_AllocatorPool.size(); }

private:
	const D3D12_COMMAND_LIST_TYPE m_cCommandListType; // 渲染，计算，复制

	ID3D12Device* m_Device;
	// 成员锁，每个命令分配器池是独立的。而描述符分配器的锁是类静态锁，全局共享。
	std::mutex m_AllocatorMutex;
	std::vector<ID3D12CommandAllocator*> m_AllocatorPool; // 分配器池
	std::queue<std::pair<uint64_t, ID3D12CommandAllocator*>> m_ReadyAllocators; // 就绪队列
};

