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
// R2：Destroy 里要调用 Graphics::g_DescriptorAllocator.Free 归还描述符。
// GraphicsCore.h 并不包含任何 ColorBuffer/DepthBuffer，不存在循环依赖。
#include "02_RHI/GraphicsCore.h"
#include <array>


// 用作交换链Back Buffer和多渲染目标MRT
class ColorBuffer : public PixelBuffer
{
public:
	// R2 诊断：Destroy 被调用的次数 / 其中句柄已为 UNKNOWN 的次数
	// 定义放在 ColorBuffer.cpp，确保全程序只有一个实例
	static uint32_t s_DestroyCalls;
	static uint32_t s_DestroyAlreadyUnknown;
	// ============================================================
	// R2：显式析构体，确保 Destroy 走到本类实现
	// ------------------------------------------------------------
	// 坑：GpuResource 的析构函数里写的是  ~GpuResource() { Destroy(); }
	// 而 Destroy 是虚函数。C++ 的规则是 —— 基类析构体执行时，
	// 对象的动态类型已经退化为基类，虚调用不会再分派到派生类。
	// 因此 ~GpuResource 里的 Destroy() 永远调用 GpuResource::Destroy，
	// ColorBuffer::Destroy 只有在「显式调用」时才会执行。
	//
	// 实测后果：BufferManager 用 shared_ptr 持有 RT，交给 m_GBuffers 后
	// 走的是「析构」而非「显式 Destroy」路径 —— RTV/SRV/UAV 从未归还，
	// 每次 Resize 泄漏 24 个描述符，100 次后堆池由 8 涨到 36。
	//
	// 在自己的析构体里调用一次 Destroy 即可：此时动态类型仍是 ColorBuffer，
	// 虚分派正确命中本类实现。重复调用是安全的（句柄会先被置 UNKNOWN）。
	// ============================================================
	~ColorBuffer() { Destroy(); }

	ColorBuffer(Color ClearColor = Color(0.0f, 0.0f, 0.0f, 0.0f))
		: m_ClearColor(ClearColor), m_NumMipMaps(0), m_FragmentCount(1), m_SampleCount(1)
	{
		m_format = DXGI_FORMAT_UNKNOWN;
		m_RTVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		m_SRVHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
		for (int i = 0; i < _countof(m_UAVHandle); ++i)
			m_UAVHandle[i].ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
	}

	// ============================================================
	// 销毁资源并释放描述符句柄（R2）
	// ------------------------------------------------------------
	// 为什么必须重置句柄：CreateDerivedViews 用
	//   if (m_SRVHandle.ptr == UNKNOWN) { 分配句柄 }
	// 来判断「是否需要分配」。若不重置，二次 Create 会跳过分配，
	// 直接往**上一轮已指向已销毁资源的旧句柄**里写描述符。
	//
	// 后果有二：
	//   1) 描述符泄漏 —— 每次 Resize 都新建资源但不回收旧描述符槽
	//   2) 更隐蔽：旧句柄位置若已被别的资源复用，就会覆盖别人的视图，
	//      产生难以复现的渲染错乱
	//
	// 与 Texture::Destroy 保持一致的模式。
	// ============================================================
	virtual void Destroy() override
	{
		PixelBuffer::Destroy();

		// ============================================================
		// R2：归还描述符给分配器
		// ------------------------------------------------------------
		// 上面「重置为 UNKNOWN」只解决了「下次 Create 会重新分配」，
		// 但**没有把槽位还给分配器** —— 分配器只有 Allocate 没有 Free，
		// 于是每次 Resize 的 RTV/SRV/UAV 全部永久泄漏。
		// 实测：100 次 Resize 泄漏 4320 个描述符，堆池 8 -> 40。
		//
		// 这里必须先归还、再置 UNKNOWN：
		//   - 归还时用的是当前（有效）句柄
		//   - 置 UNKNOWN 供 CreateDerivedViews 判断「需要重新分配」
		//     （此时 Allocate 会命中刚归还的空闲段，实现复用）
		// ============================================================
		++s_DestroyCalls;
		if (m_RTVHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			++s_DestroyAlreadyUnknown;

		if (m_RTVHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].Free(m_RTVHandle, 1);

		if (m_SRVHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_SRVHandle, 1);

		for (int i = 0; i < _countof(m_UAVHandle); ++i)
		{
			if (m_UAVHandle[i].ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
				Graphics::g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].Free(m_UAVHandle[i], 1);
		}

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
