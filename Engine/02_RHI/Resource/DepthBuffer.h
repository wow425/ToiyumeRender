#pragma once

#include "02_RHI/Resource/PixelBuffer.h"
// R2：Destroy 里要调用 Graphics::g_DescriptorAllocator.Free 归还描述符。
#include "02_RHI/GraphicsCore.h"

class DepthBuffer : public PixelBuffer
{
public:
	// R2：显式析构体（原因同 ColorBuffer 的析构注释）
	// GpuResource 的析构里调用虚函数 Destroy 不会分派到本类，
	// 导致 DSV/SRV 在走 shared_ptr 析构路径时从不归还。
	~DepthBuffer() { Destroy(); }

	DepthBuffer(float ClearDepth = 1.0f, uint8_t ClearStencil = 0)
		: m_ClearDepth(ClearDepth), m_ClearStencil(ClearStencil)
	{
		m_hDSV[0].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hDSV[1].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hDSV[2].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hDSV[3].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hDepthSRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hStencilSRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
	}

	// ============================================================
	// 销毁资源并释放描述符句柄（R2）
	// ------------------------------------------------------------
	// 与 ColorBuffer::Destroy 同理：CreateDerivedViews 依赖
	// 「句柄 == UNKNOWN」判断是否重新分配描述符。
	// 不重置会导致 Resize 时跳过分配、往旧句柄写入，
	// 造成描述符泄漏与跨资源覆盖。
	// ============================================================
	virtual void Destroy() override
	{
		PixelBuffer::Destroy();

		// ============================================================
		// R2：归还描述符（与 ColorBuffer::Destroy 同因）
		// ------------------------------------------------------------
		// DSV 4 个变体 + 深度/模板 SRV 都属于 CPU-only 描述符堆，
		// 不归还就会在每次 Resize 时按此数量持续泄漏。
		// ============================================================
		for (int i = 0; i < _countof(m_hDSV); ++i)
		{
			if (m_hDSV[i].ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
				Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].Free(m_hDSV[i], 1);
		}
		if (m_hDepthSRV.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_hDepthSRV, 1);
		if (m_hStencilSRV.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_hStencilSRV, 1);

		for (int i = 0; i < _countof(m_hDSV); ++i)
			m_hDSV[i].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hDepthSRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_hStencilSRV.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
	}

	// Create a depth buffer.  If an address is supplied, memory will not be allocated.
	// The vmem address allows you to alias buffers 
	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height, DXGI_FORMAT Format,
		D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height, uint32_t NumSamples, DXGI_FORMAT Format,
		D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	// Get pre-created CPU-visible descriptor handles
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSV() const { return m_hDSV[0]; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSV_DepthReadOnly() const { return m_hDSV[1]; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSV_StencilReadOnly() const { return m_hDSV[2]; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSV_ReadOnly() const { return m_hDSV[3]; }

	const D3D12_CPU_DESCRIPTOR_HANDLE& GetDepthSRV() const { return m_hDepthSRV; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetStencilSRV() const { return m_hStencilSRV; }

	float GetClearDepth() const { return m_ClearDepth; }
	uint8_t GetClearStencil() const { return m_ClearStencil; }

	DXGI_FORMAT GetFormat() const { return m_format; }

protected:

	void CreateDerivedViews(ID3D12Device* Device, DXGI_FORMAT Format);

	float m_ClearDepth; // 清空值
	uint8_t m_ClearStencil;
	//DSV,OM（output merger）阶段用
	// A,B。读写与只读组合排并
	// 0双读写。1.只读深度，读写模板。2.读写深度，只读模板。3.双只读
	D3D12_CPU_DESCRIPTOR_HANDLE m_hDSV[4];
	D3D12_CPU_DESCRIPTOR_HANDLE m_hDepthSRV; // 深度SRV
	D3D12_CPU_DESCRIPTOR_HANDLE m_hStencilSRV; // 模板SRV

	DXGI_FORMAT m_format;
};

