#pragma once

#include "RHI/DescriptorHeap/DescriptorHeap.h"
#include "RHI/PipelineState/RootSignature.h"
#include <vector>
#include <queue>
#include <functional> // 用于改造CPP成员函数指针

// 不取名的为匿名空间,此为具名空间。
namespace Graphics
{
    // extern声明该变量已经被定义过了的，全局共享
    extern ID3D12Device* g_Device;

    // static内部链接，每个文件均可创建互不相干，彼此独立的变量，链接器会认为各自私有的。各自为政
}

class CommandContext;
// 在类的声明内部（头文件中）编写实现的方法，会被编译器视为内联函数，因此消除函数调用方法
// DynamicDescriptorHeap 的 public 方法在每一帧中可能会被调用成千上万次,每次调用都会产生函数调用开销.
// 快路径与慢路径设计模式
// 简短的代码适合内联故直接实现。
// 慢路径不放在头文件，因为实现需要#include不少头文件，会导致修改就必须重编译包含此头文件的cpp文件
// 复杂实现隐藏在cpp中，则修改私有逻辑只需编译这一个文件，实现编译隔离,避免指令缓存污染


// 
class DynamicDescriptorHeap
{
public:
    DynamicDescriptorHeap(CommandContext& OwningContext, D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
        :m_OwningContext(OwningContext), m_DescriptorType(HeapType)
    {
        m_CurrentHeapPtr = nullptr;
        m_CurrentOffset = 0;
        m_DescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(HeapType);
    }
    ~DynamicDescriptorHeap() {};

    static void DestroyAll(void)
    {
        sm_DescriptorHeapPool[0].clear();
        sm_DescriptorHeapPool[1].clear();
    }

    void CleanupUsedHeaps(uint64_t fenceValue);

    // 绕过缓冲和提交，直接堆上分配位置上传，用于小资源分配
    D3D12_GPU_DESCRIPTOR_HANDLE UploadDirect(D3D12_CPU_DESCRIPTOR_HANDLE Handles);

    // 解析根签名
    void ParseGraphicsRootSignature(const RootSignature& RootSig)
    {
        m_GraphicsHandleCache.ParseRootSignature(m_DescriptorType, RootSig);
    }
    // 设置图形描述符句柄
    void SetGraphicsDescriptorHandles(UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
    {
        m_GraphicsHandleCache.StageDescriptorHandles(RootIndex, Offset, NumHandles, Handles);
    }
    // 提交图形根描述符表
    inline void CommitGraphicsRootDescriptorTables(ID3D12GraphicsCommandList* CmdList)
    {
        if (m_GraphicsHandleCache.m_StaleRootParamsBitMap != 0)
            CopyAndBindStagedTables(m_GraphicsHandleCache, CmdList, &ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable);
    }


    void SetComputeDescriptorHandles(UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
    {
        m_ComputeHandleCache.StageDescriptorHandles(RootIndex, Offset, NumHandles, Handles);
    }
    void ParseComputeRootSignature(const RootSignature& RootSig)
    {
        m_ComputeHandleCache.ParseRootSignature(m_DescriptorType, RootSig);
    }
    inline void CommitComputeRootDescriptorTables(ID3D12GraphicsCommandList* CmdList)
    {
        if (m_ComputeHandleCache.m_StaleRootParamsBitMap != 0)
            CopyAndBindStagedTables(m_ComputeHandleCache, CmdList, &ID3D12GraphicsCommandList::SetComputeRootDescriptorTable);
    }

private:

    // 静态成员变量，全实例共享，实现对象池模式
    // 描述符堆类型就两种：采样器，资源（CBV,UAV,SRV)
    static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> sm_DescriptorHeapPool[2];
    static std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>> sm_RetiredDescriptorHeaps[2];
    static std::queue<ID3D12DescriptorHeap*> sm_AvailableDescriptorHeaps[2];
    static const uint32_t kNumDescriptorsPerHeap = 1024;
    static std::mutex sm_Mutex;

    // 静态成员方法。Manager 类无实例化
    // 无需实例。普通成员方法需要实例this指针才能调用，而静态成员方法不用
    // 可访问private静态成员变量
    static ID3D12DescriptorHeap* RequestDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE HeapType);
    static void DiscardDescriptorHeaps(D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint64_t FenceValueForReset, const std::vector<ID3D12DescriptorHeap*>& UsedHeaps);

    // 依赖注入的设计模式,消灭了多线程下跨上下文访问的竞合危险。
    // 引用以绑定对应的commandcontext，可解决drawcall时更换堆需要重绑定堆和描述符，1个分配器只能服务于1个上下文。
    CommandContext& m_OwningContext;
    ID3D12DescriptorHeap* m_CurrentHeapPtr; // 当前堆指针
    std::vector<ID3D12DescriptorHeap*> m_RetiredHeaps; // 未提交的退休堆队列
    DescriptorHandle m_FirstDescriptor;   // 当前堆的描述符封装类
    const D3D12_DESCRIPTOR_HEAP_TYPE m_DescriptorType; // 描述符类型
    uint32_t m_DescriptorSize; // 描述符大小
    uint32_t m_CurrentOffset; // 描述符偏移



    // 类中封装的结构体用于打包数据，实现高内聚
    // 描述了一个“描述符表条目”：包含句柄缓存（Handle Cache）中的一段特定区域，以及（记录了）哪些句柄已经被设置。
    // 描述符表缓存
    struct DescriptorTableCache
    {
        DescriptorTableCache() : AssignedHandlesBitMap(0) {}
        // 64位cpu一次性读8字节
        D3D12_CPU_DESCRIPTOR_HANDLE* TableStart; // 起始位置
        uint32_t AssignedHandlesBitMap; // 已分配句柄位图，标记哪些位置已分配
        uint32_t TableSize; // 大小
    };
    //==============================================
    // CPU端描述符句柄缓存
    struct DescriptorHandleCache
    {
        DescriptorHandleCache() { ClearCache(); }
        void ClearCache()
        {
            m_RootDescriptorTablesBitMap = 0;
            m_StaleRootParamsBitMap = 0;
            m_MaxCachedDescriptors = 0;
        }

        uint32_t m_RootDescriptorTablesBitMap; // 根描述符表位图, 根签名上根描述符表的索引
        uint32_t m_StaleRootParamsBitMap; // 脏根参数更新位图，根签名上发生变动的根参数的索引
        uint32_t m_MaxCachedDescriptors;

        // k：代表编译期确定的常量
        static const uint32_t kMaaxNumDescriptors = 256;
        static const uint32_t kMaxNumDescriptorTables = 16;
        DescriptorTableCache m_RootDescriptorTable[kMaxNumDescriptorTables];  // 描述符表缓存
        D3D12_CPU_DESCRIPTOR_HANDLE m_HandleCache[kMaaxNumDescriptors];

        uint32_t ComputeStagedSize();
        void CopyAndBindStaleTables(D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t DescriptorSize, DescriptorHandle DestHandleStart, ID3D12GraphicsCommandList* CmdList,
            void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE));

        void UnbindAllValid();
        void StageDescriptorHandles(UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
        void ParseRootSignature(D3D12_DESCRIPTOR_HEAP_TYPE Type, const RootSignature& RootSig);
    };
    // 描述符句柄缓冲
    //==============================================
    DescriptorHandleCache m_GraphicsHandleCache; // 图形句柄缓冲
    DescriptorHandleCache m_ComputeHandleCache; // 计算句柄缓冲


    bool HasSpace(uint32_t Count)
    {
        return (m_CurrentHeapPtr != nullptr && m_CurrentOffset + Count <= kNumDescriptorsPerHeap);
    }

    void RetireCurrentHeap(void);
    void RetireUsedHeaps(uint64_t fenceValue);
    ID3D12DescriptorHeap* GetHeapPointer();

    DescriptorHandle Allocate(UINT Count)
    {
        DescriptorHandle ret = m_FirstDescriptor + m_CurrentOffset * m_DescriptorSize;
        m_CurrentOffset += Count;
        return ret;
    }

    void UnbindAllValid(void);

    //此为CPP 98风格，过于古老且难看
    void CopyAndBindStagedTables(DescriptorHandleCache& HandleCache, ID3D12GraphicsCommandList* CmdList,
        void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::* SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE));
    // 此为现代风格.使用 std::function (多态函数包装器)存在内存分配
    void CopyAndBindStableTables
    (
        DescriptorHandleCache& HandleCache,
        ID3D12GraphicsCommandList* CmdList,
        std::function<void(UINT, D3D12_GPU_DESCRIPTOR_HANDLE)> SetFunc
    );
    // 使用模板,零成本抽象 泛型编程 (Generic Programming / 汎用プログラミング)
    template <typename Callable>
    void CopyAndBindStableTables
    (
        DescriptorHandleCache& HandleCache,
        ID3D12GraphicsCommandList* CmdList,
        Callable&& SetFunc // 接收任何“可调用对象”
    );
};
