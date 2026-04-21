#include "PCH.h"
#include "DescriptorHeap.h"
#include "../GraphicsCore.h"
#include "../Command/CommandListManager.h"

using namespace Graphics;

//
// DescriptorAllocator implementation
//
std::mutex DescriptorAllocator::sm_AllocationMutex;
std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> DescriptorAllocator::sm_DescriptorHeapPool;

void DescriptorAllocator::DestroyAll(void)
{
    sm_DescriptorHeapPool.clear();
}
// 申请新堆
ID3D12DescriptorHeap* DescriptorAllocator::RequestNewHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type)
{
    std::lock_guard<std::mutex> LockGuard(sm_AllocationMutex);

    D3D12_DESCRIPTOR_HEAP_DESC Desc;
    Desc.Type = Type;
    Desc.NumDescriptors = sm_NumDescriptorsPerHeap;
    Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Desc.NodeMask = 1;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pHeap;
    ASSERT_SUCCEEDED(Graphics::g_Device->CreateDescriptorHeap(&Desc, TY_IID_PPV_ARGS(&pHeap)));

    sm_DescriptorHeapPool.emplace_back(pHeap);
    return pHeap.Get();
}

// 分配，前端接口
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocator::Allocate(uint32_t Count)
{
    // 若无堆或可用容量<要求数量
    if (m_CurrentHeap == nullptr || m_RemainingFreeHandles < Count)
    {
        m_CurrentHeap = RequestNewHeap(m_Type); // 申请current堆
        m_CurrentHandle = m_CurrentHeap->GetCPUDescriptorHandleForHeapStart(); // 获取current句柄
        m_RemainingFreeHandles = sm_NumDescriptorsPerHeap; // 设置current堆剩余容量句柄

        if (m_DescriptorSize == 0)
            m_DescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(m_Type); // 设置描述符大小
    }

    D3D12_CPU_DESCRIPTOR_HANDLE ret = m_CurrentHandle;
    m_CurrentHandle.ptr += Count * m_DescriptorSize;
    m_RemainingFreeHandles -= Count;
    return ret;
}

//
// DescriptorHeap implementation
//

// 创建shader可视的描述符堆
void DescriptorHeap::Create(const std::wstring& Name, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t MaxCount)
{
    m_HeapDesc.Type = Type;
    m_HeapDesc.NumDescriptors = MaxCount;
    m_HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_HeapDesc.NodeMask = 1;

    ASSERT_SUCCEEDED(g_Device->CreateDescriptorHeap(&m_HeapDesc, TY_IID_PPV_ARGS(m_Heap.ReleaseAndGetAddressOf())));

#ifdef RELEASE
    (void)Name;
#else
    m_Heap->SetName(Name.c_str());
#endif

    m_DescriptorSize = g_Device->GetDescriptorHandleIncrementSize(m_HeapDesc.Type);
    m_NumFreeDescriptors = m_HeapDesc.NumDescriptors;
    m_FirstHandle = DescriptorHandle(
        m_Heap->GetCPUDescriptorHandleForHeapStart(),
        m_Heap->GetGPUDescriptorHandleForHeapStart());
    m_NextFreeHandle = m_FirstHandle;
}

DescriptorHandle DescriptorHeap::Alloc(uint32_t Count)
{
    // 检查容量
    ASSERT(HasAvailableSpace(Count), "Descriptor Heap out of space.Increase heap size.");
    // 返回当前游标句柄，并移动游标和可用容量减小
    DescriptorHandle ret = m_NextFreeHandle;
    m_NextFreeHandle += Count * m_DescriptorSize;
    m_NumFreeDescriptors -= Count;
    return ret;
}

// 检验句柄合法不和是否属于当前堆中的
bool DescriptorHeap::ValidateHandle(const DescriptorHandle& DHandle) const
{
    // 绝对边界检查。起始地址之前或最大地址之后
    if (DHandle.GetCpuPtr() < m_FirstHandle.GetCpuPtr() ||
        DHandle.GetCpuPtr() >= m_FirstHandle.GetCpuPtr() + m_HeapDesc.NumDescriptors * m_DescriptorSize)
        return false;

    // 相对偏移一致性检查。确保gpu跟cpu两个虚拟地址偏移距离相同
    if (DHandle.GetGpuPtr() - m_FirstHandle.GetGpuPtr() !=
        DHandle.GetCpuPtr() - m_FirstHandle.GetCpuPtr())
        return false;

    return true;
}
