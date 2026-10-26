#include "00_Core/PCH.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"

using namespace Graphics;

//
// DescriptorAllocator implementation
//
std::mutex DescriptorAllocator::sm_AllocationMutex;
std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> DescriptorAllocator::sm_DescriptorHeapPool;

void DescriptorAllocator::DestroyAll(void)
{
	sm_DescriptorHeapPool.clear();
}
// 申请新堆
ID3D12DescriptorHeap* DescriptorAllocator::RequestNewHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type)
{
	std::lock_guard<std::mutex> LockGuard(sm_AllocationMutex);

	D3D12_DESCRIPTOR_HEAP_DESC Desc;
	Desc.Type = Type;
	Desc.NumDescriptors = sm_NumDescriptorsPerHeap;
	Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	Desc.NodeMask = 1;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pHeap;
	ASSERT_SUCCEEDED(Graphics::g_Device->CreateDescriptorHeap(&Desc, TY_IID_PPV_ARGS(&pHeap)));

	sm_DescriptorHeapPool.emplace_back(pHeap);
	return pHeap.Get();
}

namespace
{
	// 把「堆内偏移」换算成 CPU 描述符句柄。
	// 之所以按 (堆, 偏移) 而不是直接按句柄地址记账：
	//   CPU 句柄地址在多堆场景下不连续，无法用一次 memcpy 复用整段；
	//   而 (堆, 偏移, 数量) 三元组既不依赖地址，也能直接定位句柄。
	inline D3D12_CPU_DESCRIPTOR_HANDLE MakeHandle(ID3D12DescriptorHeap* Heap,
		uint32_t Offset, uint32_t DescriptorSize)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = Heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)Offset * DescriptorSize;
		return h;
	}
}

