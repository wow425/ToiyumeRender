#include "PCH.h"
#include "DynamicDescriptorHeap.h"
#include "../Command/CommandContext.h"
#include "../GraphicsCore.h"
#include "../Command/CommandListManager.h"
#include "../PipelineState/RootSignature.h"

using namespace Graphics;

std::mutex DynamicDescriptorHeap::sm_Mutex;
// 0为资源描述符，1为采样器描述符
std::vector< Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> DynamicDescriptorHeap::sm_DescriptorHeapPool[2]; // 描述符池
std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>> DynamicDescriptorHeap::sm_RetiredDescriptorHeaps[2]; // 退休描述符队列
std::queue<ID3D12DescriptorHeap*> DynamicDescriptorHeap::sm_AvailableDescriptorHeaps[2];                   // 可用描述符队列

ID3D12DescriptorHeap* DynamicDescriptorHeap::RequestDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
{
	std::lock_guard<std::mutex> LockGuard(sm_Mutex);
	// 选择堆类型。是D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER为1，不是为0
	uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
	// 退休队列回收。退休队列不为空且任务执行完的进行回收
	while (!sm_RetiredDescriptorHeaps[idx].empty() && g_CommandManager.IsFenceComplete(sm_RetiredDescriptorHeaps[idx].front().first))
	{
		sm_AvailableDescriptorHeaps[idx].push(sm_RetiredDescriptorHeaps[idx].front().second);
		sm_RetiredDescriptorHeaps[idx].pop();
	}
	// 返回堆
	if (!sm_AvailableDescriptorHeaps[idx].empty()) // 可用队列可用,则返回第一个
	{
		ID3D12DescriptorHeap* HeapPtr = sm_AvailableDescriptorHeaps[idx].front();
		sm_AvailableDescriptorHeaps[idx].pop();
		return HeapPtr;
	}
	else // 可用队列为空，则新创一个
	{
		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.Type = HeapType;
		HeapDesc.NumDescriptors = kNumDescriptorsPerHeap;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HeapDesc.NodeMask = 1;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> HeapPtr;
		ASSERT_SUCCEEDED(g_Device->CreateDescriptorHeap(&HeapDesc, TY_IID_PPV_ARGS(&HeapPtr)));
		sm_DescriptorHeapPool[idx].emplace_back(HeapPtr);
		return HeapPtr.Get();
	}
}

// 回收用过的堆放入退休队列
void DynamicDescriptorHeap::DiscardDescriptorHeaps(D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint64_t FenceValue, const std::vector<ID3D12DescriptorHeap*>& UsedHeaps)
{
	uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
	std::lock_guard<std::mutex> LockGuard(sm_Mutex);
	for (auto iter = UsedHeaps.begin(); iter != UsedHeaps.end(); ++iter)
		sm_RetiredDescriptorHeaps[idx].push(std::make_pair(FenceValue, *iter));
}

// 退休当前堆
void DynamicDescriptorHeap::RetireCurrentHeap(void)
{
	// 未使用就不退休
	if (m_CurrentOffset == 0)
	{
		ASSERT(m_CurrentHeapPtr == nullptr)
			return;
	}

	ASSERT(m_CurrentHeapPtr != nullptr);
	m_RetiredHeaps.push_back(m_CurrentHeapPtr);
	m_CurrentHeapPtr = nullptr;
	m_CurrentOffset = 0;
}

// 退休已使用堆
void DynamicDescriptorHeap::RetireUsedHeaps(uint64_t fenceValue)
{
	DiscardDescriptorHeaps(m_DescriptorType, fenceValue, m_RetiredHeaps);
	m_RetiredHeaps.clear();
}

// 清空当前堆和已使用堆，句柄缓冲清空
void DynamicDescriptorHeap::CleanupUsedHeaps(uint64_t fenceValue)
{
	RetireCurrentHeap();
	RetireUsedHeaps(fenceValue);
	m_GraphicsHandleCache.ClearCache();
	m_ComputeHandleCache.ClearCache();
}

// 获取当前堆,不存在就申请个
inline ID3D12DescriptorHeap* DynamicDescriptorHeap::GetHeapPointer()
{
	if (m_CurrentHeapPtr == nullptr)
	{
		ASSERT(m_CurrentOffset == 0);
		m_CurrentHeapPtr = RequestDescriptorHeap(m_DescriptorType);
		m_FirstDescriptor = DescriptorHandle(
			m_CurrentHeapPtr->GetCPUDescriptorHandleForHeapStart(),
			m_CurrentHeapPtr->GetGPUDescriptorHandleForHeapStart()
		);
	}
	return m_CurrentHeapPtr;
}

