#pragma once

// GpuResource的抽象基类




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

	~GpuResource() { Destroy(); }

	virtual void Destroy()
	{
		m_pResource = nullptr;
		m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;
		++m_VersionID;
	}
	
	ID3D12Resource* operator->() { return m_pResource.Get(); } // 运算符重载，允许直接通过GpuResource对象访问ID3D12Resource的方法
	const ID3D12Resource* operator->() const { return m_pResource.Get(); }

	ID3D12Resource* GetResource() { return m_pResource.Get(); }
	const ID3D12Resource* GetResource() const { return m_pResource.Get(); }

	ID3D12Resource** GetAddressOf() { return m_pResource.GetAddressOf(); }

	D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const { return m_GpuVirtualAddress; }

	uint32_t GetVersionID() const { return m_VersionID; }

protected:

	Microsoft::WRL::ComPtr<ID3D12Resource> m_pResource; // 资源指针

	D3D12_RESOURCE_STATES m_UsageState; // 当前资源状态
	D3D12_RESOURCE_STATES m_TransitioningState; // 过渡状态
	uint32_t m_VersionID = 0; // 标记资源版本ID，每次资源更新时递增，便于追踪资源变化
	D3D12_GPU_VIRTUAL_ADDRESS m_GpuVirtualAddress; // GPU虚拟地址

};

