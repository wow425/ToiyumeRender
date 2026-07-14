#pragma once

// DX12把resource分为buffer和texture, 被抽象为ID3D12Resource,但内存布局,访问方式,cache 行为,view 类型,shader 读取方式都不同
// 区分二者靠D3D12_RESOURCE_DESC.Dimension 
// Buff为线性连续内存.,有VB,IB,CBV,Structured Buffer,ByteAddressBuffer
// texture为具有空间维度的数据,有Texture2D,TextureCube,Texture3D,Texture2DArray.

//ColorBuffer。代表：“GPU 渲染目标”。强调“渲染过程”
//即：Render Target Texture
//它强调的是：“这个纹理会被 GPU 写入”
//而不是：“这个纹理只是被采样”


#include "02_RHI/Resource/PixelBuffer.h"
#include "00_Core/Utility/Color.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include <array>


// 用作交换链Back Buffer和多渲染目标MRT
class ColorBuffer : public PixelBuffer
{
public:
	ColorBuffer(Color ClearColor = Color(0.0f, 0.0f, 0.0f, 0.0f))
		: m_ClearColor(ClearColor), m_NumMipMaps(0), m_FragmentCount(1), m_SampleCount(1)
	{
		m_format = DXGI_FORMAT_UNKNOWN;
		m_RTVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_SRVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		for (int i = 0; i < _countof(m_UAVHandle); ++i)
			m_UAVHandle[i].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
	}

	// 从swap chain buffer中创建color buffer，swap chain buffer只可RTV，不可UAV
	void CreateFromSwapChain(const std::wstring& Name, ID3D12Resource* BaseResource);

	// Create a color buffer.  If an address is supplied, memory will not be allocated.
	// The vmem address allows you to alias buffers 
	// 创建color buffer，若提供显存地址，则不会重新分配内存，虚拟内存地址允许对buffer取别名
	void Create(const std::wstring& Name, uint32_t Width, uint32_t Height, uint32_t NumMips,
		DXGI_FORMAT Format, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	// Create a color buffer.  If an address is supplied, memory will not be allocated.
	// The vmem address allows you to alias buffers
	// 创建纹理数组，若提供显存地址，则不会重新分配内存，虚拟内存地址允许对buffer取别名
	void CreateArray(const std::wstring& Name, uint32_t Width, uint32_t Height, uint32_t ArrayCount,
		DXGI_FORMAT Format, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);

	void CreateCube(const std::wstring& Name, uint32_t Width, uint32_t NumMips,
	DXGI_FORMAT Format, D3D12_GPU_VIRTUAL_ADDRESS VidMemPtr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);


	// Get pre-created CPU-visible descriptor handles
	// 获取预创建cpu可视的描述符句柄。SRV,RTV,UAV
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV(void) const { return m_SRVHandle; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetRTV(void) const { return m_RTVHandle; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV(void) const { return m_UAVHandle[0]; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV(uint32_t MipSlice) const { return m_UAVHandle[MipSlice]; }

	void SetClearColor(Color ClearColor) { m_ClearColor = ClearColor; }

	void SetMsaaMode(uint32_t NumColorSamples, uint32_t NumCoverageSamples)
	{
		ASSERT(NumCoverageSamples >= NumColorSamples);
		m_FragmentCount = NumColorSamples;
		m_SampleCount = NumCoverageSamples;
	}

	Color GetClearColor(void) const { return m_ClearColor; }

	DXGI_FORMAT GetFormat(void) const { return m_format; }

	// This will work for all texture sizes, but it's recommended for speed and quality
	// that you use dimensions with powers of two (but not necessarily square.)  Pass
	// 0 for ArrayCount to reserve space for mips at creation time.
	// 此方法适用于所有纹理尺寸，但为了追求速度和图像质量，强烈建议你使用 2 的幂次方的尺寸（但不要求必须是正方形）。
	// 在资源创建时，将 NumMips传为 0，可以在底层自动为整个 Mip 层级预留内存空间。
	void GenerateMipMaps(CommandContext& Context);

protected:

	// 不开MSAA则Flags为渲染+UAV
	D3D12_RESOURCE_FLAGS CombineResourceFlags(void) const
	{
		D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
		// 不开MSAA则支持UAV
		if (Flags == D3D12_RESOURCE_FLAG_NONE && m_FragmentCount == 1)
			Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // 按位或，有1为1

		return D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | Flags; // 按位或，混合打包
	}

	// 计算给定宽高的纹理在生成完整的多级渐远纹理(Mipmap / ミップマップ) 时，总共需要多少个层级的函数。
	static inline uint32_t ComputeNumMips(uint32_t Width, uint32_t Height)
	{
		uint32_t HighBit;
		_BitScanReverse((unsigned long*)&HighBit, Width | Height);
		return HighBit + 1;
	}


	void CreateDerivedViews(ID3D12Device* Device, DXGI_FORMAT Format, uint32_t ArraySize, uint32_t NumMips = 1);
	void CreateCubeDerivedViews(ID3D12Device* Device, DXGI_FORMAT Format, uint32_t NumMips);

	Color m_ClearColor;
	D3D12_CPU_DESCRIPTOR_HANDLE m_SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_RTVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE m_UAVHandle[12];
	uint32_t m_NumMipMaps;
	uint32_t m_FragmentCount;
	uint32_t m_SampleCount;
	DXGI_FORMAT m_format;
	bool m_IsCubeMap = false;
};
