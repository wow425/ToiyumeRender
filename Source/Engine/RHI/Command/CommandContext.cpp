#include "PCH.h"
#include "CommandContext.h"
#include "../Resource/Buffer/ColorBuffer.h"
#include "../Resource/Buffer/DepthBuffer.h"
#include "../GraphicsCore.h"
#include "../DescriptorHeap/DescriptorHeap.h"
// #include "EngineProfiling.h"
#include "../Resource/Buffer/UploadBuffer.h"
#include "../Resource/Buffer/ReadbackBuffer.h"


#pragma warning(push) // 将编译器的警告状态压入内部栈
#pragma warning(disable:4100) // 关闭未引用形参警告
#include <pix3.h> // 引入PIX
#pragma warning(pop) // 栈中弹处警告

using namespace Graphics;

void ContextManager::DestroyAllContexts(void)
{
    for (uint32_t i = 0; i < 4; ++i)
        sm_ContextPool[i].clear();
}

CommandContext* ContextManager::AllocateContext(D3D12_COMMAND_LIST_TYPE Type)
{
    std::lock_guard<std::mutex> LockGuard(sm_ContextAllocationMutex);
    // 获取Type上下文
    auto& AvailableContexts = sm_AvailableContexts[Type]; // Return Value。用于从一个 工厂方法或 管理单例 中获取并返回一个可用的对象实例。

    CommandContext* ret = nullptr;
    if (AvailableContexts.empty()) // 无，就创建个
    {
        ret = new CommandContext(Type);
        sm_ContextPool[Type].emplace_back(ret);
        ret->Initialize();
    }
    else // 有，就可用队列中pop
    {
        ret = AvailableContexts.front();
        AvailableContexts.pop();
        ret->Reset();
    }
    ASSERT(ret != nullptr);

    ASSERT(ret->m_Type == Type);

    return ret;
}

void ContextManager::FreeContext(CommandContext* UsedContext)
{
    ASSERT(UsedContext != nullptr);
    std::lock_guard<std::mutex> LockGuard(sm_ContextAllocationMutex);
    sm_AvailableContexts[UsedContext->m_Type].push(UsedContext); // 回收到可用队列
}

void CommandContext::DestroyAllContexts(void)
{
    LinearAllocator::DestroyAll(); // CPU端堆销毁
    DynamicDescriptorHeap::DestroyAll(); // GPU端shader——visible堆销毁
    g_ContextManager.DestroyAllContexts(); // 上下文销毁
}

// 创建上下文
CommandContext& CommandContext::Begin(const std::wstring ID)
{
    CommandContext* NewContext = g_ContextManager.AllocateContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
    NewContext->SetID(ID);
    //     if (ID.length() > 0)
    // EngineProfiling::BeginBlock(ID, NewContext);
    return *NewContext;
}
// 创建上下文
ComputeContext& ComputeContext::Begin(const std::wstring& ID, bool Async)
{
    ComputeContext& NewContext = g_ContextManager.AllocateContext(Async ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT)->GetComputeContext();
    NewContext.SetID(ID);
    //if (ID.length() > 0)
    //	EngineProfiling::BeginBlock(ID, &NewContext);
    return NewContext;
}

uint64_t CommandContext::Flush(bool WaitForCompletion)
{
    // 提交屏障数组
    FlushResourceBarriers();

    ASSERT(m_CurrentAllocator != nullptr);
    // 获取最新围栏值
    uint64_t FenceValue = g_CommandManager.GetQueue(m_Type).ExecuteCommandList(m_CommandList);
    // 是否FenceValue对应的命令队列等待
    if (WaitForCompletion)
        g_CommandManager.WaitForFence(FenceValue);

    //
    // 重置命令列表，设置图形，计算根签名，渲染管线
    //
    m_CommandList->Reset(m_CurrentAllocator, nullptr);

    if (m_CurGraphicsRootSignature)
    {
        m_CommandList->SetGraphicsRootSignature(m_CurGraphicsRootSignature);
    }
    if (m_CurComputeRootSignature)
    {
        m_CommandList->SetComputeRootSignature(m_CurComputeRootSignature);
    }
    if (m_CurPipelineState)
    {
        m_CommandList->SetPipelineState(m_CurPipelineState);
    }

    BindDescriptorHeaps();

    return FenceValue;
}