// 计算出根签名所需描述符句柄个数
uint32_t DynamicDescriptorHeap::DescriptorHandleCache::ComputeStagedSize()
{
	uint32_t NeededSpace = 0; // 16个根描述符表的变动数据所需的空间大小的描述符个数
	uint32_t RootIndex;
	uint32_t StaleParams = m_StaleRootParamsBitMap; // 脏根参数掩码

	// _BitScanForward硬件指令，瞬间找到二进制中最低位（从右向左）的 1 的索引。比如找到了第 0 号参数，RootIndex 变为 0。
	while (_BitScanForward( (unsigned long*) & RootIndex, StaleParams))
	{
		StaleParams ^= (1 << RootIndex); // XOR，抹去找到的1，以进行下次while

		uint32_t MaxSetHandle;
		// _BitScanReverse 也是硬件指令（对应 BSR），瞬间找到某个表中被分配的最高位的 1 的索引。
		ASSERT(TRUE == _BitScanReverse( (unsigned long*)&MaxSetHandle, m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap),
			"Root entry marked as stale but has no stale descriptors");

		NeededSpace += MaxSetHandle + 1;
	}
	return NeededSpace;
}

void DynamicDescriptorHeap::UnbindAllValid(void)
{
	m_GraphicsHandleCache.UnbindAllValid();
	m_ComputeHandleCache.UnbindAllValid();
}

/*这通常发生在动态描述符堆（Dynamic Descriptor Heap）当前的空间耗尽，引擎被迫申请了一块全新的 GPU 描述符堆的时候。
当你把底层的 GPU 内存换了新的一块，之前绑定在旧堆上的所有描述符就全部失效（失效 / Invalidation / 無効化[むこうか]）了。
 为了让下一次 Draw Call 能正常运行，引擎必须把 CPU 缓存里那些仍然有效的资源，全部打上“脏标记”，强迫它们在提交时被重新拷贝到这个新的 GPU 堆里。
 */
// 全量标记当前已绑定的根表为‘脏’状态，以强制触发下一帧或下次绘制时的 GPU 同步  重新标记根签名cpu端处缓存已绑定的根表,m_StaleRootParamsBitMap
void DynamicDescriptorHeap::DescriptorHandleCache::UnbindAllValid()
{
	m_StaleRootParamsBitMap = 0; // 脏根参数位图清零

	unsigned long TableParams = m_RootDescriptorTablesBitMap;
	unsigned long RootIndex;
	while (_BitScanForward(&RootIndex, TableParams))
	{
		TableParams ^= (1 << RootIndex);
		if (m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap != 0) 
			m_StaleRootParamsBitMap |= (1 << RootIndex);
	}
}
// 将单一CPU描述符拷贝到shader visible dynamicdescriptor heap中，并返回资源在GPU端堆上的句柄
D3D12_GPU_DESCRIPTOR_HANDLE DynamicDescriptorHeap::UploadDirect(D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
	// 当前堆存在且容量不溢出,就直接寻址，拷贝资源句柄即可。否则申请新堆并重新标记已绑定的根表 
	if (!HasSpace(1)) 
	{
		RetireCurrentHeap(); // 退休当前堆
		UnbindAllValid(); // 全量标记当前已绑定的根表为‘脏’状态，以强制触发下一帧或下次绘制时的 GPU 同步
	}

	m_OwningContext.SetDescriptorHeap(m_DescriptorType, GetHeapPointer());
	DescriptorHandle DestHandle = m_FirstDescriptor + m_CurrentOffset * m_DescriptorSize;
	m_CurrentOffset += 1;
	// 直接将资源句柄Handle拷贝到shader visible heap上
	g_Device->CopyDescriptorsSimple(1, DestHandle, Handle, m_DescriptorType); 

	return DestHandle;
}

// 资源句柄绑定在RootIndex根参数的CPU端处缓存
void DynamicDescriptorHeap::DescriptorHandleCache::StageDescriptorHandles(UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
	// 确保RootIndex是描述符表
	ASSERT( ( (1 << RootIndex) & m_RootDescriptorTablesBitMap) != 0, "Root parameter is not a CBV_SRV_UAV descriptor table");
	// 防止缓冲区溢出
	ASSERT(Offset + NumHandles <= m_RootDescriptorTable[RootIndex].TableSize);

	DescriptorTableCache& TableCache = m_RootDescriptorTable[RootIndex]; // 获取对应根描述符表的CPU端处缓存
	D3D12_CPU_DESCRIPTOR_HANDLE* CopyDest = TableCache.TableStart + Offset; // 做偏移，指向该根描述符表的缓存的首地址
	for (UINT i = 0; i < NumHandles; ++i) 
		CopyDest[i] = Handles[i];
	// 标记已分配句柄
	TableCache.AssignedHandlesBitMap |= ((1 << NumHandles) - 1) << Offset;
	// 标记对应索引的根参数脏标记
	m_StaleRootParamsBitMap |= (1 << RootIndex);
}