// 分配，前端接口
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocator::Allocate(uint32_t Count)
{
	if (m_DescriptorSize == 0)
		m_DescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(m_Type);

	// ============================================================
	// R2：优先复用已回收的槽位（首次适配）
	// ------------------------------------------------------------
	// 先查空闲表，再走线性游标。这样 Resize 反复销毁/重建 RT 时
	// 描述符总量会稳定在一个高水位，而不是每次都新开堆。
	// ============================================================
	for (size_t i = 0; i < m_FreeRanges.size(); ++i)
	{
		// 必须按值拷贝：下面的 swap-erase 会覆盖 m_FreeRanges[i]，
		// 若这里用引用，后续读 r.Offset / r.HeapIndex 就是读到被换进来的
		// 另一段区间 —— 位图会登记到完全错误的位置。
		const FreeRange r = m_FreeRanges[i];
		if (r.Count < Count)
			continue;

		D3D12_CPU_DESCRIPTOR_HANDLE ret = MakeHandle(sm_DescriptorHeapPool[r.HeapIndex].Get(),
			r.Offset, m_DescriptorSize);

		if (r.Count == Count)
		{
			// 整段用完：从空闲表移除（swap-erase，顺序无关）
			m_FreeRanges[i] = m_FreeRanges.back();
			m_FreeRanges.pop_back();
		}
		else
		{
			// 切走前半段，剩余部分前移
			m_FreeRanges[i].Offset += Count;
			m_FreeRanges[i].Count  -= Count;
		}

		for (uint32_t k = 0; k < Count; ++k)
			SetBit(r.HeapIndex, r.Offset + k);

		m_TotalAllocated += Count;
		m_LiveAllocated += Count;
		return ret;
	}

	// 无可用空闲段：走线性游标；当前堆不够就新开一堆
	if (m_CurrentHeap == nullptr || m_RemainingFreeHandles < Count)
	{
		m_CurrentHeap = RequestNewHeap(m_Type);
		m_CurrentHandle = m_CurrentHeap->GetCPUDescriptorHandleForHeapStart();
		m_RemainingFreeHandles = sm_NumDescriptorsPerHeap;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE ret = m_CurrentHandle;
	m_CurrentHandle.ptr += Count * m_DescriptorSize;
	m_RemainingFreeHandles -= Count;

	// R2：登记本次分配的槽位，供 Free 做严格一次校验。
	// 注意不要用「堆池大小 - 1」猜测堆索引：RequestNewHeap 只在
	// 当前堆耗尽时调用，此时新堆确实在末尾；但若将来复用堆池，
	// 该假设立刻失效。这里直接从返回的句柄地址反查堆索引，
	// 与 Free 使用完全相同的定位逻辑，保证两侧一致。
	{
		for (uint32_t hi = 0; hi < (uint32_t)sm_DescriptorHeapPool.size(); ++hi)
		{
			const SIZE_T base = sm_DescriptorHeapPool[hi]->GetCPUDescriptorHandleForHeapStart().ptr;
			const SIZE_T end  = base + (SIZE_T)sm_NumDescriptorsPerHeap * m_DescriptorSize;
			if (ret.ptr < base || ret.ptr >= end)
				continue;

			const uint32_t offset = (uint32_t)((ret.ptr - base) / m_DescriptorSize);
			for (uint32_t k = 0; k < Count; ++k)
				SetBit(hi, offset + k);
			break;
		}
	}

	m_TotalAllocated += Count;
	m_LiveAllocated += Count;

	return ret;
}

// ============================================================
// R2：回收描述符
// ------------------------------------------------------------
// 把句柄反向定位到 (堆, 偏移) 后登记为空闲区间。
// 定位方式是「遍历堆池比对地址区间」—— 堆最多几十个，
// 相对 Resize 本身的代价可以忽略，换来不必在每个句柄里附带元数据。
// ============================================================
void DescriptorAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE Handle, uint32_t Count)
{
	if (Handle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN || Count == 0)
		return;

	// ============================================================
	// R2 修复：静态析构顺序崩溃
	// ------------------------------------------------------------
	// 全局对象（例如 s_EnvironmentLightingManager 持有的 ColorBuffer）
	// 的析构发生在 main 返回之后，由 atexit 触发。此时
	// Graphics::Shutdown 早已执行完毕：g_Device 已释放、堆池已 clear。
	//
	// 若这里照常往下走，会先解引用已为空的 g_Device
	// （GetDescriptorHandleIncrementSize），或遍历空的堆池后
	// 走到末尾的 ASSERT —— 两者都会在进程退出阶段产生
	// 0xC0000005（实测 exit 139，且崩溃栈落在 atexit 里，
	// 很容易被误判成「只是退出时的噪音」而长期忽略）。
	//
	// 关闭之后已无任何 GPU 资源语义，描述符无需也无法归还，
	// 直接返回即可。
	// ============================================================
	if (Graphics::g_Device == nullptr || sm_DescriptorHeapPool.empty())
		return;

	const uint32_t descSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(m_Type);

	for (uint32_t hi = 0; hi < (uint32_t)sm_DescriptorHeapPool.size(); ++hi)
	{
		ID3D12DescriptorHeap* heap = sm_DescriptorHeapPool[hi].Get();
		const SIZE_T base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
		const SIZE_T end  = base + (SIZE_T)sm_NumDescriptorsPerHeap * descSize;

		if (Handle.ptr < base || Handle.ptr >= end)
			continue;

		const uint32_t offset = (uint32_t)((Handle.ptr - base) / descSize);

		// R2：严格一次语义 —— 逐位确认本段确实处于「已分配」状态。
		// 之所以逐位而不是整段检查：调用方可能把一段连续分配拆开归还，
		// 只应把真正属于它的那几位清零。
		uint32_t owned = 0;
		for (uint32_t i = 0; i < Count; ++i)
		{
			if (!TestAndClearBit(hi, offset + i))
				break;
			++owned;
		}

		if (owned == 0)
		{
			// 这一整段都不是「已分配」状态：重复归还，忽略。
			++m_DoubleFreeCount;
			return;
		}

		// 与相邻空闲段合并，避免反复分配/回收产生碎片
		for (auto& r : m_FreeRanges)
		{
			if (r.HeapIndex == hi && r.Offset + r.Count == offset)
			{
				r.Count += owned;
				m_LiveAllocated -= owned;
				m_FreedCount += owned;
				return;
			}
			if (r.HeapIndex == hi && offset + owned == r.Offset)
			{
				r.Offset = offset;
				r.Count += owned;
				m_LiveAllocated -= owned;
				m_FreedCount += owned;
				return;
			}
		}

		m_FreeRanges.push_back({ hi, offset, owned });
		m_LiveAllocated -= owned;
		m_FreedCount += owned;
		return;
	}

	// 未命中任何堆：说明句柄不是本分配器发出的，属于调用方 bug。
	ASSERT(false, "DescriptorAllocator::Free got a handle not owned by this allocator");
}