// 执行命令列表，“清空”资源
uint64_t CommandContext::Finish(bool WaitForCompletion)
{
    ASSERT(m_Type == D3D12_COMMAND_LIST_TYPE_DIRECT || m_Type == D3D12_COMMAND_LIST_TYPE_COMPUTE);

    FlushResourceBarriers();

    //if (m_ID.length() > 0)
    //	EngineProfiling::EndBlock(this);

    ASSERT(m_CurrentAllocator != nullptr);

    CommandQueue& Queue = g_CommandManager.GetQueue(m_Type);
    // 执行命令列表，发出signal
    uint64_t FenceValue = Queue.ExecuteCommandList(m_CommandList);
    Queue.DiscardAllocator(FenceValue, m_CurrentAllocator);
    m_CurrentAllocator = nullptr;
    // CPU端堆资源清空
    m_CpuLinearAllocator.CleanupUsedPages(FenceValue);
    m_GpuLinearAllocator.CleanupUsedPages(FenceValue);
    // GPU端shader-visible堆资源清空
    m_DynamicViewDescriptorHeap.CleanupUsedHeaps(FenceValue);
    m_DynamicSamplerDescriptorHeap.CleanupUsedHeaps(FenceValue);

    if (WaitForCompletion)
        g_CommandManager.WaitForFence(FenceValue);
    // 上下文清空
    g_ContextManager.FreeContext(this);

    return FenceValue;
}