// 解析根签名，为新的根签名创建CPU端的缓存
void DynamicDescriptorHeap::DescriptorHandleCache::ParseRootSignature(D3D12_DESCRIPTOR_HEAP_TYPE Type, const RootSignature& RootSig)
{
	ASSERT(RootSig.m_NumParameters <= 16, "RootSig.m_NumParameters must is <= 16");

	UINT CurrentOffset = 0;

	m_StaleRootParamsBitMap = 0; // 
	// 提取对应类型的布局掩码
	m_RootDescriptorTablesBitMap = (Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? RootSig.m_SamplerTableBitMap : RootSig.m_DescriptorTableBitMap);

	unsigned long TableParams = m_RootDescriptorTablesBitMap;
	unsigned long RootIndex;
	// 位扫描循环
	// 为根签名对应类型的根参数分配缓冲
	while (_BitScanForward(&RootIndex, TableParams))
	{
		TableParams ^= (1 << RootIndex); // 抹零
		UINT TableSize = RootSig.m_DescriptorTableSize[RootIndex]; // 查表，获取该描述符表容纳的描述符数量
		ASSERT(TableSize > 0);
		// 内存切片绑定
		DescriptorTableCache& RootDescriptorTable = m_RootDescriptorTable[RootIndex];
		RootDescriptorTable.AssignedHandlesBitMap = 0;
		// 数组参与加法运算，自动数组退化，编程指向数组第0个元素的首地址指针
		RootDescriptorTable.TableStart = m_HandleCache + CurrentOffset;
		RootDescriptorTable.TableSize = TableSize;

		CurrentOffset += TableSize;
	}
	m_MaxCachedDescriptors = CurrentOffset;
	ASSERT(m_MaxCachedDescriptors <= kMaxNumDescriptorTables, "Exceeded user-supplied maximum cache size");
}