// ============================================================
// R2：已分配位图维护
// ============================================================
void DescriptorAllocator::EnsureBits(uint32_t HeapIndex)
{
	if (m_AllocatedBits.size() <= HeapIndex)
		m_AllocatedBits.resize(HeapIndex + 1);

	const uint32_t words = (sm_NumDescriptorsPerHeap + 63) / 64;
	if (m_AllocatedBits[HeapIndex].size() != words)
		m_AllocatedBits[HeapIndex].assign(words, 0ull);
}

void DescriptorAllocator::SetBit(uint32_t HeapIndex, uint32_t Offset)
{
	EnsureBits(HeapIndex);
	m_AllocatedBits[HeapIndex][Offset >> 6] |= (1ull << (Offset & 63));
}

bool DescriptorAllocator::TestAndClearBit(uint32_t HeapIndex, uint32_t Offset)
{
	EnsureBits(HeapIndex);
	uint64_t& w = m_AllocatedBits[HeapIndex][Offset >> 6];
	const uint64_t mask = (1ull << (Offset & 63));
	if ((w & mask) == 0)
		return false;
	w &= ~mask;
	return true;
}

//
// DescriptorHeap implementation
//

// 创建shader可视的描述符堆
void DescriptorHeap::Create(const std::wstring& Name, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t MaxCount)
{
	m_HeapDesc.Type = Type;
	m_HeapDesc.NumDescriptors = MaxCount;
	m_HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	m_HeapDesc.NodeMask = 1;

	ASSERT_SUCCEEDED(g_Device->CreateDescriptorHeap(&m_HeapDesc, TY_IID_PPV_ARGS(m_Heap.ReleaseAndGetAddressOf())));

#ifdef RELEASE
	(void)Name;
#else
	m_Heap->SetName(Name.c_str());
#endif

	m_DescriptorSize = g_Device->GetDescriptorHandleIncrementSize(m_HeapDesc.Type);
	m_NumFreeDescriptors = m_HeapDesc.NumDescriptors;
	m_FirstHandle = DescriptorHandle(
		m_Heap->GetCPUDescriptorHandleForHeapStart(),
		m_Heap->GetGPUDescriptorHandleForHeapStart());
	m_NextFreeHandle = m_FirstHandle;

	// R3：堆的代号 +1。游标在上面的 m_NextFreeHandle = m_FirstHandle 处归零，
	// 从此同一偏移可能指向与上一代完全不同的描述符 —— 让缓存方靠代际号识别失效。
	// 只在 Create 成功末尾自增（前面 ASSERT_SUCCEEDED 已经兜住失败路径），
	// Destroy 刻意不复位，保证单调递增、不会出现 ABA。
	++m_Generation;
}

DescriptorHandle DescriptorHeap::Alloc(uint32_t Count)
{
	// 检查容量
	ASSERT(HasAvailableSpace(Count), "Descriptor Heap out of space.Increase heap size.");
	// 返回当前游标句柄，并移动游标和可用容量减小
	DescriptorHandle ret = m_NextFreeHandle;      // 当前游标更新
	m_NextFreeHandle += Count * m_DescriptorSize; // next游标偏移
	m_NumFreeDescriptors -= Count;                // 可用容量缩减
	return ret;
}

// 检验句柄合法不和是否属于当前堆中的
bool DescriptorHeap::ValidateHandle(const DescriptorHandle& DHandle) const
{
	// 绝对边界检查。起始地址之前或最大地址之后
	if (DHandle.GetCpuPtr() < m_FirstHandle.GetCpuPtr() ||
		DHandle.GetCpuPtr() >= m_FirstHandle.GetCpuPtr() + m_HeapDesc.NumDescriptors * m_DescriptorSize)
		return false;

	// 相对偏移一致性检查。确保gpu跟cpu两个虚拟地址偏移距离相同
	if (DHandle.GetGpuPtr() - m_FirstHandle.GetGpuPtr() !=
		DHandle.GetCpuPtr() - m_FirstHandle.GetCpuPtr())
		return false;

	return true;
}
