#pragma once


#include <vector>
#include <queue>
#include <mutex>
#include <stdint.h>
#include "CommandAllocatorPool.h"

// 命令队列封装
class CommandQueue
{
    friend class CommandListManager;
    friend class CommandContext;

public:
    CommandQueue(D3D12_COMMAND_LIST_TYPE Type);
    ~CommandQueue();

    void Create(ID3D12Device* pDevice);
    void Shutdown();



    uint64_t IncrementFence(void);
    bool IsFenceComplete(uint64_t FenceValue);
    void StallForFence(uint64_t FenceValue);
    void StallForProducer(CommandQueue& Producer);
    void WaitForFence(uint64_t FenceValue);
    void WaitForIdle(void) { WaitForFence(IncrementFence()); }

    ID3D12CommandQueue* GetCommandQueue() { return m_CommandQueue; }
    uint64_t GetNexFenceValue() { return m_NextFenceValue; }
    inline bool IsReady() { return m_CommandQueue != nullptr; }

private:

    uint64_t ExecuteCommandList(ID3D12CommandList* List);
    ID3D12CommandAllocator* RequestAllocator(void);
    void DiscardAllocator(uint64_t FenceValueForReset, ID3D12CommandAllocator* Allocator);

    ID3D12CommandQueue* m_CommandQueue;
    const D3D12_COMMAND_LIST_TYPE m_Type;

    CommandAllocatorPool m_AllocatorPool;
    std::mutex m_FenceMutex;
    std::mutex m_EveneMutex;


    // Lifetime of these objects is managed by the descriptor cache
    ID3D12Fence* m_pFence; // 围栏对象
    uint64_t m_NextFenceValue; // 下一个值
    uint64_t m_LastCompletedFenceValue; // 本地缓存的完成值
    HANDLE m_FenceEventHandle; // win32事件句柄
};


// 命令列表管理器，管理图形，计算，复制三种类型的命令队列
class CommandListManager
{
    friend class CommandContext;

public:
    CommandListManager();
    ~CommandListManager();

    void Create(ID3D12Device* pDevice);
    void Shutdown();

    CommandQueue& GetGraphicsQueue(void) { return m_GraphicsQueue; }
    CommandQueue& GetComputeQueue(void) { return m_ComputeQueue; }
    CommandQueue& GetCopyQueue(void) { return m_CopyQueue; }
    ID3D12CommandQueue* GetCommandQueue() { return m_GraphicsQueue.GetCommandQueue(); }
    CommandQueue& GetQueue(D3D12_COMMAND_LIST_TYPE Type = D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        switch (Type)
        {
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: return m_ComputeQueue;
        case D3D12_COMMAND_LIST_TYPE_COPY: return m_CopyQueue;
        default: return m_GraphicsQueue;
        }
    }

    void CreateNewCommandList(D3D12_COMMAND_LIST_TYPE Type, ID3D12GraphicsCommandList** List, ID3D12CommandAllocator** Allocator);


    // 查询围栏值是否抵达
    bool IsFenceComplete(uint64_t FenceValue)
    {
        // FenceValue64位，右移56位，去除任务ID，前8位为队列类型
        // 解压->类型转换->查询队列->查询任务是否完成
        return GetQueue(D3D12_COMMAND_LIST_TYPE(FenceValue >> 56)).IsFenceComplete(FenceValue);
    }
    // CPU等待围栏值命中
    void WaitForFence(uint64_t FenceValue);

    // CPU挂起直至命令队列执行完毕，
    void IdleGPU(void)
    {
        m_GraphicsQueue.WaitForIdle();
        m_ComputeQueue.WaitForIdle();
        m_CopyQueue.WaitForIdle();
    }
private:

    ID3D12Device* m_Device;
    // 图形，计算，复制，三队列类
    CommandQueue m_GraphicsQueue;
    CommandQueue m_ComputeQueue;
    CommandQueue m_CopyQueue;
};
