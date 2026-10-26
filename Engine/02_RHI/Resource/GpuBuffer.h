#pragma once
#include "02_RHI/Resource/GpuResource.h"
// R3：Destroy 里要调用 Graphics::g_DescriptorAllocator.Free 归还描述符。
// 与 ColorBuffer.h 的做法一致（GraphicsCore.h 不包含任何 Resource 头，无循环依赖）。
#include "02_RHI/GraphicsCore.h"

class CommandContext;
class EsramAllocator;
class UploadBuffer;



class GpuBuffer : public GpuResource
{
public:
    virtual ~GpuBuffer() { Destroy(); }

    // R3：Destroy 必须一并归还 SRV/UAV 描述符并复位。
    // ------------------------------------------------------------------
    // 原因与 ColorBuffer/DepthBuffer 在 R2 的修复完全同构：
    // CreateDerivedViews 用 `if (m_SRV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)`
    // 判断「这块 buffer 还没分配过描述符」，只分配一次。如果不在这里复位，
    // 同一个 GpuBuffer 对象第二次 Create（例如切换 Renderer 时重新
    // InitializeResources）就会跳过分配，继续沿用上一轮指向已销毁资源的句柄：
    //   (1) 每次重建泄漏一批描述符；
    //   (2) 旧句柄位置被别的资源复用后，写入会覆盖别人的视图，
    //       表现为随机贴图错乱 —— 这类 bug 极难复现。
    // 句柄本身是 CPU 描述符（不属于 GpuResource），所以只能在这一层复位。
    virtual void Destroy(void) override
    {
        if (m_SRV.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
        {
            Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_SRV, 1);
            m_SRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
        }
        if (m_UAV.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
        {
            Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_UAV, 1);
            m_UAV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
        }
        GpuResource::Destroy();
    }
    // 创建
        // 1. 基础型：全自动创建 (Committed Resource)
    void Create(const std::wstring& name, uint32_t NumElements, uint32_t ElementSize,
        const void* initialData = nullptr);

    // 2. 高效型：直接从中转缓冲区创建
    void Create(const std::wstring& name, uint32_t NumElements, uint32_t ElementSize,
        const UploadBuffer& srcData, uint32_t srcOffset = 0);

    // 3. 架构师型：放置型创建 (Placed Resource)
    void CreatePlaced(const std::wstring& name, ID3D12Heap* pBackingHeap, uint32_t HeapOffset, uint32_t NumElements, uint32_t ElementSize,
        const void* initialData = nullptr);

    // 视图获取
        // 1. 获取UAV,SRV
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV(void) const { return m_UAV; }
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV(void) const { return m_SRV; }

    // 2. 根常量视图创建和获取
    D3D12_CPU_DESCRIPTOR_HANDLE CreateConstantBufferView(uint32_t Offset, uint32_t Size) const;
    D3D12_GPU_VIRTUAL_ADDRESS RootConstantBufferView(void) const { return m_GpuVirtualAddress; }

    // 3. 获取VBV，IBV
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView(size_t Offset, uint32_t Size, uint32_t Stride) const;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView(size_t BaseVertexIndex = 0) const // 从指定索引处往后的数据作为VBV
    {
        size_t Offset = BaseVertexIndex * m_ElementSize;
        return VertexBufferView(Offset, (uint32_t)(m_BufferSize - Offset), m_ElementSize);
    }

    D3D12_INDEX_BUFFER_VIEW IndexBufferView(size_t Offset, uint32_t Size, bool b32Bit = false) const; // const只读传递，禁止修改成员变量
    D3D12_INDEX_BUFFER_VIEW IndexBufferView(size_t StateIndex = 0) const // 使用16位还是32位存储索引？ 65535为界限
    {
        size_t Offset = StateIndex * m_ElementSize;
        return IndexBufferView(Offset, (uint32_t)(m_BufferSize - Offset), m_ElementSize == 4);
    }
    // 4. 获取buffer三属性：buffer大小，元素数量，元素大小
    size_t GetBufferSize() const { return m_BufferSize; }
    uint32_t GetElementCount() const { return m_ElementCount; }
    uint32_t GetElementSize() const { return m_ElementSize; }

protected:

    GpuBuffer(void) : m_BufferSize(0), m_ElementCount(0), m_ElementSize(0)
    {
        m_ResourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        m_UAV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
        m_SRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }

    D3D12_RESOURCE_DESC DescribeBuffer(void);
    // virtual表虚函数，允许子类通过多态重写
    // = 0 纯虚函数，强制子类必须重写该函数，
    // 含此为抽象类，不可直接实例化基类
    virtual void CreateDerivedViews(void) = 0; // 创建派生视图
    // 如：ColorBuffer用RTV/SRV，DepthBuffer用DSV，StructuredBuffer用UAV


    D3D12_CPU_DESCRIPTOR_HANDLE m_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE m_SRV;

    size_t m_BufferSize;
    uint32_t m_ElementCount;
    uint32_t m_ElementSize;
    D3D12_RESOURCE_FLAGS m_ResourceFlags;
};

inline D3D12_VERTEX_BUFFER_VIEW GpuBuffer::VertexBufferView(size_t Offset, uint32_t Size, uint32_t Stride) const
{
    D3D12_VERTEX_BUFFER_VIEW VBView;
    VBView.BufferLocation = m_GpuVirtualAddress + Offset;
    VBView.SizeInBytes = Size;  // 缓冲区大小
    VBView.StrideInBytes = Stride; // 顶点步长
    return VBView;
}

inline D3D12_INDEX_BUFFER_VIEW GpuBuffer::IndexBufferView(size_t Offset, uint32_t Size, bool b32Bit) const
{
    D3D12_INDEX_BUFFER_VIEW IBView;
    IBView.BufferLocation = m_GpuVirtualAddress + Offset;
    IBView.Format = b32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT; // 
    IBView.SizeInBytes = Size;
    return IBView;
}
// 1. 字节地址缓冲区
class ByteAddressBuffer : public GpuBuffer
{
public:
    virtual void CreateDerivedViews(void) override;
};

// 2. 间接参数缓冲区，gpu根据指令自动执行，解决cpu瓶颈
class IndirectArgsBuffer : public ByteAddressBuffer
{
public:
    IndirectArgsBuffer(void) {};
};

// 3. 存放同种结构体
class StructuredBuffer : public GpuBuffer
{
public:
    virtual void Destroy(void) override
    {
        m_CounterBuffer.Destroy();
        GpuBuffer::Destroy();
    }

    virtual void CreateDerivedViews(void) override;

    ByteAddressBuffer& GetCounterBuffer(void) { return m_CounterBuffer; }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetCounterSRV(CommandContext& Context);
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetCounterUAV(CommandContext& Context);

private:
    ByteAddressBuffer m_CounterBuffer;
};

// 4. 格式自动转换 (Format Conversion)
class TypedBuffer : public GpuBuffer
{
public:
    TypedBuffer(DXGI_FORMAT Format) : m_DataFormat(Format) {}
    virtual void CreateDerivedViews(void) override;

protected:
    DXGI_FORMAT m_DataFormat;
};
