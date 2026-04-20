#include "PCH.h"
#include "GpuBuffer.h"
#include "../RHI/GraphicsCore.h"
#include "../RHI/Command/CommandContext.h"
#include "../Resource/ResourceManager/BufferManager.h"
#include "UploadBuffer.h"
#include "../Math/Common.h"

using namespace Graphics;
// 创建
    // 1. 基础型：全自动创建 (Committed Resource)
void GpuBuffer::Create(const std::wstring& name, uint32_t NumElements, uint32_t ElementSize, const void* initialData)
{
    // 销毁旧资源
    Destroy();

    // 获取Buffer三属性,元素数量，大小，Buffer大小
    m_ElementCount = NumElements;
    m_ElementSize = ElementSize;
    m_BufferSize = NumElements * ElementSize;

    // 资源描述
    D3D12_RESOURCE_DESC ResourceDesc = DescribeBuffer();
    m_UsageState = D3D12_RESOURCE_STATE_COMMON;

    // 堆描述
    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;
    // 创默认堆并上传资源
    ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
        &ResourceDesc, m_UsageState, nullptr, TY_IID_PPV_ARGS(&m_pResource)));

    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();
    // 若资源存在则数据上传
    if (initialData)
        CommandContext::InitializeBuffer(*this, initialData, m_BufferSize);

    // debug时资源取名便于调试
#ifdef RELEASE
    (name);
#else
    m_pResource->SetName(name.c_str());
#endif

    CreateDerivedViews();
}

// 2. 高效型：直接从中转缓冲区创建
void GpuBuffer::Create(const std::wstring& name, uint32_t NumElements, uint32_t ElementSize, const UploadBuffer& srcData, uint32_t srcOffset)
{
    // 销毁旧资源
    Destroy();

    // 获取Buffer三属性,元素数量，大小，Buffer大小
    m_ElementCount = NumElements;
    m_ElementSize = ElementSize;
    m_BufferSize = NumElements * ElementSize;

    // 资源描述
    D3D12_RESOURCE_DESC ResourceDesc = DescribeBuffer();
    m_UsageState = D3D12_RESOURCE_STATE_COMMON;

    // 堆描述
    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
        &ResourceDesc, m_UsageState, nullptr, TY_IID_PPV_ARGS(&m_pResource)));

    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();
    // 数据上传
    // CommandContext::InitializeBuffer(*this, srcData, srcOffset);

    // debug时资源取名便于调试
#ifdef RELEASE
    (name);
#else
    m_pResource->SetName(name.c_str());
#endif

    CreateDerivedViews();
}

// 3. 架构师型：放置型创建 (Placed Resource)
void GpuBuffer::CreatePlaced(const std::wstring& name, ID3D12Heap* pBackingHeap, uint32_t HeapOffset, uint32_t NumElements, uint32_t ElementSize, const void* initialData)
{
    // 获取Buffer三属性,元素数量，大小，Buffer大小
    m_ElementCount = NumElements;
    m_ElementSize = ElementSize;
    m_BufferSize = NumElements * ElementSize;

    // 资源描述
    D3D12_RESOURCE_DESC ResourceDesc = DescribeBuffer();
    m_UsageState = D3D12_RESOURCE_STATE_COMMON;

    ASSERT_SUCCEEDED(g_Device->CreatePlacedResource(pBackingHeap, HeapOffset,
        &ResourceDesc, m_UsageState, nullptr, TY_IID_PPV_ARGS(&m_pResource)));

    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();
    //// 数据上传
    //if (initialData) 
    //    CommandContext::InitializeBuffer(*this, initialData, m_BufferSize);

    // debug时资源取名便于调试
#ifdef RELEASE
    (name);
#else
    m_pResource->SetName(name.c_str());
#endif

    CreateDerivedViews();
}

