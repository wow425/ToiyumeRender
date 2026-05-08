#pragma once







#include <mutex>
#include <vector>
#include <queue>
#include <string>






// CPU可见描述符分配器
// 无界限（Unbounded）的线性内存分配器，专门用于分配仅 CPU 可见（CPU-visible）的描述符。
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

    static ID3D12DescriptorHeap* RequestNewHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type);

    static const uint32_t sm_NumDescriptorsPerHeap = 256; // 每堆的描述符容量
    static std::mutex sm_AllocationMutex;
    static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> sm_DescriptorHeapPool; // 描述符堆池


    D3D12_DESCRIPTOR_HEAP_TYPE m_Type;
    ID3D12DescriptorHeap* m_CurrentHeap; // 当前指向的堆
    D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentHandle; // 当前指向的堆的游标
    uint32_t m_DescriptorSize;       // 描述符大小
    uint32_t m_RemainingFreeHandles; // 可用容量
};

// 双端描述符句柄封装
// 从接口设计上明确了该类的语义——它专门服务于“着色器可见”的描述符，强制要求 GPU 地址的有效性。
class DescriptorHandle
{
public:
    DescriptorHandle()
    {
        m_CpuHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
        m_GpuHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }

    DescriptorHandle(D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
        : m_CpuHandle(CpuHandle), m_GpuHandle(GpuHandle) {
    }

    DescriptorHandle operator+ (INT offsetScaledByDesriptorSize) const
    {
        DescriptorHandle ret = *this;
        ret += offsetScaledByDesriptorSize;
        return ret;
    }

    void operator += (INT OffsetScaledByDescriptorSize)
    {
        if (m_CpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
            m_CpuHandle.ptr += OffsetScaledByDescriptorSize;
        if (m_GpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
            m_GpuHandle.ptr += OffsetScaledByDescriptorSize;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE* operator&() const { return &m_CpuHandle; }
    operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return m_CpuHandle; }
    operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return m_GpuHandle; }

    size_t GetCpuPtr() const { return m_CpuHandle.ptr; }
    uint64_t GetGpuPtr() const { return m_GpuHandle.ptr; }
    bool IsNull() const { return m_CpuHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }
    bool IsShaderVisible() const { return m_GpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE m_CpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE m_GpuHandle;

};

// Shader Visible Descriptor Heap
// 固定容量（Bounded）的线性内存分配器，专门管理对 GPU 着色器可见的描述符资源。
class DescriptorHeap
{
public:
    DescriptorHeap(void) {}
    ~DescriptorHeap(void) { Destroy(); }

    void Create(const std::wstring& DebugHeapName, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t MaxCount);
    void Destroy(void) { m_Heap = nullptr; }

    bool HasAvailableSpace(uint32_t Count) const { return Count <= m_NumFreeDescriptors; } // 查询空间是否足够存放
    DescriptorHandle Alloc(uint32_t Count = 1); // 分配
    // 寻址
    DescriptorHandle operator[] (uint32_t arrayIndex) const { return m_FirstHandle + arrayIndex * m_DescriptorSize; }
    // 反推索引
    uint32_t GetOffsetOfHandle(const DescriptorHandle& DHandle)
    {
        return (uint32_t)(DHandle.GetCpuPtr() - m_FirstHandle.GetCpuPtr()) / m_DescriptorSize;
    }


    bool ValidateHandle(const DescriptorHandle& DHandle) const;

    ID3D12DescriptorHeap* GetHeapPointer() const { return m_Heap.Get(); }

    uint32_t GetDescriptorSize(void) const { return m_DescriptorSize; }

private:

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
    D3D12_DESCRIPTOR_HEAP_DESC m_HeapDesc;
    uint32_t m_DescriptorSize;
    uint32_t m_NumFreeDescriptors; // 堆上可存放描述符容量
    DescriptorHandle m_FirstHandle; // 当前堆的起始地址
    DescriptorHandle m_NextFreeHandle; // 下一个可用空闲地址
};