CommandContext::CommandContext(D3D12_COMMAND_LIST_TYPE Type) :
    m_Type(Type),
    m_DynamicViewDescriptorHeap(*this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
    m_DynamicSamplerDescriptorHeap(*this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER),
    m_CpuLinearAllocator(kCpuWritable),
    m_GpuLinearAllocator(kGpuExclusive)
{
    m_OwningManager = nullptr;
    m_CommandList = nullptr;
    m_CurrentAllocator = nullptr;
    ZeroMemory(m_CurrentDescriptorHeaps, sizeof(m_CurrentDescriptorHeaps));

    m_CurGraphicsRootSignature = nullptr;
    m_CurComputeRootSignature = nullptr;
    m_CurPipelineState = nullptr;
    m_NumBarriersToFlush = 0;
}

CommandContext::~CommandContext(void)
{
    if (m_CommandList != nullptr)
        m_CommandList->Release();
}

void CommandContext::Initialize(void)
{
    g_CommandManager.CreateNewCommandList(m_Type, &m_CommandList, &m_CurrentAllocator);
}

void CommandContext::Reset(void)
{

    // 只对先前已释放的上下文调用Reset()，命令列表保留，但命令分配器需重新申请
    ASSERT(m_CommandList != nullptr && m_CurrentAllocator == nullptr);
    m_CurrentAllocator = g_CommandManager.GetQueue(m_Type).RequestAllocator(); // 申请命令分配器
    m_CommandList->Reset(m_CurrentAllocator, nullptr);

    m_CurGraphicsRootSignature = nullptr;
    m_CurComputeRootSignature = nullptr;
    m_CurPipelineState = nullptr;
    m_NumBarriersToFlush = 0;

    BindDescriptorHeaps();
}

// Context存储的描述符堆m_CurrentDescriptorHeaps绑定在命令列表上
void CommandContext::BindDescriptorHeaps(void)
{
    UINT NonNullHeaps = 0; // 计数器，记录需要绑定的有效堆数量
    // 统计要绑定的堆
    ID3D12DescriptorHeap* HeapsToBind[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
    for (UINT i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i)
    {
        ID3D12DescriptorHeap* HeapIter = m_CurrentDescriptorHeaps[i];
        if (HeapIter != nullptr)
            HeapsToBind[NonNullHeaps++] = HeapIter;
    }
    // 一次性绑定
    if (NonNullHeaps > 0)
        m_CommandList->SetDescriptorHeaps(NonNullHeaps, HeapsToBind);
}

void GraphicsContext::SetRenderTargets(UINT NumRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[], D3D12_CPU_DESCRIPTOR_HANDLE DSV)
{
    m_CommandList->OMSetRenderTargets(NumRTVs, RTVs, FALSE, &DSV);
}

void GraphicsContext::SetRenderTargets(UINT NumRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[])
{
    m_CommandList->OMSetRenderTargets(NumRTVs, RTVs, FALSE, nullptr);
}

void GraphicsContext::BeginQuery(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT HeapIndex)
{
    m_CommandList->BeginQuery(QueryHeap, Type, HeapIndex);
}

void GraphicsContext::EndQuery(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT HeapIndex)
{
    m_CommandList->EndQuery(QueryHeap, Type, HeapIndex);
}

void GraphicsContext::ResolveQueryData(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource* DestinationBuffer, UINT64 DestinationBufferOffset)
{
    m_CommandList->ResolveQueryData(QueryHeap, Type, StartIndex, NumQueries, DestinationBuffer, DestinationBufferOffset);
}

void GraphicsContext::ClearUAV(GpuBuffer& Target)
{
    // 提交屏障数组
    FlushResourceBarriers();

    // After binding a UAV, we can get a GPU handle that is required to clear it as a UAV (because it essentially runs
    // a shader to set all of the values).
    // 在绑定 UAV 之后，我们可以获得一个用于将其作为 UAV 进行清空操作所需的 GPU 句柄（因为该操作本质上是运行了一个着色器来设置所有数值
    /*这句话揭示了 DX12 硬件架构中的一个重要真相：ClearUnorderedAccessViewUint/Float 并不是一个简单的硬件级“内存抹除”指令。
    在 GPU 硬件层面，由于 UAV 支持原子操作且具有高度的并行性，硬件并没有提供一个像 CPU memset 那样简单的全局电路来清空它。
    相反，当你调用清空 UAV 的 API 时，驱动程序（Driver）通常会在底层调度一个隐藏的 计算着色器 (Compute Shader / コンピュートシェーダー)。这个着色器会根据 UAV 的维度，分派大量的 线程束 (Warps / Wavefronts)，每个线程负责将清空值（如 0）写入对应的内存地址。
    这就是为什么注释中强调需要 GPU 句柄 (GPU Handle / GPU ハンドル)，因为 GPU 必须像寻址普通纹理一样，通过描述符定位到该资源才能运行这个“清空着色器”。*/
    D3D12_GPU_DESCRIPTOR_HANDLE GpuVisibleHandle = m_DynamicViewDescriptorHeap.UploadDirect(Target.GetUAV()); // UAV上传到GPU端shader-visible堆，获取堆上对应位置的句柄
    const UINT ClearColor[4] = {};
    m_CommandList->ClearUnorderedAccessViewUint(GpuVisibleHandle, Target.GetUAV(), Target.GetResource(), ClearColor, 0, nullptr); //
}

void ComputeContext::ClearUAV(GpuBuffer& Target)
{
    FlushResourceBarriers();

    // After binding a UAV, we can get a GPU handle that is required to clear it as a UAV (because it essentially runs
    // a shader to set all of the values).
    // 在绑定 UAV 之后，我们可以获得一个用于将其作为 UAV 进行清空操作所需的 GPU 句柄（因为该操作本质上是运行了一个着色器来设置所有数值
    /*这句话揭示了 DX12 硬件架构中的一个重要真相：ClearUnorderedAccessViewUint/Float 并不是一个简单的硬件级“内存抹除”指令。
    在 GPU 硬件层面，由于 UAV 支持原子操作且具有高度的并行性，硬件并没有提供一个像 CPU memset 那样简单的全局电路来清空它。
    相反，当你调用清空 UAV 的 API 时，驱动程序（Driver）通常会在底层调度一个隐藏的 计算着色器 (Compute Shader / コンピュートシェーダー)。这个着色器会根据 UAV 的维度，分派大量的 线程束 (Warps / Wavefronts)，每个线程负责将清空值（如 0）写入对应的内存地址。
    这就是为什么注释中强调需要 GPU 句柄 (GPU Handle / GPU ハンドル)，因为 GPU 必须像寻址普通纹理一样，通过描述符定位到该资源才能运行这个“清空着色器”。*/
    D3D12_GPU_DESCRIPTOR_HANDLE GpuVisibleHandle = m_DynamicViewDescriptorHeap.UploadDirect(Target.GetUAV()); // UAV上传到GPU端shader-visible堆，获取堆上对应位置的句柄
    const UINT ClearColor[4] = {};
    m_CommandList->ClearUnorderedAccessViewUint(GpuVisibleHandle, Target.GetUAV(), Target.GetResource(), ClearColor, 0, nullptr);
}


void GraphicsContext::ClearUAV(ColorBuffer& Target)
{
    FlushResourceBarriers();

    // After binding a UAV, we can get a GPU handle that is required to clear it as a UAV (because it essentially runs
    // a shader to set all of the values).
    // 在绑定 UAV 之后，我们可以获得一个用于将其作为 UAV 进行清空操作所需的 GPU 句柄（因为该操作本质上是运行了一个着色器来设置所有数值
    /*这句话揭示了 DX12 硬件架构中的一个重要真相：ClearUnorderedAccessViewUint/Float 并不是一个简单的硬件级“内存抹除”指令。
    在 GPU 硬件层面，由于 UAV 支持原子操作且具有高度的并行性，硬件并没有提供一个像 CPU memset 那样简单的全局电路来清空它。
    相反，当你调用清空 UAV 的 API 时，驱动程序（Driver）通常会在底层调度一个隐藏的 计算着色器 (Compute Shader / コンピュートシェーダー)。这个着色器会根据 UAV 的维度，分派大量的 线程束 (Warps / Wavefronts)，每个线程负责将清空值（如 0）写入对应的内存地址。
    这就是为什么注释中强调需要 GPU 句柄 (GPU Handle / GPU ハンドル)，因为 GPU 必须像寻址普通纹理一样，通过描述符定位到该资源才能运行这个“清空着色器”。*/
    D3D12_GPU_DESCRIPTOR_HANDLE GpuVisibleHandle = m_DynamicViewDescriptorHeap.UploadDirect(Target.GetUAV()); // UAV上传到GPU端shader-visible堆，获取堆上对应位置的句柄
    CD3DX12_RECT ClearRect(0, 0, (LONG)Target.GetWidth(), (LONG)Target.GetHeight());


    const float* ClearColor = Target.GetClearColor().GetPtr();
    m_CommandList->ClearUnorderedAccessViewFloat(GpuVisibleHandle, Target.GetUAV(), Target.GetResource(), ClearColor, 1, &ClearRect);
}

void ComputeContext::ClearUAV(ColorBuffer& Target)
{
    FlushResourceBarriers();

    // After binding a UAV, we can get a GPU handle that is required to clear it as a UAV (because it essentially runs
    // a shader to set all of the values).
        // 在绑定 UAV 之后，我们可以获得一个用于将其作为 UAV 进行清空操作所需的 GPU 句柄（因为该操作本质上是运行了一个着色器来设置所有数值
    /*这句话揭示了 DX12 硬件架构中的一个重要真相：ClearUnorderedAccessViewUint/Float 并不是一个简单的硬件级“内存抹除”指令。
    在 GPU 硬件层面，由于 UAV 支持原子操作且具有高度的并行性，硬件并没有提供一个像 CPU memset 那样简单的全局电路来清空它。
    相反，当你调用清空 UAV 的 API 时，驱动程序（Driver）通常会在底层调度一个隐藏的 计算着色器 (Compute Shader / コンピュートシェーダー)。这个着色器会根据 UAV 的维度，分派大量的 线程束 (Warps / Wavefronts)，每个线程负责将清空值（如 0）写入对应的内存地址。
    这就是为什么注释中强调需要 GPU 句柄 (GPU Handle / GPU ハンドル)，因为 GPU 必须像寻址普通纹理一样，通过描述符定位到该资源才能运行这个“清空着色器”。*/
    D3D12_GPU_DESCRIPTOR_HANDLE GpuVisibleHandle = m_DynamicViewDescriptorHeap.UploadDirect(Target.GetUAV()); // UAV上传到GPU端shader-visible堆，获取堆上对应位置的句柄
    CD3DX12_RECT ClearRect(0, 0, (LONG)Target.GetWidth(), (LONG)Target.GetHeight());

    //TODO: My Nvidia card is not clearing UAVs with either Float or Uint variants.
    const float* ClearColor = Target.GetClearColor().GetPtr();
    m_CommandList->ClearUnorderedAccessViewFloat(GpuVisibleHandle, Target.GetUAV(), Target.GetResource(), ClearColor, 1, &ClearRect);
}

void GraphicsContext::ClearColor(ColorBuffer& Target, D3D12_RECT* Rect)
{
    FlushResourceBarriers();
    m_CommandList->ClearRenderTargetView(Target.GetRTV(), Target.GetClearColor().GetPtr(), (Rect == nullptr) ? 0 : 1, Rect);
}

void GraphicsContext::ClearColor(ColorBuffer& Target, float Colour[4], D3D12_RECT* Rect)
{
    FlushResourceBarriers();
    m_CommandList->ClearRenderTargetView(Target.GetRTV(), Colour, (Rect == nullptr) ? 0 : 1, Rect);
}

void GraphicsContext::ClearDepth(DepthBuffer& Target)
{
    FlushResourceBarriers();
    m_CommandList->ClearDepthStencilView(Target.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, Target.GetClearDepth(), Target.GetClearStencil(), 0, nullptr);
}

void GraphicsContext::ClearStencil(DepthBuffer& Target)
{
    FlushResourceBarriers();
    m_CommandList->ClearDepthStencilView(Target.GetDSV(), D3D12_CLEAR_FLAG_STENCIL, Target.GetClearDepth(), Target.GetClearStencil(), 0, nullptr);
}

void GraphicsContext::ClearDepthAndStencil(DepthBuffer& Target)
{
    FlushResourceBarriers();
    m_CommandList->ClearDepthStencilView(Target.GetDSV(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, Target.GetClearDepth(), Target.GetClearStencil(), 0, nullptr);
}

void GraphicsContext::SetViewportAndScissor(const D3D12_VIEWPORT& vp, const D3D12_RECT& rect)
{
    ASSERT(rect.left < rect.right && rect.top < rect.bottom);
    m_CommandList->RSSetViewports(1, &vp);
    m_CommandList->RSSetScissorRects(1, &rect);
}

void GraphicsContext::SetViewport(const D3D12_VIEWPORT& vp)
{
    m_CommandList->RSSetViewports(1, &vp);
}

void GraphicsContext::SetViewport(FLOAT x, FLOAT y, FLOAT w, FLOAT h, FLOAT minDepth, FLOAT maxDepth)
{
    D3D12_VIEWPORT vp;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = minDepth;
    vp.MaxDepth = maxDepth;
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    m_CommandList->RSSetViewports(1, &vp);
}

void GraphicsContext::SetScissor(const D3D12_RECT& rect)
{
    ASSERT(rect.left < rect.right && rect.top < rect.bottom);
    m_CommandList->RSSetScissorRects(1, &rect);
}


void CommandContext::TransitionResource(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate)
{
    D3D12_RESOURCE_STATES OldState = Resource.m_UsageState;
    // 若为计算命令列表，则验证类型是否合法
    if (m_Type == D3D12_COMMAND_LIST_TYPE_COMPUTE) // &按位与，同1为1
    {
        ASSERT((OldState & VALID_COMPUTE_QUEUE_RESOURCE_STATES) == OldState);
        ASSERT((NewState & VALID_COMPUTE_QUEUE_RESOURCE_STATES) == NewState);
    }

    if (OldState != NewState)
    {
        // 屏障数量必须小于16
        ASSERT(m_NumBarriersToFlush < 16, "Exceeded arbitrary limit on buffered barriers屏障数量必须小于16");
        D3D12_RESOURCE_BARRIER& BarrierDesc = m_ResourceBarrierBuffer[m_NumBarriersToFlush++];

        BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        BarrierDesc.Transition.pResource = Resource.GetResource();
        BarrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        BarrierDesc.Transition.StateBefore = OldState;
        BarrierDesc.Transition.StateAfter = NewState;

        // Check to see if we already started the transition检查是否已经开始转换
        if (NewState == Resource.m_TransitioningState)
        {
            BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY; // 发出“转换必须在此完成”的信号，后续指令必须等待转换彻底结束。
            Resource.m_TransitioningState = (D3D12_RESOURCE_STATES)-1;
        }
        else
            BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

        Resource.m_UsageState = NewState;
    }
    else if (NewState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) // 新状态为无序访问的话
        InsertUAVBarrier(Resource, FlushImmediate);

    if (FlushImmediate || m_NumBarriersToFlush == 16) // 是否提交屏障数组
        FlushResourceBarriers();
}

void CommandContext::BeginResourceTransition(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate)
{
    // 如果已经转换，结束转换
    if (Resource.m_TransitioningState != (D3D12_RESOURCE_STATES)-1)
        TransitionResource(Resource, Resource.m_TransitioningState);

    D3D12_RESOURCE_STATES OldState = Resource.m_UsageState;

    if (OldState != NewState)
    {
        ASSERT(m_NumBarriersToFlush < 16, "Exceeded arbitrary limit on buffered barriers");
        D3D12_RESOURCE_BARRIER& BarrierDesc = m_ResourceBarrierBuffer[m_NumBarriersToFlush++];

        BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        BarrierDesc.Transition.pResource = Resource.GetResource();
        BarrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        BarrierDesc.Transition.StateBefore = OldState;
        BarrierDesc.Transition.StateAfter = NewState;

        BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;

        Resource.m_TransitioningState = NewState;
    }

    if (FlushImmediate || m_NumBarriersToFlush == 16)
        FlushResourceBarriers();
}

void CommandContext::InsertUAVBarrier(GpuResource& Resource, bool FlushImmediate)
{
    ASSERT(m_NumBarriersToFlush < 16, "Exceeded arbitrary limit on buffered barriers");
    D3D12_RESOURCE_BARRIER& BarrierDesc = m_ResourceBarrierBuffer[m_NumBarriersToFlush++];

    BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    BarrierDesc.UAV.pResource = Resource.GetResource();

    if (FlushImmediate)
        FlushResourceBarriers();
}

// 处理两个或多个共享同一块物理内存（ID3D12Heap）的资源之间的转换。
// 即便只有 1 字节的重叠也必须Alias Barrier 重叠屏障
void CommandContext::InsertAliasBarrier(GpuResource& Before, GpuResource& After, bool FlushImmediate)
{
    ASSERT(m_NumBarriersToFlush < 16, "Exceeded arbitrary limit on buffered barriers");
    D3D12_RESOURCE_BARRIER& BarrierDesc = m_ResourceBarrierBuffer[m_NumBarriersToFlush++];

    BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    BarrierDesc.Aliasing.pResourceBefore = Before.GetResource();
    BarrierDesc.Aliasing.pResourceAfter = After.GetResource();

    if (FlushImmediate)
        FlushResourceBarriers();
}
// 用BufferData填充Buffer
void CommandContext::WriteBuffer(GpuResource& Dest, size_t DestOffset, const void* BufferData, size_t NumBytes)
{
    ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16)); // Buffer16对齐
    DynAlloc TempSpace = m_CpuLinearAllocator.Allocate(NumBytes, 512); // 临时CPU端堆512对齐
    SIMDMemCopy(TempSpace.DataPtr, BufferData, Math::DivideByMultiple(NumBytes, 16));
    CopyBufferRegion(Dest, DestOffset, TempSpace.Buffer, TempSpace.Offset, NumBytes);
}
// 用DWParam填充Buffer
void CommandContext::FillBuffer(GpuResource& Dest, size_t DestOffset, DWParam Value, size_t NumBytes)
{
    DynAlloc TempSpace = m_CpuLinearAllocator.Allocate(NumBytes, 512);
    // 将一个普通的 float 变量 Value.Float，广播 (Broadcast) 到 128 位寄存器的全部 4 个通道中。
    // __m128: 这是一个特殊的 128 位（16字节）寄存器类型。在硬件层面，它直接对应 CPU 里的 XMM 寄存器。它可以同时存放 4 个 32 位浮点数 (float)。
    __m128 VectorValue = _mm_set1_ps(Value.Float);
    SIMDMemFill(TempSpace.DataPtr, VectorValue, Math::DivideByMultiple(NumBytes, 16));
    CopyBufferRegion(Dest, DestOffset, TempSpace.Buffer, TempSpace.Offset, NumBytes);
}

void CommandContext::InitializeTexture(GpuResource& Dest, UINT NumSubresources, D3D12_SUBRESOURCE_DATA SubData[])
{
    // 对齐大小
    UINT64 uploadBufferSize = GetRequiredIntermediateSize(Dest.GetResource(), 0, NumSubresources);

    CommandContext& InitContext = CommandContext::Begin();

    // copy data to the intermediate upload heap and then schedule a copy from the upload heap to the default texture
    // 复制数据到上传堆，再从上传堆复制到默认纹理堆
    DynAlloc mem = InitContext.ReserveUploadMemory(uploadBufferSize);
    UpdateSubresources(InitContext.m_CommandList, Dest.GetResource(), mem.Buffer.GetResource(), 0, 0, NumSubresources, SubData);
    InitContext.TransitionResource(Dest, D3D12_RESOURCE_STATE_GENERIC_READ);

    // Execute the command list and wait for it to finish so we can release the upload buffer
    InitContext.Finish(true);
}

void CommandContext::CopySubresource(GpuResource& Dest, UINT DestSubIndex, GpuResource& Src, UINT SrcSubIndex)
{
    FlushResourceBarriers();

    D3D12_TEXTURE_COPY_LOCATION DestLocation =
    {
        Dest.GetResource(),
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        DestSubIndex
    };

    D3D12_TEXTURE_COPY_LOCATION SrcLocation =
    {
        Src.GetResource(),
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        SrcSubIndex
    };

    m_CommandList->CopyTextureRegion(&DestLocation, 0, 0, 0, &SrcLocation, nullptr);
}

void CommandContext::InitializeTextureArraySlice(GpuResource& Dest, UINT SliceIndex, GpuResource& Src)
{
    CommandContext& Context = CommandContext::Begin();

    Context.TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST);
    Context.FlushResourceBarriers();

    const D3D12_RESOURCE_DESC& DestDesc = Dest.GetResource()->GetDesc();
    const D3D12_RESOURCE_DESC& SrcDesc = Src.GetResource()->GetDesc();

    ASSERT(SliceIndex < DestDesc.DepthOrArraySize &&
        SrcDesc.DepthOrArraySize == 1 &&
        DestDesc.Width == SrcDesc.Width &&
        DestDesc.Height == SrcDesc.Height &&
        DestDesc.MipLevels <= SrcDesc.MipLevels
    );

    UINT SubResourceIndex = SliceIndex * DestDesc.MipLevels;

    for (UINT i = 0; i < DestDesc.MipLevels; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION destCopyLocation =
        {
            Dest.GetResource(),
            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            SubResourceIndex + i
        };

        D3D12_TEXTURE_COPY_LOCATION srcCopyLocation =
        {
            Src.GetResource(),
            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            i
        };

        Context.m_CommandList->CopyTextureRegion(&destCopyLocation, 0, 0, 0, &srcCopyLocation, nullptr);
    }

    Context.TransitionResource(Dest, D3D12_RESOURCE_STATE_GENERIC_READ);
    Context.Finish(true);
}