// 把发生了变化的（Stale）描述符，从零散的 CPU 缓存中，紧凑地拷贝到 GPU 堆，并绑定到命令列表
void DynamicDescriptorHeap::DescriptorHandleCache::CopyAndBindStaleTables(
	D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t DescriptorSize,
	DescriptorHandle DestHandleStart, ID3D12GraphicsCommandList* CmdList,
	void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
	uint32_t StaleParamCount = 0; // 脏根参数
	uint32_t TableSize[DescriptorHandleCache::kMaxNumDescriptorTables];
	uint32_t RootIndices[DescriptorHandleCache::kMaxNumDescriptorTables];
	uint32_t NeededSpace = 0;
	uint32_t RootIndex;

	/*
	Sum the maximum assigned offsets of stale descriptor tables to determine total needed space.
	计算出脏根表的最大已分配偏移总合以决定所需总大小
	*/
// 1. 统计脏根参已绑定句柄的总大小NeededSpace。并CPU端处缓存一一设置脏根参的RootIndex和已分配句柄
	uint32_t StaleParams = m_StaleRootParamsBitMap;
	while (_BitScanForward((unsigned long*)&RootIndex, StaleParams))
	{
		RootIndices[StaleParamCount] = RootIndex; // 记录RootIndex根参索引
		StaleParams ^= (1 << RootIndex);

		uint32_t MaxSetHandle;
		ASSERT(TRUE == _BitScanReverse((unsigned long*)&MaxSetHandle, m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap),
			"Root entry marked as stale but has no stale descriptors");

		NeededSpace += MaxSetHandle + 1;
		TableSize[StaleParamCount] = MaxSetHandle + 1; // 记录RootIndex根参的已分配句柄数量

		++StaleParamCount;
	}
	ASSERT(StaleParamCount <= DescriptorHandleCache::kMaxNumDescriptorTables, // 断言，脏根参数量不超
		"We're only equipped to handle so many descriptor tables");

	m_StaleRootParamsBitMap = 0;
	

// 2. 批量拷贝优化Batch Copying。DX12 的 API 调用有开销，不要一个一个拷贝，而是收集一批后一次性交给 g_Device->CopyDescriptors
	// 声明所需
	static const uint32_t kMaxDescriptorsPerCopy = 16; // 每批最大拷贝描述符数量16
	UINT NumDestDescriptorRanges = 0; // 目的地描述符范围
	D3D12_CPU_DESCRIPTOR_HANDLE pDestDescriptorRangeStarts[kMaxDescriptorsPerCopy]; // 目的地描述符范围起点
	UINT pDestDescriptorRangeSizes[kMaxDescriptorsPerCopy]; // 

	UINT NumSrcDescriptorRanges = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE pSrcDescriptorRangeStarts[kMaxDescriptorsPerCopy];
	UINT pSrcDescriptorRangeSizes[kMaxDescriptorsPerCopy];

	for (uint32_t i = 0; i < StaleParamCount; ++i) // 遍历脏参根
	{
		RootIndex = RootIndices[i]; // 获取RootIndex根参索引
		// GPU句柄绑定到 Command List。SetGraphicsRootDescriptorTable/SetComputeRootDescriptorTable。根签名的根参对应索引与堆上地址
		(CmdList->*SetFunc)(RootIndex, DestHandleStart);

		DescriptorTableCache& RootDescTable = m_RootDescriptorTable[RootIndex]; // 获取RootIndex根表

		D3D12_CPU_DESCRIPTOR_HANDLE* SrcHandles = RootDescTable.TableStart;
		uint64_t SetHandles = (uint64_t)RootDescTable.AssignedHandlesBitMap; // RootIndex根表的句柄位图
		D3D12_CPU_DESCRIPTOR_HANDLE CurDest = DestHandleStart;
		DestHandleStart += TableSize[i] * DescriptorSize; // 地址偏移到RootIndex根表

		// 跳过没有被赋值的“空洞”，只拷贝有效数据
		unsigned long SkipCount;
		while (_BitScanForward64(&SkipCount, SetHandles)) // 遍历RootIndex根表的已分配句柄位图AssignedHandlesBitMap
		{
			// 跨域空洞，无效位
			SetHandles >>= SkipCount; // 右移掉无效位
			SrcHandles += SkipCount; //  源地址指针偏移掉无效位
			CurDest.ptr += SkipCount * DescriptorSize; // 描述符句柄地址偏移掉无效位的地址

			unsigned long DescriptorCount;
			// ~位图取反，计算连续有几个有效的 Handle
			_BitScanForward64(&DescriptorCount, ~SetHandles);
			SetHandles >>= DescriptorCount;

			// 至少攒够了 16 个，就触发一次 CopyDescriptors 以降低 API 开销
			if (NumSrcDescriptorRanges + DescriptorCount > kMaxDescriptorsPerCopy)
			{
				g_Device->CopyDescriptors(
					NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
					NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
					Type);
				// 重置计数器
				NumSrcDescriptorRanges = 0;
				NumDestDescriptorRanges = 0;
			}

			// Setup destination range
			pDestDescriptorRangeStarts[NumDestDescriptorRanges] = CurDest;
			pDestDescriptorRangeSizes[NumDestDescriptorRanges] = DescriptorCount;
			++NumDestDescriptorRanges;

			// Setup source ranges (one descriptor each because we don't assume they are contiguous)
			for (uint32_t j = 0; j < DescriptorCount; ++j)
			{
				pSrcDescriptorRangeStarts[NumSrcDescriptorRanges] = SrcHandles[j];
				pSrcDescriptorRangeSizes[NumSrcDescriptorRanges] = 1;
				++NumSrcDescriptorRanges;
			}

			// Move the destination pointer forward by the number of descriptors we will copy
			SrcHandles += DescriptorCount;
			CurDest.ptr += DescriptorCount * DescriptorSize;
		}
	}
	// 收尾：把最后剩下的一批拷贝过去
	g_Device->CopyDescriptors(
		NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, // 目的地的
		NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,    // 源
		Type);
}

void DynamicDescriptorHeap::CopyAndBindStagedTables(DescriptorHandleCache& HandleCache, ID3D12GraphicsCommandList* CmdList,
	void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
	uint32_t NeededSize = HandleCache.ComputeStagedSize(); // 获取所需描述符个数
	if (!HasSpace(NeededSize)) // 堆容纳不下的话
	{
		RetireCurrentHeap();
		UnbindAllValid(); // 解绑
		NeededSize = HandleCache.ComputeStagedSize(); // 重新获取所需描述符个数
	}

	// This can trigger触发 the creation of a new heap
	m_OwningContext.SetDescriptorHeap(m_DescriptorType, GetHeapPointer()); // 设置堆
	HandleCache.CopyAndBindStaleTables(m_DescriptorType, m_DescriptorSize, Allocate(NeededSize), CmdList, SetFunc); // 上传
}