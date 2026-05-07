#include "PCH.h"
#include "CommandAllocatorPool.h"

CommandAllocatorPool::CommandAllocatorPool(D3D12_COMMAND_LIST_TYPE Type) :
	m_cCommandListType(Type), m_Device(nullptr) 
{}

CommandAllocatorPool::~CommandAllocatorPool()
{
	Shutdown();
}

// 创池
void CommandAllocatorPool::Create(ID3D12Device* pDevice) 
{
	m_Device = pDevice;
}

// 释放池
void CommandAllocatorPool::Shutdown()
{
	// 命令分配器释放
	for (size_t i = 0; i < m_AllocatorPool.size(); ++i)
		m_AllocatorPool[i]->Release();
	// 池清空
	m_AllocatorPool.clear();
}

// 申请命令分配器
ID3D12CommandAllocator* CommandAllocatorPool::RequestAllocator(uint64_t CompletedFenceValue)
{
	std::lock_guard<std::mutex> LockGuard(m_AllocatorMutex);

	ID3D12CommandAllocator* pAllocator = nullptr;
	// 若能复用就绪队列的分配器
	if (!m_ReadyAllocators.empty())
	{
		std::pair<uint64_t, ID3D12CommandAllocator*>& AllocatorPair = m_ReadyAllocators.front();
		if (AllocatorPair.first <= CompletedFenceValue) // 确保当前分配器命令已执行完
		{
			pAllocator = AllocatorPair.second;
			ASSERT_SUCCEEDED(pAllocator->Reset());
			m_ReadyAllocators.pop();
		}
	}
	// 不能复用，则创建
	if (pAllocator == nullptr)
	{
		ASSERT_SUCCEEDED(m_Device->CreateCommandAllocator(m_cCommandListType, TY_IID_PPV_ARGS(&pAllocator)));
		wchar_t AllocatorName[32];
		swprintf(AllocatorName, 32, L"CommandAllocator %zu", m_AllocatorPool.size());
		pAllocator->SetName(AllocatorName);
		m_AllocatorPool.push_back(pAllocator);
	}

	return pAllocator;
}

// 废弃指定分配器(数据不再有效，可被覆盖)
void CommandAllocatorPool::DiscardAllocator(uint64_t FenceValue, ID3D12CommandAllocator* Allocator)
{
	std::lock_guard<std::mutex> LockGuard(m_AllocatorMutex);
	m_ReadyAllocators.push(std::make_pair(FenceValue, Allocator));
}