// 暂时跳
uint32_t CommandContext::ReadbackTexture(ReadbackBuffer& DstBuffer, PixelBuffer& SrcBuffer)
{
    uint64_t CopySize = 0;

    // The footprint may depend on the device of the resource, but we assume there is only one device.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
    auto ResourceDesc = SrcBuffer.GetResource()->GetDesc();
    g_Device->GetCopyableFootprints(&ResourceDesc, 0, 1, 0,
        &PlacedFootprint, nullptr, nullptr, &CopySize);

    DstBuffer.Create(L"Readback", (uint32_t)CopySize, 1);

    TransitionResource(SrcBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, true);

    auto dst = CD3DX12_TEXTURE_COPY_LOCATION(DstBuffer.GetResource(), PlacedFootprint);
    auto res = CD3DX12_TEXTURE_COPY_LOCATION(SrcBuffer.GetResource(), 0);
    m_CommandList->CopyTextureRegion(
        &dst, 0, 0, 0,
        &res, nullptr);

    return PlacedFootprint.Footprint.RowPitch;
}

// 手动分配CPU端内存以存储并上传至GPU端
void CommandContext::InitializeBuffer(GpuBuffer& Dest, const void* BufferData, size_t NumBytes, size_t DestOffset)
{
    CommandContext& InitContext = CommandContext::Begin();

    DynAlloc mem = InitContext.ReserveUploadMemory(NumBytes); // CPU端申请内存
    SIMDMemCopy(mem.DataPtr, BufferData, Math::DivideByMultiple(NumBytes, 16)); // 数据复制至CPU端

    // copy data to the intermediate upload heap and then schedule a copy from the upload heap to the default texture
    InitContext.TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST, true);
    InitContext.m_CommandList->CopyBufferRegion(Dest.GetResource(), DestOffset, mem.Buffer.GetResource(), 0, NumBytes);
    InitContext.TransitionResource(Dest, D3D12_RESOURCE_STATE_GENERIC_READ, true);

    // Execute the command list and wait for it to finish so we can release the upload buffer
    InitContext.Finish(true);
}

