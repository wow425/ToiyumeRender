#pragma once

// ============================================================================
// UnifiedDescriptorHeap —— 契约 1：全局唯一 4 分区 Shader-Visible 描述符堆
// ----------------------------------------------------------------------------
// 为什么必须统一为单堆（DX12 硬约束 + 硬件性能不变量）：
//   1) D3D12 规定同一命令列表在同一时刻每种 Shader-Visible 堆类型最多绑定 1 张
//      （1 张 CBV_SRV_UAV + 1 张 SAMPLER）。旧架构中 Renderer 持有 s_TextureHeap，
//      CommandContext 私有 DynamicDescriptorHeap 持有 1024 槽临时堆，ImGuiLayer 又
//      自建 64 槽堆 —— 单帧内多次调用 SetDescriptorHeaps 触发 GPU 管线冲刷（Flush）。
//   2) 原先 s_TextureHeap 归各 Renderer 所有，切换管线即销毁堆，迫使 ModelLoader
//      与全部场景模型被迫卸载重载（TD-01b）。
//   3) 面向 Phase 3 SM 6.6 Bindless（ResourceDescriptorHeap[idx]）与 Phase 4 神经
//      推理（Zone C DirectML / HLSL FP16 MLP 权重），所有分配 API 统一返回带绝对
//      堆内下标 HeapIndex 的 BindlessAllocation，做到单堆零切堆、原生 Bindless 就绪。
//
// 4 分区槽位契约（CBV_SRV_UAV 共 65,536 槽 + SAMPLER 共 2,048 槽）：
//   - Zone A [0, 16384)     : 常驻资产区。槽 0..3 固化为不可变默认哨兵（0=White2D,
//                             1=Black2D, 2=FlatNormal2D, 3=ZeroByteBuffer）；其余槽
//                             提供单槽 Free-List + 5 槽连续材质块 Free-List；
//                             释放经 DeferredReleaseQueue 挂靠帧尾 Fence 延迟回收。
//   - Zone B [16384, 32768) : 帧瞬态环形区。3 帧环形 Bump（每帧配额 5,461 槽），
//                             由 BeginFrame(frameIndex) 轮换槽区，每帧写入临时 SRV/UAV
//                             并直接通过 SetDescriptorTable 绑定，彻底消除 DynamicDescriptorHeap
//                             带来的帧内切堆！
//   - Zone C [32768, 49152) : 神经推理预留区（16,384 槽），支持连续区间分配与延迟释放，
//                             供 Phase 4 DirectML IDMLBindingTable / HLSL MLP 权重复用同一堆。
//   - Zone D [49152, 49408) : UI 专属区（256 槽），供 ImGuiLayer 直接分配/回收字体与 UI SRV。
// ============================================================================

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "02_RHI/Resource/GpuBuffer.h"

class CommandContext;

namespace Graphics
{
	class DeferredReleaseQueue;
}

namespace Renderer
{
	// 无效 Bindless 下标哨兵（与契约 1 / HLSL 侧约定的 0xFFFFFFFFu 严格一致）
	constexpr uint32_t kInvalidBindlessIndex = 0xFFFFFFFFu;

	// 统一描述符分配结果：既可直接给传统 SetDescriptorTable 使用（Gpu），
	// 也可直接作为 SM 6.6 ResourceDescriptorHeap[HeapIndex] 的全局下标。
	struct BindlessAllocation
	{
		uint32_t                    HeapIndex = kInvalidBindlessIndex;
		D3D12_CPU_DESCRIPTOR_HANDLE Cpu       = { 0 };
		D3D12_GPU_DESCRIPTOR_HANDLE Gpu       = { 0 };

		bool IsValid() const { return HeapIndex != kInvalidBindlessIndex; }
	};

	class UnifiedDescriptorHeap
	{
	public:
		// ---- 槽位分区硬常量（契约 1 表格逐项对应）----
		static constexpr uint32_t kTotalCbvSrvUavSlots   = 65536;
		static constexpr uint32_t kTotalSamplerSlots     = 2048;

		// Zone A：常驻资产区 [0, 16384)
		static constexpr uint32_t kZoneAStart            = 0;
		static constexpr uint32_t kZoneACount            = 16384;
		static constexpr uint32_t kZoneAEnd              = kZoneAStart + kZoneACount;

