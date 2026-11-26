#pragma once

// DX12把resource分为buffer和texture, 被抽象为ID3D12Resource,但内存布局,访问方式,cache 行为,view 类型,shader 读取方式都不同
// 区分二者靠D3D12_RESOURCE_DESC.Dimension 
// Buffer为线性连续内存.,有VB,IB,CBV,Structured Buffer,ByteAddressBuffer
// texture为具有空间维度的数据,有Texture2D,TextureCube,Texture3D,Texture2DArray.

// Texture代表“静态纹理资源” 强调“数据”
//	从文件加载
//	不作为 Render Target
//	不参与 GPU 渲染输出

#include "00_Core/PCH.h"
#include "02_RHI/Resource/GpuResource.h"

class Texture : public GpuResource
{
	friend class CommandContext;

public:

	Texture() { m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }
	Texture(D3D12_CPU_DESCRIPTOR_HANDLE Handle) : m_hCpuDescriptorHandle(Handle) {}
	// [TD-03d] 派生类析构主动调用 Destroy()，归还由本对象分配的 CPU SRV 槽位。
	virtual ~Texture() override { Destroy(); }

	void Create2D(size_t RowPitchBytes, size_t Width, size_t Height, DXGI_FORMAT Format, const void* InitData);
	void CreateCube(size_t RowPitchBytes, size_t Width, size_t Height, DXGI_FORMAT Format, const void* InitData);

	// 加载纹理
	void CreateTGAFromMemory(const void* memBuffer, size_t fileSize, bool sRGB);
	bool CreateDDSFromMemory(const void* memBuffer, size_t fileSize, bool sRGB);
	void CreatePIXImageFromMemory(const void* memBuffer, size_t fileSize);

	// [TD-03d] 释放 GPU 资源并将 m_OwnsDescriptor==true 的 CPU SRV 槽归还 g_DescriptorAllocator。
	virtual void Destroy() override;

	const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV() const { return m_hCpuDescriptorHandle; }

	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	uint32_t GetDepth() const { return m_Depth; }
protected:

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	uint32_t m_Depth = 0;
	// m成员，h句柄
	D3D12_CPU_DESCRIPTOR_HANDLE m_hCpuDescriptorHandle;
	bool m_OwnsDescriptor = false;
};

class TextureCube : public GpuResource
{
	friend class CommandContext;

public:

	TextureCube() { m_hCpuDescriptorHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }
	TextureCube(D3D12_CPU_DESCRIPTOR_HANDLE Handle) : m_hCpuDescriptorHandle(Handle) {}
	virtual ~TextureCube() override { Destroy(); }

	void CreateCube(size_t RowPitchBytes, size_t Width, size_t Height, DXGI_FORMAT Format, const void* InitData);
	// 加载纹理
	void CreateTGAFromMemory(const void* memBuffer, size_t fileSize, bool sRGB);
	bool CreateDDSFromMemory(const void* memBuffer, size_t fileSize, bool sRGB);
	void CreatePIXImageFromMemory(const void* memBuffer, size_t fileSize);

	virtual void Destroy() override;

	const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV() const { return m_hCpuDescriptorHandle; }

	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	uint32_t GetDepth() const { return m_Depth; }
protected:

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	uint32_t m_Depth = 0;
	// m成员，h句柄
	D3D12_CPU_DESCRIPTOR_HANDLE m_hCpuDescriptorHandle;
	bool m_OwnsDescriptor = false;
};
