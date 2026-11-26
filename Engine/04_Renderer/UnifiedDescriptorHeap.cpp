#include "00_Core/PCH.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"

namespace Renderer
{
	bool UnifiedDescriptorHeap::Initialize()
	{
		if (m_Initialized)
			return true;

		if (Graphics::g_Device == nullptr)
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] Initialize 失败：Graphics::g_Device 为空\n");
			return false;
		}

		// 1. 创建全局唯一 65,536 槽 CBV_SRV_UAV Shader-Visible 堆
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = kTotalCbvSrvUavSlots;
			desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			desc.NodeMask       = 1;

			const HRESULT hr = Graphics::g_Device->CreateDescriptorHeap(&desc, TY_IID_PPV_ARGS(&m_CbvSrvUavHeap));
			if (FAILED(hr))
			{
				LOG_ERROR(L"[UnifiedDescriptorHeap] 创建 CBV_SRV_UAV 堆失败 (slots=%u, hr=0x%08X)\n",
					kTotalCbvSrvUavSlots, (unsigned)hr);
				return false;
			}
			m_CbvSrvUavHeap->SetName(L"UnifiedDescriptorHeap_CBV_SRV_UAV_64K");
			m_CbvSrvUavDescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			m_CbvSrvUavCpuStart       = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
			m_CbvSrvUavGpuStart       = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
		}

		// 2. 创建全局唯一 2,048 槽 SAMPLER Shader-Visible 堆
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
			desc.NumDescriptors = kTotalSamplerSlots;
			desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			desc.NodeMask       = 1;

			const HRESULT hr = Graphics::g_Device->CreateDescriptorHeap(&desc, TY_IID_PPV_ARGS(&m_SamplerHeap));
			if (FAILED(hr))
			{
				LOG_ERROR(L"[UnifiedDescriptorHeap] 创建 SAMPLER 堆失败 (slots=%u, hr=0x%08X)\n",
					kTotalSamplerSlots, (unsigned)hr);
				m_CbvSrvUavHeap.Reset();
				return false;
			}
			m_SamplerHeap->SetName(L"UnifiedDescriptorHeap_SAMPLER_2K");
			m_SamplerDescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			m_SamplerCpuStart       = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
			m_SamplerGpuStart       = m_SamplerHeap->GetGPUDescriptorHandleForHeapStart();
		}

		// 3. 复位各分区游标与空闲表（代际号单调递增，避免 ABA）
		++m_Generation;
		m_LifetimeToken = std::make_shared<uint32_t>(m_Generation);

		m_ZoneANextCursor = kZoneAFirstDynamicSlot;
		m_ZoneAFreeSingle.clear();
		m_ZoneAFreeMaterial5.clear();
		m_ZoneALiveCount = kZoneAFirstDynamicSlot; // 4 个默认哨兵常驻

		m_ZoneBFrameSlot = 0;
		m_ZoneBFrameCursor = 0;

		m_ZoneCNextCursor = kZoneCStart;
		m_ZoneCFreeRanges.clear();
		m_ZoneCLiveCount = 0;

		m_ZoneDUsed.fill(false);
		m_ZoneDLiveCount = 0;

		m_SamplerNextCursor = 0;

		// 4. 先置 m_Initialized = true（两张堆与游标均已就绪），再固化 Zone A 槽 0..3 默认哨兵
		//    （PopulateDefaultSentinels 会调用 GetCbvSrvUavHandle(0..3)，依赖 m_Initialized 标志）。
		m_Initialized = true;
		PopulateDefaultSentinels();

		LOG_INFO(L"[UnifiedDescriptorHeap] 初始化完成 (gen=%u, CBV_SRV_UAV=%u, SAMPLER=%u, ZoneA=[0,%u), ZoneB=[%u,%u), ZoneC=[%u,%u), ZoneD=[%u,%u))\n",
			m_Generation, kTotalCbvSrvUavSlots, kTotalSamplerSlots,
			kZoneAEnd, kZoneBStart, kZoneBStart + kZoneBCount,
			kZoneCStart, kZoneCEnd, kZoneDStart, kZoneDEnd);
		return true;
	}

	void UnifiedDescriptorHeap::PopulateDefaultSentinels()
	{
		// 槽 0: White2D；槽 1: Black2D；槽 2: FlatNormal2D
		const D3D12_CPU_DESCRIPTOR_HANDLE srcWhite  = Graphics::GetDefaultTexture(Graphics::kWhiteOpaque2D);
		const D3D12_CPU_DESCRIPTOR_HANDLE srcBlack  = Graphics::GetDefaultTexture(Graphics::kBlackOpaque2D);
		const D3D12_CPU_DESCRIPTOR_HANDLE srcNormal = Graphics::GetDefaultTexture(Graphics::kDefaultNormalMap);

		if (srcWhite.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_Device->CopyDescriptorsSimple(1, GetCbvSrvUavHandle(kSentinelWhite2D).Cpu, srcWhite, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (srcBlack.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_Device->CopyDescriptorsSimple(1, GetCbvSrvUavHandle(kSentinelBlack2D).Cpu, srcBlack, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (srcNormal.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			Graphics::g_Device->CopyDescriptorsSimple(1, GetCbvSrvUavHandle(kSentinelFlatNormal2D).Cpu, srcNormal, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// 槽 3: ZeroByteBuffer（16 字节全零 Raw ByteAddressBuffer SRV，供 Bindless 缺失缓冲回退读取）
		alignas(16) const uint32_t zeroData[4] = { 0, 0, 0, 0 };
		m_ZeroSentinelBuffer.Create(L"UnifiedHeap_SentinelZeroByteBuffer", 4, sizeof(uint32_t), zeroData);
		if (m_ZeroSentinelBuffer.GetSRV().ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
		{
			Graphics::g_Device->CopyDescriptorsSimple(
				1, GetCbvSrvUavHandle(kSentinelZeroByteBuffer).Cpu,
				m_ZeroSentinelBuffer.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
	}

	void UnifiedDescriptorHeap::Shutdown()
	{
		if (!m_Initialized)
			return;

		// 先销毁存活令牌，令任何滞留在 DeferredReleaseQueue 中的归还回调自动失效
		m_LifetimeToken.reset();

		m_ZeroSentinelBuffer.Destroy();
		m_CbvSrvUavHeap.Reset();
		m_SamplerHeap.Reset();

		m_CbvSrvUavCpuStart = { 0 };
		m_CbvSrvUavGpuStart = { 0 };
		m_CbvSrvUavDescriptorSize = 0;
		m_SamplerCpuStart = { 0 };
		m_SamplerGpuStart = { 0 };
		m_SamplerDescriptorSize = 0;

		m_ZoneAFreeSingle.clear();
		m_ZoneAFreeMaterial5.clear();
		m_ZoneCFreeRanges.clear();
		m_ZoneDUsed.fill(false);

		m_ZoneANextCursor = kZoneAFirstDynamicSlot;
		m_ZoneALiveCount = 0;
		m_ZoneBFrameSlot = 0;
		m_ZoneBFrameCursor = 0;
		m_ZoneCNextCursor = kZoneCStart;
		m_ZoneCLiveCount = 0;
		m_ZoneDLiveCount = 0;
		m_SamplerNextCursor = 0;

		m_Initialized = false;
	}

	void UnifiedDescriptorHeap::BeginFrame(uint32_t frameIndex)
	{
		if (!m_Initialized)
			return;

		// 3 帧环形槽区切换：每个槽区独占 5,461 槽，受帧延迟同步保护，零碎片复位。
		m_ZoneBFrameSlot   = frameIndex % kZoneBFramesInFlight;
		m_ZoneBFrameCursor = 0;
	}

	void UnifiedDescriptorHeap::BindToContext(CommandContext& context) const
	{
		if (!m_Initialized)
			return;

		// 单次合并调用 SetDescriptorHeaps(2, {CBV_SRV_UAV, SAMPLER})，
		// 避免连续两次 SetDescriptorHeap 触发不必要的第二次 API 调用。
		D3D12_DESCRIPTOR_HEAP_TYPE types[2] = {
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
		};
		ID3D12DescriptorHeap* heaps[2] = {
			m_CbvSrvUavHeap.Get(),
			m_SamplerHeap.Get()
		};
		context.SetDescriptorHeaps(2, types, heaps);
	}

	BindlessAllocation UnifiedDescriptorHeap::GetCbvSrvUavHandle(uint32_t heapIndex) const
	{
		BindlessAllocation out = {};
		if (!m_Initialized || heapIndex >= kTotalCbvSrvUavSlots)
			return out;

		out.HeapIndex = heapIndex;
		out.Cpu.ptr   = m_CbvSrvUavCpuStart.ptr + static_cast<SIZE_T>(heapIndex) * m_CbvSrvUavDescriptorSize;
		out.Gpu.ptr   = m_CbvSrvUavGpuStart.ptr + static_cast<UINT64>(heapIndex) * m_CbvSrvUavDescriptorSize;
		return out;
	}

	BindlessAllocation UnifiedDescriptorHeap::GetSamplerHandle(uint32_t heapIndex) const
	{
		BindlessAllocation out = {};
		if (!m_Initialized || heapIndex >= kTotalSamplerSlots)
			return out;

		out.HeapIndex = heapIndex;
		out.Cpu.ptr   = m_SamplerCpuStart.ptr + static_cast<SIZE_T>(heapIndex) * m_SamplerDescriptorSize;
		out.Gpu.ptr   = m_SamplerGpuStart.ptr + static_cast<UINT64>(heapIndex) * m_SamplerDescriptorSize;
		return out;
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocPersistentSingle()
	{
		if (!m_Initialized)
			return {};

		uint32_t slot = kInvalidBindlessIndex;
		if (!m_ZoneAFreeSingle.empty())
		{
			slot = m_ZoneAFreeSingle.back();
			m_ZoneAFreeSingle.pop_back();
		}
		else if (m_ZoneANextCursor < kZoneAEnd)
		{
			slot = m_ZoneANextCursor++;
		}
		else
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] Zone A 单槽分配耗尽（上限 %u）\n", kZoneACount);
			ASSERT(false, "UnifiedDescriptorHeap Zone A exhausted");
			return {};
		}

		++m_ZoneALiveCount;
		return GetCbvSrvUavHandle(slot);
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocPersistentMaterialBlock(uint32_t count)
	{
		if (!m_Initialized || count == 0)
			return {};

		uint32_t startSlot = kInvalidBindlessIndex;
		if (count == kMaterialBlockSize && !m_ZoneAFreeMaterial5.empty())
		{
			startSlot = m_ZoneAFreeMaterial5.back();
			m_ZoneAFreeMaterial5.pop_back();
		}
		else if (m_ZoneANextCursor + count <= kZoneAEnd)
		{
			startSlot = m_ZoneANextCursor;
			m_ZoneANextCursor += count;
		}
		else
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] Zone A 连续材质块分配耗尽（请求 %u 槽，剩余 %u）\n",
				count, kZoneAEnd - m_ZoneANextCursor);
			ASSERT(false, "UnifiedDescriptorHeap Zone A material block exhausted");
			return {};
		}

		m_ZoneALiveCount += count;
		return GetCbvSrvUavHandle(startSlot);
	}

	void UnifiedDescriptorHeap::FreePersistentSingle(uint32_t heapIndex, Graphics::DeferredReleaseQueue* releaseQueue)
	{
		if (!m_Initialized || heapIndex < kZoneAFirstDynamicSlot || heapIndex >= kZoneAEnd)
			return;

		if (releaseQueue != nullptr)
		{
			std::weak_ptr<uint32_t> token = m_LifetimeToken;
			releaseQueue->EnqueueCallback([this, token, heapIndex]() {
				if (!token.expired())
					FreePersistentSingleImmediate(heapIndex);
			});
		}
		else
		{
			FreePersistentSingleImmediate(heapIndex);
		}
	}

	void UnifiedDescriptorHeap::FreePersistentMaterialBlock(uint32_t heapIndex, uint32_t count,
		Graphics::DeferredReleaseQueue* releaseQueue)
	{
		if (!m_Initialized || count == 0 || heapIndex < kZoneAFirstDynamicSlot || heapIndex + count > kZoneAEnd)
			return;

		if (releaseQueue != nullptr)
		{
			std::weak_ptr<uint32_t> token = m_LifetimeToken;
			releaseQueue->EnqueueCallback([this, token, heapIndex, count]() {
				if (!token.expired())
					FreePersistentMaterialBlockImmediate(heapIndex, count);
			});
		}
		else
		{
			FreePersistentMaterialBlockImmediate(heapIndex, count);
		}
	}

	void UnifiedDescriptorHeap::FreePersistentSingleImmediate(uint32_t heapIndex)
	{
		m_ZoneAFreeSingle.push_back(heapIndex);
		if (m_ZoneALiveCount > kZoneAFirstDynamicSlot)
			--m_ZoneALiveCount;
	}

	void UnifiedDescriptorHeap::FreePersistentMaterialBlockImmediate(uint32_t heapIndex, uint32_t count)
	{
		if (count == kMaterialBlockSize)
		{
			m_ZoneAFreeMaterial5.push_back(heapIndex);
		}
		else
		{
			for (uint32_t i = 0; i < count; ++i)
				m_ZoneAFreeSingle.push_back(heapIndex + i);
		}
		if (m_ZoneALiveCount >= kZoneAFirstDynamicSlot + count)
			m_ZoneALiveCount -= count;
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocTransient(uint32_t count)
	{
		if (!m_Initialized || count == 0)
			return {};

		if (m_ZoneBFrameCursor + count > kZoneBSlotsPerFrame)
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] Zone B 当帧瞬态槽耗尽（已用 %u，请求 %u，单帧上限 %u）\n",
				m_ZoneBFrameCursor, count, kZoneBSlotsPerFrame);
			ASSERT(false, "UnifiedDescriptorHeap Zone B per-frame budget exhausted");
			return {};
		}

		const uint32_t baseSlot = kZoneBStart + m_ZoneBFrameSlot * kZoneBSlotsPerFrame + m_ZoneBFrameCursor;
		m_ZoneBFrameCursor += count;
		return GetCbvSrvUavHandle(baseSlot);
	}

	BindlessAllocation UnifiedDescriptorHeap::StageTransientDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE srcHandle)
	{
		BindlessAllocation dst = AllocTransient(1);
		if (!dst.IsValid())
			return dst;

		ASSERT(srcHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);
		Graphics::g_Device->CopyDescriptorsSimple(1, dst.Cpu, srcHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return dst;
	}

	BindlessAllocation UnifiedDescriptorHeap::StageTransientDescriptors(const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles, uint32_t count)
	{
		if (srcHandles == nullptr || count == 0)
			return {};

		BindlessAllocation dst = AllocTransient(count);
		if (!dst.IsValid())
			return dst;

		// 批量一次 CopyDescriptors，减少 CPU API 开销（源句柄在 CPU 侧不一定连续，每项长度为 1）
		static constexpr uint32_t kStackMax = 32;
		if (count <= kStackMax)
		{
			UINT srcSizes[kStackMax];
			for (uint32_t i = 0; i < count; ++i)
			{
				ASSERT(srcHandles[i].ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN);
				srcSizes[i] = 1;
			}
			const UINT destSize = count;
			Graphics::g_Device->CopyDescriptors(
				1, &dst.Cpu, &destSize,
				count, srcHandles, srcSizes,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
		else
		{
			std::vector<UINT> srcSizes(count, 1);
			const UINT destSize = count;
			Graphics::g_Device->CopyDescriptors(
				1, &dst.Cpu, &destSize,
				count, srcHandles, srcSizes.data(),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		return dst;
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocNeuralRange(uint32_t count)
	{
		if (!m_Initialized || count == 0)
			return {};

		// 首次适配复用已归还的神经推理区间
		for (size_t i = 0; i < m_ZoneCFreeRanges.size(); ++i)
		{
			if (m_ZoneCFreeRanges[i].Count >= count)
			{
				const uint32_t start = m_ZoneCFreeRanges[i].Start;
				if (m_ZoneCFreeRanges[i].Count == count)
				{
					m_ZoneCFreeRanges[i] = m_ZoneCFreeRanges.back();
					m_ZoneCFreeRanges.pop_back();
				}
				else
				{
					m_ZoneCFreeRanges[i].Start += count;
					m_ZoneCFreeRanges[i].Count -= count;
				}
				m_ZoneCLiveCount += count;
				return GetCbvSrvUavHandle(start);
			}
		}

		if (m_ZoneCNextCursor + count > kZoneCEnd)
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] Zone C 神经推理槽耗尽（请求 %u，剩余 %u）\n",
				count, kZoneCEnd - m_ZoneCNextCursor);
			ASSERT(false, "UnifiedDescriptorHeap Zone C exhausted");
			return {};
		}

		const uint32_t start = m_ZoneCNextCursor;
		m_ZoneCNextCursor += count;
		m_ZoneCLiveCount += count;
		return GetCbvSrvUavHandle(start);
	}

	void UnifiedDescriptorHeap::FreeNeuralRange(uint32_t startIndex, uint32_t count,
		Graphics::DeferredReleaseQueue* releaseQueue)
	{
		if (!m_Initialized || count == 0 || startIndex < kZoneCStart || startIndex + count > kZoneCEnd)
			return;

		if (releaseQueue != nullptr)
		{
			std::weak_ptr<uint32_t> token = m_LifetimeToken;
			releaseQueue->EnqueueCallback([this, token, startIndex, count]() {
				if (!token.expired())
					FreeNeuralRangeImmediate(startIndex, count);
			});
		}
		else
		{
			FreeNeuralRangeImmediate(startIndex, count);
		}
	}

	void UnifiedDescriptorHeap::FreeNeuralRangeImmediate(uint32_t startIndex, uint32_t count)
	{
		m_ZoneCFreeRanges.push_back({ startIndex, count });
		if (m_ZoneCLiveCount >= count)
			m_ZoneCLiveCount -= count;
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocUI()
	{
		if (!m_Initialized)
			return {};

		for (uint32_t i = 0; i < kZoneDCount; ++i)
		{
			if (!m_ZoneDUsed[i])
			{
				m_ZoneDUsed[i] = true;
				++m_ZoneDLiveCount;
				return GetCbvSrvUavHandle(kZoneDStart + i);
			}
		}

		LOG_ERROR(L"[UnifiedDescriptorHeap] Zone D UI 槽位耗尽（上限 %u）\n", kZoneDCount);
		ASSERT(false, "UnifiedDescriptorHeap Zone D exhausted");
		return {};
	}

	void UnifiedDescriptorHeap::FreeUI(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
	{
		if (!m_Initialized || m_CbvSrvUavDescriptorSize == 0)
			return;

		if (cpuHandle.ptr < m_CbvSrvUavCpuStart.ptr)
			return;

		const uint64_t byteOffset = static_cast<uint64_t>(cpuHandle.ptr - m_CbvSrvUavCpuStart.ptr);
		if ((byteOffset % m_CbvSrvUavDescriptorSize) != 0)
		{
			ASSERT(false, "UnifiedDescriptorHeap::FreeUI 句柄未按描述符大小对齐");
			return;
		}

		const uint32_t heapIndex = static_cast<uint32_t>(byteOffset / m_CbvSrvUavDescriptorSize);
		if (heapIndex < kZoneDStart || heapIndex >= kZoneDEnd)
		{
			ASSERT(false, "UnifiedDescriptorHeap::FreeUI 归还的槽位不属于 Zone D");
			return;
		}

		const uint32_t localIndex = heapIndex - kZoneDStart;
		ASSERT(m_ZoneDUsed[localIndex], "UnifiedDescriptorHeap::FreeUI 重复归还空闲 UI 槽位");
		if (m_ZoneDUsed[localIndex])
		{
			m_ZoneDUsed[localIndex] = false;
			if (m_ZoneDLiveCount > 0)
				--m_ZoneDLiveCount;
		}
	}

	BindlessAllocation UnifiedDescriptorHeap::AllocSamplers(uint32_t count)
	{
		if (!m_Initialized || count == 0)
			return {};

		if (m_SamplerNextCursor + count > kTotalSamplerSlots)
		{
			LOG_ERROR(L"[UnifiedDescriptorHeap] SAMPLER 堆耗尽（已用 %u，请求 %u，上限 %u）\n",
				m_SamplerNextCursor, count, kTotalSamplerSlots);
			ASSERT(false, "UnifiedDescriptorHeap SAMPLER heap exhausted");
			return {};
		}

		const uint32_t startSlot = m_SamplerNextCursor;
		m_SamplerNextCursor += count;
		return GetSamplerHandle(startSlot);
	}
}