// 有数据截断风险要修改，强制内存安全边界的暂存拷贝
void CommandContext::InitializeBuffer(GpuBuffer& Dest, const UploadBuffer& Src, size_t SrcOffset, size_t NumBytes, size_t DestOffset)
{

    CommandContext& InitContext = CommandContext::Begin();


    size_t MaxBytes = std::min<size_t>(Dest.GetBufferSize() - DestOffset, Src.GetBufferSize() - SrcOffset);
    // 此处有隐式拒绝超出上限数据功能，因为NumBytes未初始化，默认为极限
    NumBytes = std::min<size_t>(MaxBytes, NumBytes);

    // copy data to the intermediate upload heap and then schedule a copy from the upload heap to the default texture
    InitContext.TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST, true);
    InitContext.m_CommandList->CopyBufferRegion(Dest.GetResource(), DestOffset, (ID3D12Resource*)Src.GetResource(), SrcOffset, NumBytes);
    InitContext.TransitionResource(Dest, D3D12_RESOURCE_STATE_GENERIC_READ, true);

    // Execute the command list and wait for it to finish so we can release the upload buffer
    InitContext.Finish(true);
}

/*PIX现在不导入
void CommandContext::PIXBeginEvent(const wchar_t* label)
{
#ifdef RELEASE
    (label);
#else
    ::PIXBeginEvent(m_CommandList, 0, label);
#endif
}

void CommandContext::PIXEndEvent(void)
{
#ifndef RELEASE
    ::PIXEndEvent(m_CommandList);
#endif
}

void CommandContext::PIXSetMarker(const wchar_t* label)
{
#ifdef RELEASE
    (label);
#else
    ::PIXSetMarker(m_CommandList, 0, label);
#endif
}
*/
