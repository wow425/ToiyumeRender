#pragma once







#include <mutex>
#include <vector>
#include <queue>
#include <string>
#include <atomic>






// CPU可见描述符分配器
// 无界限（Unbounded）的线性内存分配器，专门用于分配仅 CPU 可见（CPU-visible）的描述符。
class DescriptorAllocator
{
public:
    DescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE Type) :
        m_Type(Type), m_CurrentHeap(nullptr), m_DescriptorSize(0)
    {
        m_CurrentHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Allocate(uint32_t Count);

    // ============================================================
    // R2：回收描述符（原先只有 Allocate，没有 Free）
    // ------------------------------------------------------------
    // 为什么必须补上：Resize 会销毁并重建全部 RT/DS，ColorBuffer/
    // DepthBuffer 的每个视图（RTV/SRV/每个 mip 的 UAV）都占用一个
    // CPU 描述符。没有回收路径时，这些槽位永久泄漏 ——
    // 实测 100 次 Resize 泄漏 4320 个描述符、堆池由 8 涨到 40 个堆。
    //
    // 为什么可以安全复用：
    //   这些都是 FLAG_NONE 的 CPU-only 堆。RTV/DSV 只在
    //   OMSetRenderTargets/写命令时由 CPU 读取；SRV/UAV 由 CPU
    //   在 CopyDescriptors 阶段复制进 shader-visible 堆。
    //   GPU 从不直接读这些槽位，因此回收后立即复用是安全的。
    // ============================================================
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE Handle, uint32_t Count);

    static void DestroyAll(void);

    // ============================================================
    // R2 观测接口：存活描述符计数
    // ------------------------------------------------------------
    // 把「描述符不持续增长」从主观判断变成可断言的数字（R2 验收第 3 项）：
    //   Sm_Live    = 当前仍被占用的描述符数（Allocate 加、Free 减）
    //   Sm_Peak    = 历史峰值
    //   HeapPool   = 已申请的堆数量，稳定即说明没有泄漏
    //
    // 堆池不回收（DestroyAll 才清），这是刻意的：堆申请代价高，
    // 复用堆内的空槽比反复创建/销毁堆划算。
    // ============================================================
    // 注意：这些计数是「每个分配器实例」的（非 static）。
    // 早先它们被写成 static/shared，导致 RTV/SRV/DSV/Sampler 的
    // 分配与归还混在同一个计数器里 —— 跨类型相加再相减，
    // 结果出现下溢（freed > alloc），既无法读数也无法定位。
    uint32_t GetLiveAllocated(void) const { return m_LiveAllocated; }
    uint32_t GetFreedCount(void) const { return m_FreedCount; }
    uint32_t GetDoubleFreeCount(void) const { return m_DoubleFreeCount; }
    uint32_t GetTotalAllocated(void) const { return m_TotalAllocated; }
    static uint32_t GetHeapPoolSize(void) { return (uint32_t)sm_DescriptorHeapPool.size(); }


protected:

    static ID3D12DescriptorHeap* RequestNewHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type);

    static const uint32_t sm_NumDescriptorsPerHeap = 256; // 每堆的描述符容量
    static std::mutex sm_AllocationMutex;
    static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> sm_DescriptorHeapPool; // 描述符堆池
    // R2 诊断计数（每实例）
    uint32_t m_TotalAllocated = 0;   // 累计分配的描述符数（只增）
    uint32_t m_LiveAllocated = 0;    // 当前存活占用数
    uint32_t m_FreedCount = 0;       // 累计归还数
    uint32_t m_DoubleFreeCount = 0;  // 重复归还（被位图挡下的次数）

    // 空闲区间：同一堆内的一段连续描述符槽位
    // 用「首次适配」复用，够用且实现简单；碎片化在 RT 场景可忽略。
    struct FreeRange { uint32_t HeapIndex; uint32_t Offset; uint32_t Count; };
    std::vector<FreeRange> m_FreeRanges;

    // ============================================================
    // R2：每堆的「已分配位图」—— 用来把 Free 变成严格一次语义
    // ------------------------------------------------------------
    // 为什么需要：Destroy() 可能被重复调用（显式 Destroy + 析构体
    // + 全局对象在 Shutdown 时再清一次），而句柄置 UNKNOWN 只能挡住
    // 「同一对象重复 Destroy」。若某个对象被**拷贝**过，两份副本
    // 持有相同句柄，就会各自归还同一个槽位 —— 该槽位进入空闲表两次，
    // 之后被两个不同资源同时认领，RTV/SRV 互相覆盖，画面随机错乱。
    //
    // 位图让「归还」只对真正由 Allocate 发出的槽位生效一次：
    //   已置位 -> 允许归还并清零
    //   未置位 -> 重复归还，直接忽略（并计入诊断计数）
    // ============================================================
    std::vector<std::vector<uint64_t>> m_AllocatedBits;  // 每堆 sm_NumDescriptorsPerHeap 位
    void EnsureBits(uint32_t HeapIndex);
    bool TestAndClearBit(uint32_t HeapIndex, uint32_t Offset);
    void SetBit(uint32_t HeapIndex, uint32_t Offset);


    D3D12_DESCRIPTOR_HEAP_TYPE m_Type;
    ID3D12DescriptorHeap* m_CurrentHeap; // 当前指向的堆
    D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentHandle; // 当前指向的堆的游标
    uint32_t m_DescriptorSize;       // 描述符大小
    uint32_t m_RemainingFreeHandles; // 可用容量
};

