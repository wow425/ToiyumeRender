#pragma once

// GpuResource的抽象基类
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include <functional>
#include <vector>




class GpuResource
{
	// 命令系统可访问
	friend class CommandContext;
	friend class GraphicsContext;
	friend class ComputeContext;

public:

	GpuResource() :
		m_GpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS_NULL),
		m_UsageState(D3D12_RESOURCE_STATE_COMMON),
		m_TransitioningState((D3D12_RESOURCE_STATES)-1) { }

	GpuResource(ID3D12Resource* pResource, D3D12_RESOURCE_STATES CurrentState) :
		m_GpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS_NULL),
		m_pResource(pResource),
		m_UsageState(CurrentState),
		m_TransitioningState((D3D12_RESOURCE_STATES)-1) {}

	// R2：析构必须是 virtual。
	// 原因：本基类被 ColorBuffer/DepthBuffer 等以多态方式持有
	// （shared_ptr<ColorBuffer>、shared_ptr<DepthBuffer> 并存），
	// 非虚析构在通过基类指针销毁时会跳过分派。
	// 更隐蔽的是下面这行 Destroy() —— 即使在虚析构里，
	// 基类析构体执行时动态类型也已退化为基类，虚调用依旧不生效，
	// 因此派生类各自补了显式析构体负责归还描述符。
	// 这里保留 Destroy() 作为 GpuResource 自身字段的兜底清理。
	virtual ~GpuResource() { Destroy(); }

	virtual void Destroy()
	{
		m_pResource = nullptr;
		m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;
		m_HeapType = kUnknownHeapType; // 资源已释放，堆类型缓存作废（下次重建重新查询）
		m_SubresourceStates.clear();
		++m_VersionID;
	}

	// [WP-1A / 契约 1：DX12 时钟 1 帧尾延迟释放入口]
	// 将底层 ID3D12Resource 的所有权转移进帧尾延迟释放队列，并可选附带 CPU 描述符 / BindlessIndex
	// 归还回调；随后立即清零本地地址与状态缓存并递增版本号。
	// 这样 CPU 可立刻重建/丢弃壳对象，而真实 COM 释放与槽位复用严格延迟至 GPU 越过 EndOfFrameFence。
	void ReleaseDeferred(Graphics::DeferredReleaseQueue& releaseQueue,
		std::function<void()> onGpuComplete = nullptr)
	{
		if (m_pResource != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> unk;
			m_pResource.As(&unk);
			releaseQueue.Enqueue(std::move(unk));
			m_pResource.Reset();
		}
		if (onGpuComplete)
		{
			releaseQueue.EnqueueCallback(std::move(onGpuComplete));
		}
		m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;
		m_HeapType = kUnknownHeapType;
		m_UsageState = D3D12_RESOURCE_STATE_COMMON;
		m_TransitioningState = (D3D12_RESOURCE_STATES)-1;
		m_SubresourceStates.clear();
		++m_VersionID;
	}

	// R3：资源所在堆的类型。
	// ------------------------------------------------------------
	// 存在的理由：D3D12 里有两类资源的状态是**创建时固定、不可转换**的 ——
	//   D3D12_HEAP_TYPE_UPLOAD   恒为 D3D12_RESOURCE_STATE_GENERIC_READ
	//   D3D12_HEAP_TYPE_READBACK 恒为 D3D12_RESOURCE_STATE_COPY_DEST
	// 对它们发 Transition 屏障，Debug Layer 立即报
	//   RESOURCE_MANIPULATION ERROR #741 RESOURCE_BARRIER_INVALID_HEAP
	// 并在 D3D12Core 里 RemoveCommandListAndThrow，进程直接终止。
	// CommandContext::TransitionResource 用这个查询来拦截这类屏障。
	//
	// 缓存：GetHeapProperties 是 CPU 侧查询（不会同步 GPU），但状态转换在热路径上，
	// 每帧几十次，缓存一次比反复查设备便宜。kUnknownHeapType 表示尚未查询。
	static constexpr D3D12_HEAP_TYPE kUnknownHeapType = (D3D12_HEAP_TYPE)-1;

	D3D12_HEAP_TYPE GetHeapType(void) const
	{
		if (m_HeapType == kUnknownHeapType)
		{
			D3D12_HEAP_PROPERTIES props = {};
			if (m_pResource != nullptr && SUCCEEDED(m_pResource->GetHeapProperties(&props, nullptr)))
				m_HeapType = props.Type;
			else
				m_HeapType = D3D12_HEAP_TYPE_DEFAULT; // 无资源时按默认堆处理（不会走到屏障）
		}
		return m_HeapType;
	}
	
	ID3D12Resource* operator->() { return m_pResource.Get(); } // 运算符重载，允许直接通过GpuResource对象访问ID3D12Resource的方法
	const ID3D12Resource* operator->() const { return m_pResource.Get(); }

	ID3D12Resource* GetResource() { return m_pResource.Get(); }
	const ID3D12Resource* GetResource() const { return m_pResource.Get(); }

	ID3D12Resource** GetAddressOf() { return m_pResource.GetAddressOf(); }

	D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const { return m_GpuVirtualAddress; }

	uint32_t GetVersionID() const { return m_VersionID; }

	// [WP-3A / 契约 3] 状态观测与按需子资源状态查询（单真源，常规路径 m_SubresourceStates 为空）
	D3D12_RESOURCE_STATES GetUsageState() const { return m_UsageState; }
	bool HasSubresourceStates() const { return !m_SubresourceStates.empty(); }

protected:

	Microsoft::WRL::ComPtr<ID3D12Resource> m_pResource; // 资源指针

	D3D12_RESOURCE_STATES m_UsageState; // 当前资源状态（全资源标量快速路径）
	D3D12_RESOURCE_STATES m_TransitioningState; // 过渡状态
	// [WP-3A / 契约 3] 按需懒展开的子资源状态表（仅当某 Pass 指定 Subresource != ALL_SUBRESOURCES 时分配，
	// 下一次全资源转换时自动收敛并清空，零常驻开销）。
	std::vector<D3D12_RESOURCE_STATES> m_SubresourceStates;
	uint32_t m_VersionID = 0; // 标记资源版本ID，每次资源更新时递增，便于追踪资源变化
	D3D12_GPU_VIRTUAL_ADDRESS m_GpuVirtualAddress; // GPU虚拟地址

	// R3：堆类型缓存（mutable 因为 GetHeapType 是 const 查询）。
	// 见 GetHeapType() 的说明：UPLOAD/READBACK 堆的资源状态固定，不能插转换屏障。
	mutable D3D12_HEAP_TYPE m_HeapType = kUnknownHeapType;

};

