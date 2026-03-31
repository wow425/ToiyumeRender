#pragma once


#include <vector>
#include <queue>
#include <mutex>
#include <stdint.h>
#include "CommandAllocatorPool.h"


class CommandQueue
{
	friend class CommandListManager;
	friend class CommandContext;

public:

	CommandQueue(D3D12_COMMAND_LIST_TYPE Type);
	~CommandQueue();

	void Create(ID3D12Device* pDevice);
	void Shutdown();

	inline bool IsReady()
	{
		return m_CommandQueue != nullptr;
	}

	uint64_t IncrementFence(void); // 
	bool IsFenceComplete(uint64_t FenceValue);
	void StallForFence(uint64_t FenceValue);
	void StallForProducer(CommandQueue& Producer);
	void WaitForFence(uint64_t FenceValue);
	void WaitForIdle(void) { WaitForFence(IncrementFence()); }


private:

	uint64_t ExecutrCommandList(ID3D12CommandList* List);
	ID3D12CommandAllocator* RequestAllocator(void);
	void DiscardAllocator(uint64_t FenceValueForReset, ID3D12CommandAllocator* Allocator);

	ID3D12CommandQueue* m_CommandQueue;
	const D3D12_COMMAND_LIST_TYPE m_Type;

	CommandAllocatorPool m_AllocatorPool;
	std::mutex m_FenceMutex;
	std::mutex m_EveneMutex;


	ID3D12Fence* m_pFence; // 围栏对象
	uint64_t m_NextFenceValue; // 下一个值
	uint64_t m_LastCompletedFenceValue; // 已完成值
	HANDLE m_FenceEventHandle; // win32事件句柄


};

class CommandListManager
{
	friend class CommandContext;

public:


private:

	ID3D12Device* m_Device;
	// 图形，计算，复制，三队列
	CommandQueue m_GraphicsQueue;
	CommandQueue m_ComputeQueueu;
	CommandQueue m_CopyQueue;
};