// 双端描述符句柄封装
// 从接口设计上明确了该类的语义——它专门服务于“着色器可见”的描述符，强制要求 GPU 地址的有效性。
class DescriptorHandle
{
public:
    DescriptorHandle()
    {
        m_CpuHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
        m_GpuHandle.ptr = D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN;
    }

    DescriptorHandle(D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle)
        : m_CpuHandle(CpuHandle), m_GpuHandle(GpuHandle) {
    }

    DescriptorHandle operator+ (INT offsetScaledByDesriptorSize) const
    {
        DescriptorHandle ret = *this;
        ret += offsetScaledByDesriptorSize;
        return ret;
    }

    void operator += (INT OffsetScaledByDescriptorSize)
    {
        if (m_CpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
            m_CpuHandle.ptr += OffsetScaledByDescriptorSize;
        if (m_GpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
            m_GpuHandle.ptr += OffsetScaledByDescriptorSize;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE* operator&() const { return &m_CpuHandle; }
    operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return m_CpuHandle; }
    operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return m_GpuHandle; }

    size_t GetCpuPtr() const { return m_CpuHandle.ptr; }
    uint64_t GetGpuPtr() const { return m_GpuHandle.ptr; }
    bool IsNull() const { return m_CpuHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }
    bool IsShaderVisible() const { return m_GpuHandle.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE m_CpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE m_GpuHandle;

};

// Shader Visible Descriptor Heap
// 固定容量（Bounded）的线性内存分配器，专门管理对 GPU 着色器可见的描述符资源。
class DescriptorHeap
{
public:
    DescriptorHeap(void) {}
    ~DescriptorHeap(void) { Destroy(); }

    void Create(const std::wstring& DebugHeapName, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t MaxCount);
    void Destroy(void) { m_Heap = nullptr; }

    bool HasAvailableSpace(uint32_t Count) const { return Count <= m_NumFreeDescriptors; } // 查询空间是否足够存放
    DescriptorHandle Alloc(uint32_t Count = 1); // 分配
    // 寻址
    DescriptorHandle operator[] (uint32_t arrayIndex) const { return m_FirstHandle + arrayIndex * m_DescriptorSize; }
    // 反推索引
    uint32_t GetOffsetOfHandle(const DescriptorHandle& DHandle)
    {
        return (uint32_t)(DHandle.GetCpuPtr() - m_FirstHandle.GetCpuPtr()) / m_DescriptorSize;
    }

    // R3：堆的「代」编号 —— 每次 Create 成功都会 +1，只增不减。
    // 为什么需要它：Alloc() 是纯 bump 分配器，Create 之后游标归零，同一偏移会在新堆里
    // 指向完全不同的描述符。任何跨帧缓存（例如 ModelLoader 的采样器组合表）如果只存
    // 偏移，在堆被 Destroy + Create 之后就会静默命中错误描述符 —— 不崩溃，只是画面/采样
    // 悄悄错掉，极难排查。缓存方把 m_Generation 一起存下来，代际不符就当作未命中。
    // 为什么 Destroy 不复位：保持单调递增，避免「销毁后重建又回到 0」造成的 ABA
    // （旧缓存条目恰好又匹配上第一代）。
    uint32_t GetGeneration(void) const { return m_Generation; }


    bool ValidateHandle(const DescriptorHandle& DHandle) const;

    ID3D12DescriptorHeap* GetHeapPointer() const { return m_Heap.Get(); }

    uint32_t GetDescriptorSize(void) const { return m_DescriptorSize; }

private:

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
    D3D12_DESCRIPTOR_HEAP_DESC m_HeapDesc;
    uint32_t m_DescriptorSize;
    uint32_t m_NumFreeDescriptors; // 堆上可存放描述符容量
    DescriptorHandle m_FirstHandle; // 当前堆的起始地址
    DescriptorHandle m_NextFreeHandle; // 下一个可用空闲地址

    // R3：Create 成功次数（堆的代）。Destroy 不复位，保持单调递增以避免 ABA。
    uint32_t m_Generation = 0;
};

