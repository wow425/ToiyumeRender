#include "PCH.h"
#include "UploadBuffer.h"

ID3D12Device* g_Device = nullptr; // 后面转移

void UploadBuffer::Create(const std::wstring& name, size_t BufferSize)
{
	// 初始化.m_代表成员变量
	Destroy(); 
	m_BufferSize = BufferSize;

	// 创上传堆，CPU可访问，GPU不可访问，尽量只写不读,读取缓慢
	D3D12_HEAP_PROPERTIES HeapProps;
	HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD; 
	HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN; // 已指定堆类型，设为UNKNOWN让驱动选择
	HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN; // 内存物理上位于系统内存还是显存，设为UNKNOWN让驱动选择
	HeapProps.CreationNodeMask = 1; // 内存物理在第一个GPU 节点上创建
	HeapProps.VisibleNodeMask = 1; // 该内存对第一个 GPU节点可见

	// 上传堆资源必须视为一串连续的线性字节流，故必须1维.线性寻址
	D3D12_RESOURCE_DESC ResourceDesc = {};
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	ResourceDesc.Width = m_BufferSize;
	ResourceDesc.Height = 1;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.SampleDesc.Quality = 0;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // 行主序，适用于1维资源
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;;

	ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, TY_IID_PPV_ARGS(&m_pResource)));
	
	m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();
}


void* UploadBuffer::Map(void)
{
	
	void* Memory; // void* 万能指针，存储纯粹内存地址
	// 子资源索引， 读取区间， 输出指针
	CD3DX12_RANGE readRange(0, m_BufferSize);
	m_pResource->Map(0, &readRange, &Memory);
	return Memory;
}

void UploadBuffer::Unmap(size_t begin, size_t end)
{
	CD3DX12_RANGE readRange(begin, std::min(end, m_BufferSize));
	m_pResource->Unmap(0, &readRange);
}