// 2. 根常量视图创建和获取
D3D12_CPU_DESCRIPTOR_HANDLE GpuBuffer::CreateConstantBufferView(uint32_t Offset, uint32_t Size) const
{
    // 判断buffer是否足够存放
    ASSERT(Offset + Size <= m_BufferSize);

    Size = Math::AlignUp(Size, 16);

    D3D12_CONSTANT_BUFFER_VIEW_DESC CBVDesc;
    CBVDesc.BufferLocation = m_GpuVirtualAddress + (size_t)Offset;
    CBVDesc.SizeInBytes = Size;

    //D3D12_CPU_DESCRIPTOR_HANDLE hCBV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    //g_Device->CreateConstantBufferView(&CBVDesc, hCBV);
    //return hCBV;
    D3D12_CPU_DESCRIPTOR_HANDLE text;
    return text;
}

D3D12_RESOURCE_DESC GpuBuffer::DescribeBuffer(void)
{
    ASSERT(m_BufferSize != 0);

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Alignment = 0;
    Desc.DepthOrArraySize = 1;
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Flags = m_ResourceFlags;
    Desc.Format = DXGI_FORMAT_UNKNOWN;
    Desc.Height = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Desc.MipLevels = 1;
    Desc.SampleDesc.Count = 1;
    Desc.SampleDesc.Quality = 0;
    Desc.Width = (UINT64)m_BufferSize;
    return Desc;
}

// 创建派生视图
void ByteAddressBuffer::CreateDerivedViews(void)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    SRVDesc.Format = DXGI_FORMAT_R32_TYPELESS; //
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.Buffer.NumElements = (UINT)m_BufferSize / 4;
    SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    if (m_SRV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
        m_SRV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateShaderResourceView(m_pResource.Get(), &SRVDesc, m_SRV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    UAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    UAVDesc.Buffer.NumElements = (UINT)m_BufferSize / 4;
    UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

    if (m_UAV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
        m_UAV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateUnorderedAccessView(m_pResource.Get(), nullptr, &UAVDesc, m_UAV);
}

void StructuredBuffer::CreateDerivedViews(void)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    SRVDesc.Format = DXGI_FORMAT_UNKNOWN; //
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.Buffer.NumElements = m_ElementCount; //
    SRVDesc.Buffer.StructureByteStride = m_ElementSize; // 
    SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE; //

    //if (m_SRV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
    //    m_SRV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateShaderResourceView(m_pResource.Get(), &SRVDesc, m_SRV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    UAVDesc.Buffer.CounterOffsetInBytes = 0;
    UAVDesc.Buffer.NumElements = m_ElementCount;
    UAVDesc.Buffer.StructureByteStride = m_ElementSize;
    UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    m_CounterBuffer.Create(L"StructuredBuffer::Counter", 1, 4);

    //if (m_UAV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
    //    m_UAV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateUnorderedAccessView(m_pResource.Get(), m_CounterBuffer.GetResource(), &UAVDesc, m_UAV);
}

void TypedBuffer::CreateDerivedViews(void)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    SRVDesc.Format = m_DataFormat;
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.Buffer.NumElements = m_ElementCount;
    SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    //if (m_SRV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
    //    m_SRV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateShaderResourceView(m_pResource.Get(), &SRVDesc, m_SRV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    UAVDesc.Format = m_DataFormat;
    UAVDesc.Buffer.NumElements = m_ElementCount;
    UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    //if (m_UAV.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
    //    m_UAV = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_Device->CreateUnorderedAccessView(m_pResource.Get(), nullptr, &UAVDesc, m_UAV);
}


// 结构化Buffer获取SRV,UAV
const D3D12_CPU_DESCRIPTOR_HANDLE& StructuredBuffer::GetCounterSRV(CommandContext& Context)
{
    Context.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_GENERIC_READ);
    return m_CounterBuffer.GetSRV();
}

const D3D12_CPU_DESCRIPTOR_HANDLE& StructuredBuffer::GetCounterUAV(CommandContext& Context)
{
    Context.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return m_CounterBuffer.GetUAV();
}
