#pragma once

#include <mutex>
#include <vector>
#include <queue>
#include <string>


// 没啃



// This is an unbounded resource descriptor allocator.  It is intended to provide space for CPU-visible
// resource descriptors as resources are created.  For those that need to be made shader-visible, they
// will need to be copied to a DescriptorHeap or a DynamicDescriptorHeap.
class DescriptorAllocator
{
public:
    DescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE Type) :
        m_Type(Type), m_CurrentHeap(nullptr), m_DescriptorSize(0)
    {
        m_CurrentHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Allocate(uint32_t Count);

    static void DestroyAll(void);

protected:

    static const uint32_t sm_NumDescriptorsPerHeap = 256;
    static std::mutex sm_AllocationMutex;
    static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> sm_DescriptorHeapPool;
    static ID3D12DescriptorHeap* RequestNewHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type);

    D3D12_DESCRIPTOR_HEAP_TYPE m_Type;
    ID3D12DescriptorHeap* m_CurrentHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentHandle;
    uint32_t m_DescriptorSize;
    uint32_t m_RemainingFreeHandles;
};

