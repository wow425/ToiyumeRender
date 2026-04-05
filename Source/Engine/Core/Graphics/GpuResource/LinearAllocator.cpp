#include "PCH.h"
#include "LinearAllocator.h"
#include "../Graphics/GraphicsCore.h"
#include "../Graphics/Command/CommandListManager.h"
#include <thread>

using namespace Graphics;
using namespace std;

LinearAllocatorType LinearAllocatorPageManager::sm_AutoType = kGpuExclusive;

// 自动注册设计模式，利用sm_AutoType静态成员变量为计数器,解决静态数组初始化
// sm_PageManager[2],0为Gpu，1为Cpu
LinearAllocatorPageManager::LinearAllocatorPageManager()
{
    m_AllocationType = sm_AutoType;
    sm_AutoType = (LinearAllocatorType)(sm_AutoType + 1);
    ASSERT(sm_AutoType <= kNumAllocatorTypes);
}

LinearAllocatorPageManager LinearAllocator::sm_PageManager[2];

// 申请页面
LinearAllocationPage* LinearAllocatorPageManager::RequestPage()
{
    // 上锁
    lock_guard<mutex> LockGuard(m_Mutex);

    // 回收退休页面(cpu用完，gpu还在用的）
    while (!m_RetiredPagesQueue.empty() && g_CommandManager.IsFenceComplete(m_RetiredPagesQueue.front().first))
    {
        m_AvailablePagesQueue.push(m_RetiredPagesQueue.front().second);
        m_RetiredPagesQueue.pop();
    }

    LinearAllocationPage* PagePtr = nullptr;

    // 从可用页面队列中取件
    if (!m_AvailablePagesQueue.empty())
    {
        PagePtr = m_AvailablePagesQueue.front();
        m_AvailablePagesQueue.pop();
    }
    else // 无件可取，则新创页面
    {
		PagePtr = CreateNewPage();
        m_PagePool.emplace_back(PagePtr);
    }

	return PagePtr;
}

// 废弃页面(登记到退休页面队列中, 回收利用)
void LinearAllocatorPageManager::DiscardPages(uint64_t FenceValue, const vector<LinearAllocationPage*>& UsedPages)
{
    lock_guard<mutex> LockGuard(m_Mutex);
    for (auto iter = UsedPages.begin(); iter != UsedPages.end(); ++iter)
    {
        m_RetiredPagesQueue.push(make_pair(FenceValue, *iter));
    }
}

// 释放大页面(彻底销毁，不回收利用)避免内存碎片化和迎合偶发性需求
void LinearAllocatorPageManager::FreeLargePages(uint64_t FenceValue, const vector<LinearAllocationPage*>& LargePages)
{
    lock_guard<mutex> LockGuard(m_Mutex);

    // 释放删除队列
    while (!m_DeletionQueue.empty() && g_CommandManager.IsFenceComplete(m_DeletionQueue.front().first))
    {
        delete m_DeletionQueue.front().second;
        m_DeletionQueue.pop();
    }
    // 登记废弃大页面
    for (auto iter = LargePages.begin(); iter != LargePages.end(); ++iter)
    {
        (*iter)->Unmap(); // 避免产生野指针
        m_DeletionQueue.push(make_pair(FenceValue, *iter));
    }
}

// 创建页面
LinearAllocationPage* LinearAllocatorPageManager::CreateNewPage(size_t PageSize)
{
    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC ResourceDesc;
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; // BUFFER
    ResourceDesc.Alignment = 0;
    ResourceDesc.Height = 1;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = 1;
    ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.SampleDesc.Quality = 0;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // 页面类型设置
    D3D12_RESOURCE_STATES DefaultUsage;
    if (m_AllocationType == kGpuExclusive) // 默认堆
    {
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        ResourceDesc.Width = PageSize == 0 ? kGpuAllocatorPageSize : PageSize;
        ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; //
        DefaultUsage = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; //
    }
    else // 上传堆
    {
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        ResourceDesc.Width = PageSize == 0 ? kCpuAllocatorPageSize : PageSize;
        ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE; //
        DefaultUsage = D3D12_RESOURCE_STATE_GENERIC_READ; //
    }

    ID3D12Resource* pBuffer;
    ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
        &ResourceDesc, DefaultUsage, nullptr, TY_IID_PPV_ARGS(&pBuffer)));
    pBuffer->SetName(L"LinearAllocator Page");
    
    return new LinearAllocationPage(pBuffer, DefaultUsage);
}

// 所有权交接。命令录制完成后便可交接给全局页面管理器
void LinearAllocator::CleanupUsedPages(uint64_t FenceID)
{
    // 封口当前页面
    if (m_CurPage != nullptr)
    {
        m_RetiredPages.push_back(m_CurPage);
        m_CurPage = nullptr;
        m_CurOffset = 0;
    }
    // 上交页面用于回收利用
    sm_PageManager[m_AllocationType].DiscardPages(FenceID, m_RetiredPages);
    m_RetiredPages.clear();
    // 上交大页面以销毁
    sm_PageManager[m_AllocationType].FreeLargePages(FenceID, m_LargePageList);
    m_LargePageList.clear();
}

// 分配大页面
DynAlloc LinearAllocator::AllocateLargePage(size_t SizeInBytes)
{
    LinearAllocationPage* OneOff = sm_PageManager[m_AllocationType].CreateNewPage(SizeInBytes);
    m_LargePageList.push_back(OneOff);

    DynAlloc ret(*OneOff, 0, SizeInBytes);
    ret.DataPtr = OneOff->m_CpuVirtualAddress;
    ret.GpuAddress = OneOff->m_GpuVirtualAddress;

    return ret;
}

DynAlloc LinearAllocator::Allocate(size_t SizeInBytes, size_t Alignment)
{
    // 1. 验证对齐值为2的幂
    const size_t AlignmentMask = Alignment - 1; // 获取掩码便于位运算
    ASSERT((AlignmentMask & Alignment) == 0);
    // 2. 计算对齐后的大小，大于标准页面则分配成大页面
    const size_t AlignedSize = Math::AlignUpWithMask(SizeInBytes, AlignmentMask);
    if (AlignedSize > m_PageSize) return AllocateLargePage(AlignedSize);
    // 3. 对齐当前偏移量
    m_CurOffset = Math::AlignUp(m_CurOffset, Alignment);
    // 4. 检查页面容量或翻页
    if (m_CurOffset + AlignedSize > m_PageSize)
    {
        ASSERT(m_CurPage != nullptr)
            m_RetiredPages.push_back(m_CurPage);
        m_CurPage = nullptr;
    }
    if (m_CurPage == nullptr)
    {
        m_CurPage = sm_PageManager[m_AllocationType].RequestPage();
        m_CurOffset = 0;
    }
    // 5.分配
    DynAlloc ret(*m_CurPage, m_CurOffset, AlignedSize);
    ret.DataPtr = (uint8_t*)m_CurPage->m_CpuVirtualAddress + m_CurOffset;
    ret.GpuAddress = m_CurPage->m_GpuVirtualAddress + m_CurOffset;

    m_CurOffset += AlignedSize;

    return ret;

}