		// Zone A 槽 0..3 不可变默认哨兵（永不释放、永不覆写）
		static constexpr uint32_t kSentinelWhite2D        = 0;
		static constexpr uint32_t kSentinelBlack2D        = 1;
		static constexpr uint32_t kSentinelFlatNormal2D   = 2;
		static constexpr uint32_t kSentinelZeroByteBuffer = 3;
		static constexpr uint32_t kZoneAFirstDynamicSlot  = 4;
		static constexpr uint32_t kMaterialBlockSize      = 5; // t0..t4（与 ConstantBuffers.h / RaytracingScene.h 一致）

		// Zone B：帧瞬态 3 帧环形区 [16384, 32768)
		static constexpr uint32_t kZoneBStart            = 16384;
		static constexpr uint32_t kZoneBCount            = 16384;
		static constexpr uint32_t kZoneBFramesInFlight   = 3;
		static constexpr uint32_t kZoneBSlotsPerFrame    = 5461; // 5461 * 3 = 16383 <= 16384

		// Zone C：神经推理预留区 [32768, 49152)
		static constexpr uint32_t kZoneCStart            = 32768;
		static constexpr uint32_t kZoneCCount            = 16384;
		static constexpr uint32_t kZoneCEnd              = kZoneCStart + kZoneCCount;

		// Zone D：UI（ImGui）专属区 [49152, 49408)
		static constexpr uint32_t kZoneDStart            = 49152;
		static constexpr uint32_t kZoneDCount            = 256;
		static constexpr uint32_t kZoneDEnd              = kZoneDStart + kZoneDCount;

	public:
		UnifiedDescriptorHeap() = default;
		~UnifiedDescriptorHeap() { Shutdown(); }

		UnifiedDescriptorHeap(const UnifiedDescriptorHeap&) = delete;
		UnifiedDescriptorHeap& operator=(const UnifiedDescriptorHeap&) = delete;

		// 创建 65,536 槽 CBV_SRV_UAV 堆 + 2,048 槽 SAMPLER 堆，并初始化槽 0..3 默认哨兵。
		bool Initialize();

		// 释放两张可见堆及哨兵 ZeroByteBuffer。必须晚于所有消费者与 IdleGPU 调用。
		void Shutdown();

		bool IsInitialized() const { return m_Initialized; }

		// 每帧开头调用一次，轮换 Zone B 的 3 帧环形片并清零当前片 Bump 游标。
		void BeginFrame(uint32_t frameIndex);

		// 一次性绑定 CBV_SRV_UAV + SAMPLER 两张可见堆至命令列表（仅触发 1 次 SetDescriptorHeaps）。
		void BindToContext(CommandContext& context) const;

		// ---- Zone A：常驻资产分配/回收 ----
		BindlessAllocation AllocPersistentSingle();
		BindlessAllocation AllocPersistentMaterialBlock(uint32_t count = kMaterialBlockSize);

		// 若传入 releaseQueue，则将槽位归还延迟到 GPU 越过帧尾 Fence 后执行，杜绝在途引用覆写。
		void FreePersistentSingle(uint32_t heapIndex, Graphics::DeferredReleaseQueue* releaseQueue = nullptr);
		void FreePersistentMaterialBlock(uint32_t heapIndex, uint32_t count = kMaterialBlockSize,
			Graphics::DeferredReleaseQueue* releaseQueue = nullptr);

		// ---- Zone B：帧瞬态环形分配与一步 Copy ----
		BindlessAllocation AllocTransient(uint32_t count = 1);
		BindlessAllocation StageTransientDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE srcHandle);
		BindlessAllocation StageTransientDescriptors(const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles, uint32_t count);

		// ---- Zone C：神经推理预留区间分配/回收 ----
		BindlessAllocation AllocNeuralRange(uint32_t count);
		void FreeNeuralRange(uint32_t startIndex, uint32_t count,
			Graphics::DeferredReleaseQueue* releaseQueue = nullptr);

		// ---- Zone D：UI（ImGui）专属 256 槽分配/回收 ----
		BindlessAllocation AllocUI();
		void FreeUI(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);

		// ---- Sampler 可见堆分配（2,048 槽）----
		BindlessAllocation AllocSamplers(uint32_t count);

		// ---- 索引查句柄与底层指针 ----
		BindlessAllocation GetCbvSrvUavHandle(uint32_t heapIndex) const;
		BindlessAllocation GetSamplerHandle(uint32_t heapIndex) const;

		D3D12_GPU_DESCRIPTOR_HANDLE GetCbvSrvUavBaseGpuHandle() const { return m_CbvSrvUavGpuStart; }
		D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerBaseGpuHandle() const { return m_SamplerGpuStart; }

		ID3D12DescriptorHeap* GetCbvSrvUavHeap() const { return m_CbvSrvUavHeap.Get(); }
		ID3D12DescriptorHeap* GetSamplerHeap() const { return m_SamplerHeap.Get(); }

		// 单调递增代际号：防止跨 Init/Shutdown 周期的缓存 ABA 误命中（Destroy 不归零）。
		uint32_t GetGeneration() const { return m_Generation; }
		std::weak_ptr<uint32_t> GetLifetimeToken() const { return m_LifetimeToken; }

		// ---- 观测统计（供 Performance 面板 / 日志核验）----
		uint32_t GetZoneALiveCount() const { return m_ZoneALiveCount; }
		uint32_t GetZoneBUsedInCurrentFrame() const { return m_ZoneBFrameCursor; }
		uint32_t GetZoneCLiveCount() const { return m_ZoneCLiveCount; }
		uint32_t GetZoneDLiveCount() const { return m_ZoneDLiveCount; }
		uint32_t GetSamplerUsedCount() const { return m_SamplerNextCursor; }

	private:
		void PopulateDefaultSentinels();
		void FreePersistentSingleImmediate(uint32_t heapIndex);
		void FreePersistentMaterialBlockImmediate(uint32_t heapIndex, uint32_t count);
		void FreeNeuralRangeImmediate(uint32_t startIndex, uint32_t count);

	private:
		struct FreeRange
		{
			uint32_t Start = 0;
			uint32_t Count = 0;
		};

		bool m_Initialized = false;
		uint32_t m_Generation = 0;
		// 存活令牌：放入 DeferredReleaseQueue 回调的 weak_ptr；Shutdown() 复位该令牌，
		// 避免延迟回调在堆销毁后悬空访问 this。
		std::shared_ptr<uint32_t> m_LifetimeToken;

		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_CbvSrvUavHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;

		D3D12_CPU_DESCRIPTOR_HANDLE m_CbvSrvUavCpuStart = { 0 };
		D3D12_GPU_DESCRIPTOR_HANDLE m_CbvSrvUavGpuStart = { 0 };
		uint32_t m_CbvSrvUavDescriptorSize = 0;

		D3D12_CPU_DESCRIPTOR_HANDLE m_SamplerCpuStart = { 0 };
		D3D12_GPU_DESCRIPTOR_HANDLE m_SamplerGpuStart = { 0 };
		uint32_t m_SamplerDescriptorSize = 0;

		// 哨兵槽 3（kSentinelZeroByteBuffer）所用的 16 字节零值 ByteAddressBuffer
		ByteAddressBuffer m_ZeroSentinelBuffer;

		// Zone A 状态
		uint32_t m_ZoneANextCursor = kZoneAFirstDynamicSlot;
		std::vector<uint32_t> m_ZoneAFreeSingle;
		std::vector<uint32_t> m_ZoneAFreeMaterial5;
		uint32_t m_ZoneALiveCount = 0;

		// Zone B 状态（3 帧环形）
		uint32_t m_ZoneBFrameSlot = 0;
		uint32_t m_ZoneBFrameCursor = 0;

		// Zone C 状态（神经推理连续区间）
		uint32_t m_ZoneCNextCursor = kZoneCStart;
		std::vector<FreeRange> m_ZoneCFreeRanges;
		uint32_t m_ZoneCLiveCount = 0;

		// Zone D 状态（ImGui 256 槽位图）
		std::array<bool, kZoneDCount> m_ZoneDUsed = {};
		uint32_t m_ZoneDLiveCount = 0;

		// Sampler 堆 Bump 状态
		uint32_t m_SamplerNextCursor = 0;
	};
}
