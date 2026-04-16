#pragma once

#include "PCH.h"
#include "CommandListManager.h"
#include "Color.h"
#include "../PipelineState/PipelineState.h" // 没写
#include "../PipelineState/RootSignature.h"
#include "../GpuResource/GpuBuffer.h"
#include "../GpuResource/Texture.h"
#include "../GpuResource/PixelBuffer.h"
#include "../Graphics/DescriptorAllocators/DynamicDescriptorHeap.h"
#include "../GpuResource/ReadbackBuffer.h"
#include "../GpuResource/LinearAllocator.h"
#include "../Command/CommandSignature.h" // 没写
#include "../GraphicsCore.h"
#include <vector>
#include <variant> // c17改造DWParam

// 计算队列合法资源状态名单
#define VALID_COMPUTE_QUEUE_RESOURCE_STATES \
	( D3D12_RESOURCE_STATE_UNORDERED_ACCESS \
	| D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE \
	| D3D12_RESOURCE_STATE_COPY_DEST \
	| D3D12_RESOURCE_STATE_COPY_SOURCE )

class ColorBuffer;
class DepthBuffer;
class Texture;
class CommandContext;
class GraphicsContext;
class ComputeContext;
class UploadBuffer;
class ReadbackBuffer;

// 双字参数Double Word Parameter，便于辅助根常量
struct DWParam
{
	DWParam(FLOAT f) : Float(f) {}
	DWParam(UINT u) : Uint(u) {}
	DWParam(INT i) : Int(i) {}

	void operator= (FLOAT f) { Float = f; }
	void operator= (UINT u) { Uint = u; }
	void operator= (INT i) { Int = i; }

	union
	{
		FLOAT Float;
		UINT Uint;
		INT Int;
	};
};

 
 /*union现代方案variant
 get<T>(Data);会检查类型是否一致，不一致则抛出异常，确保了不会出现union的存A调B。且支持非平凡类型
 安全读取std::get/std::get_if。Data.index()可知当前存的类型。
 std::visit可实现多态效果且无虚函数表开销
 varinat大小：max（成员大小）+对齐后的索引空间，常量缓冲区禁用
struct TYDWParam
{
	std::variant<float, uint32_t, int32_t> Data;

	TYDWParam(float f) : Data(f) {}
	TYDWParam(uint32_t u) : Data(u) {}
	TYDWParam(int32_t i) : Data(i) {}

	void operator= (float f) { Data = f; }
	void operator= (uint32_t u) { Data = u; }
	void operator= (int32_t i) { Data = i; }

	float AsFloat() const { return std::get<float>(Data); }
	float AsUint32_t() const { return std::get<uint32_t>(Data); }
	float AsInt32_t() const { return std::get<int32_t>(Data); }
};*/



class ContextManager
{
public:
	ContextManager(void) {}

	CommandContext* AllocateContext(D3D12_COMMAND_LIST_TYPE Type);
	void FreeContext(CommandContext*);
	void DestroyAllContexts();

private:
	// 视为静态类成员，而不用const是因为要满足动态增长的需求。显示初始化而不放在构造函数里是因为确保创建context不重置池子
	std::vector<std::unique_ptr<CommandContext>> sm_ContextPool[4]; // 上下文池子:Direct,Bundle,Compute,Copy
	std::queue<CommandContext*> sm_AvailableContexts[4];
	std::mutex sm_ContextAllocationMutex;
};

//不可复制基类，防止对象被非法复制的一种设计模式。
// 确保资源唯一性，避免资源管理混乱和潜在的内存泄漏问题。
struct NonCopyable
{
	// 默认构造函数，允许对象被正常创建
	NonCopyable() = default;
	// 禁用拷贝构造函数和拷贝赋值运算符，防止对象被复制
	NonCopyable(const NonCopyable&) = delete;
	NonCopyable& operator= (const NonCopyable&) = delete;
};

class CommandContext : public NonCopyable
{
	friend ContextManager;
private:
	CommandContext(D3D12_COMMAND_LIST_TYPE Type);

	void Reset(void);

public:

	~CommandContext(void);
	// 销毁contexts
	static void DestroyAllContexts(void);

	static CommandContext& Begin(const std::wstring ID = L"");
	// 刷新存在的命令但保持上下文活跃 ?
	uint64_t Flush(bool WaitForCompletion = false);
	// 刷新存在的命令和释放当前上下文
	uint64_t Finish(bool WaitForCompletion = false);
	// 准备渲染通过预留命令列表和命令分配器 ?
	void Initialize(void);
     
// 获取
	GraphicsContext& GetGraphicsContext()
	{
		ASSERT(m_Type != D3D12_COMMAND_LIST_TYPE_COMPUTE, "不能将计算上下文转为图形上下文");
		return reinterpret_cast<GraphicsContext&>(*this);
	}
	ComputeContext& GetComputeContext() { return reinterpret_cast<ComputeContext&>(*this); } // 计算命令列表不用验证类别，因为计算是图形的子集，包含完整功能
	ID3D12GraphicsCommandList* GetCommandList() { return m_CommandList; }

// 复制
	void CopyBuffer(GpuResource& Dest, GpuResource& Src);
	void CopyBufferRegion(GpuResource& Dest, size_t DestOffset, GpuResource& Src, size_t SrcOffset, size_t NumBytes);
	void CopySubresource(GpuResource& Dest, UINT DestSubIndex, GpuResource& Src, UINT SrcSubIndex);
	void CopyCounter(GpuResource& Dest, size_t DestOffset, StructuredBuffer& Src);
	void CopyTextureRegion(GpuResource& Dest, UINT x, UINT y, UINT z, GpuResource& Source, RECT& rect);
	void ResetCounter(StructuredBuffer& Buf, uint32_t Value = 0);
	// 创回读堆，以行返回字节，复制纹理进去
	uint32_t ReadbackTexture(ReadbackBuffer& DstBuffer, PixelBuffer& SrcBuffer);
	// 预留上传堆内存
	DynAlloc ReserveUploadMemory(size_t SizeInBytes) { return m_CpuLinearAllocator.Allocate(SizeInBytes); }

// 初始化
	static void InitializeTexture(GpuResource& Dest, UINT NumSubresources, D3D12_SUBRESOURCE_DATA SubData[]);
	static void InitializeBuffer(GpuBuffer& Dest, const void* Data, size_t NumBytes, size_t DestOffset = 0);
	static void InitializeBuffer(GpuBuffer& Dest, const UploadBuffer& Src, size_t SrcOffset, size_t NumBytes = -1, size_t DestOffset = 0);
	static void InitializeTextureArraySlice(GpuResource& Dest, UINT SliceIndex, GpuResource& Src);

// 缓冲区写
	void WriteBuffer(GpuResource& Dest, size_t DestOffset, const void* Data, size_t NumBytes);
	void FillBuffer(GpuResource& Dest, size_t DestOffset, DWParam Value, size_t NumBytes);

// 屏障
	void TransitionResource(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate = false);
	void BeginResourceTransition(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate = false);
	void InsertUAVBarrier(GpuResource& Resource, bool FlushImmediate = false);
	void InsertAliasBarrier(GpuResource& Before, GpuResource& After, bool FlushImmediate = false);
	inline void FlushResourceBarriers(void);

// PIX调试用
	void InsertTimeStamp(ID3D12QueryHeap* pQueryHeap, uint32_t QueryIdx);
	void ResolveTimeStamps(ID3D12Resource* pReadbackHeap, ID3D12QueryHeap* pQueryHeap, uint32_t NumQueries);
	void PIXBeginEvent(const wchar_t* label);
	void PIXEndEvent(void);
	void PIXSetMarker(const wchar_t* label);

// 设置
	void SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type, ID3D12DescriptorHeap* HeapPtr);
	void SetDescriptorHeaps(UINT HeapCount, D3D12_DESCRIPTOR_HEAP_TYPE Type[], ID3D12DescriptorHeap* HeapPtrs[]);
	void SetPipelineState(const PSO& PSO);
	// 谓词渲染，高级特性，暂不实现
	void SetPredication(ID3D12Resource* Buffer, UINT64 BufferOffset, D3D12_PREDICATION_OP Op);



protected:

	void BindDescriptorHeaps(void);
	// 命令
	CommandListManager* m_OwningManager; // 专属命令列表管理器
	ID3D12GraphicsCommandList* m_CommandList; // 图形命令列表
	ID3D12CommandAllocator* m_CurrentAllocator; // 当前命令分配器
	// 根签名，管线状态
	ID3D12RootSignature* m_CurGraphicsRootSignature; // 当前图形根签名
	ID3D12RootSignature* m_CurComputeRootSignature; // 当前计算根签名
	ID3D12PipelineState* m_CurPipelineState;       // 当前管线状态
	// GPU端shader visible堆
	DynamicDescriptorHeap m_DynamicViewDescriptorHeap; // 专存（CBV,SRV,UAV)
	DynamicDescriptorHeap m_DynamicSamplerDescriptorHeap; // 专存Sampler)
	// 屏障
	D3D12_RESOURCE_BARRIER m_ResourceBarrierBuffer[16]; // 延迟资源转换所用的屏障数组
	UINT m_NumBarriersToFlush; // 已存储屏障计数器
	// cpu端描述符堆
	ID3D12DescriptorHeap* m_CurrentDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES]; // 当前CPU端描述符堆
	LinearAllocator m_CpuLinearAllocator; // 用于CPU端的内存池
	LinearAllocator m_GpuLinearAllocator; // 用于GPU端的内存池

	std::wstring m_ID; // 该上下文ID
	void SetID(const std::wstring& ID) { m_ID = ID; }

	D3D12_COMMAND_LIST_TYPE m_Type; // 该上下文类型
};

class GraphicsContext : public CommandContext
{
public:

	static GraphicsContext& Begin(const std::wstring& ID = L"") { return CommandContext::Begin(ID).GetGraphicsContext(); }

// Clear清空
	void ClearUAV(GpuBuffer& Target);
	void ClearUAV(ColorBuffer& Target);
	void ClearColor(ColorBuffer& Target, D3D12_RECT* Rect = nullptr);
	void ClearColor(ColorBuffer& Target, float Colour[4], D3D12_RECT* Rect = nullptr);
	void ClearDepth(DepthBuffer& Target);
	void ClearStencil(DepthBuffer& Target);
	void ClearDepthAndStencil(DepthBuffer& Target);

// GPU查询
	void BeginQuery(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT HeapIndex);
	void EndQuery(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT HeapIndex);
	void ResolveQueryData(ID3D12QueryHeap* QueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource* DestinationBuffer, UINT64 DestinationBufferOffset);

// Set设置
	// 根签名
	void SetRootSignature(const RootSignature& RootSig);
	// RTV,深度模板
	void SetRenderTargets(UINT NumRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[]);
	void SetRenderTargets(UINT NumRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE RTVs[], D3D12_CPU_DESCRIPTOR_HANDLE DSV);
	void SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE RTV) { SetRenderTargets(1, &RTV); }
	void SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE RTV, D3D12_CPU_DESCRIPTOR_HANDLE DSV) { SetRenderTargets(1, &RTV, DSV); }
	void SetDepthStencilTarget(D3D12_CPU_DESCRIPTOR_HANDLE DSV) { SetRenderTargets(0, nullptr, DSV); }
	// 视口，裁剪，混合，图元
	void SetViewport(const D3D12_VIEWPORT& vp);
	void SetViewport(FLOAT x, FLOAT y, FLOAT w, FLOAT h, FLOAT minDepth = 0.0f, FLOAT maxDepth = 1.0f);
	void SetScissor(const D3D12_RECT& rect);
	void SetScissor(UINT left, UINT top, UINT right, UINT bottom);
	void SetViewportAndScissor(const D3D12_VIEWPORT& vp, const D3D12_RECT& rect);
	void SetViewportAndScissor(UINT x, UINT y, UINT w, UINT h);
	void SetStencilRef(UINT StencilRef);
	void SetBlendFactor(Color BlendFactor);
	void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology);
	// 常量，SRV,UAV
	void SetConstantArray(UINT RootIndex, UINT NumConstants, const void* pConstants);
	void SetConstant(UINT RootIndex, UINT Offset, DWParam Val);
	void SetConstants(UINT RootIndex, DWParam X);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z, DWParam W);
	void SetConstantBuffer(UINT RootIndex, D3D12_GPU_VIRTUAL_ADDRESS CBV);
	void SetDynamicConstantBufferView(UINT RootIndex, size_t BufferSize, const void* BufferData);
	void SetBufferSRV(UINT RootIndex, const GpuBuffer& SRV, UINT64 Offset = 0);
	void SetBufferUAV(UINT RootIndex, const GpuBuffer& UAV, UINT64 Offset = 0);
	void SetDescriptorTable(UINT RootIndex, D3D12_GPU_DESCRIPTOR_HANDLE FirstHandle);
	// GPU端shader-visible堆
	void SetDynamicDescriptor(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle);
	void SetDynamicDescriptors(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
	void SetDynamicSampler(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle);
	void SetDynamicSamplers(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
	// 索引，顶点，动态？
	void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& IBView);
	void SetVertexBuffer(UINT Slot, const D3D12_VERTEX_BUFFER_VIEW& VBView);
	void SetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW VBViews[]);
	void SetDynamicVB(UINT Slot, size_t NumVertices, size_t VertexStride, const void* VBData);
	void SetDynamicIB(size_t IndexCount, const uint16_t* IBData);
	void SetDynamicSRV(UINT RootIndex, size_t BufferSize, const void* BufferData);

// Draw绘制
	void Draw(UINT VertexCount, UINT VertexStartOffset = 0);
	void DrawIndexed(UINT IndexCount, UINT StartIndexLocation = 0, INT BaseVertexLocation = 0);
	void DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
		UINT StartVertexLocation = 0, UINT StartInstanceLocation = 0);
	void DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
		INT BaseVertexLocation, UINT StartInstanceLocation);
	void DrawIndirect(GpuBuffer& ArgumentBuffer, uint64_t ArgumentBufferOffset = 0);
	void ExecuteIndirect(CommandSignature& CommandSig, GpuBuffer& ArgumentBuffer, uint64_t ArgumentStartOffset = 0,
		uint32_t MaxCommands = 1, GpuBuffer* CommandCounterBuffer = nullptr, uint64_t CounterOffset = 0);

private:
};

class ComputeContext : public CommandContext
{
public:
	static ComputeContext& Begin(const std::wstring& ID = L"", bool Async = false);

// Clear清空
	void ClearUAV(GpuBuffer& Target);
	void ClearUAV(ColorBuffer& Target);

// Set设置
	void SetRootSignature(const RootSignature& RootSig);

	void SetConstantArray(UINT RootIndex, UINT NumConstants, const void* pConstants);
	void SetConstant(UINT RootIndex, UINT Offset, DWParam Val);
	void SetConstants(UINT RootIndex, DWParam X);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z);
	void SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z, DWParam W);
	void SetConstantBuffer(UINT RootIndex, D3D12_GPU_VIRTUAL_ADDRESS CBV);
	void SetDynamicConstantBufferView(UINT RootIndex, size_t BufferSize, const void* BufferData);
	void SetDynamicSRV(UINT RootIndex, size_t BufferSize, const void* BufferData);
	void SetBufferSRV(UINT RootIndex, const GpuBuffer& SRV, UINT64 Offset = 0);
	void SetBufferUAV(UINT RootIndex, const GpuBuffer& UAV, UINT64 Offset = 0);
	void SetDescriptorTable(UINT RootIndex, D3D12_GPU_DESCRIPTOR_HANDLE FirstHandle);

	void SetDynamicDescriptor(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle);
	void SetDynamicDescriptors(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
	void SetDynamicSampler(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle);
	void SetDynamicSamplers(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);

// Dispatch分发
	void Dispatch(size_t GroupCountX = 1, size_t GroupCountY = 1, size_t GroupCountZ = 1);
	void Dispatch1D(size_t ThreadCountX, size_t GroupSizeX = 64);
	void Dispatch2D(size_t ThreadCountX, size_t ThreadCountY, size_t GroupSizeX = 8, size_t GroupSizeY = 8);
	void Dispatch3D(size_t ThreadCountX, size_t ThreadCountY, size_t ThreadCountZ, size_t GroupSizeX, size_t GroupSizeY, size_t GroupSizeZ);
	void DispatchIndirect(GpuBuffer& ArgumentBuffer, uint64_t ArgumentBufferOffset = 0);
	void ExecuteIndirect(CommandSignature& CommandSig, GpuBuffer& ArgumentBuffer, uint64_t ArgumentStartOffset = 0,
		uint32_t MaxCommands = 1, GpuBuffer* CommandCounterBuffer = nullptr, uint64_t CounterOffset = 0);
private:
};

// 提交屏障数组
inline void CommandContext::FlushResourceBarriers(void)
{
	if (m_NumBarriersToFlush > 0)
	{
		m_CommandList->ResourceBarrier(m_NumBarriersToFlush, m_ResourceBarrierBuffer);
		m_NumBarriersToFlush = 0;
	}
}

inline void GraphicsContext::SetRootSignature(const RootSignature& RootSig)
{
	if (RootSig.GetSignature() == m_CurGraphicsRootSignature)
		return;

	m_CommandList->SetGraphicsRootSignature(m_CurGraphicsRootSignature = RootSig.GetSignature());
	// 解析根签名，根参数填充cache
	m_DynamicViewDescriptorHeap.ParseGraphicsRootSignature(RootSig);
	m_DynamicSamplerDescriptorHeap.ParseGraphicsRootSignature(RootSig);
}

inline void ComputeContext::SetRootSignature(const RootSignature& RootSig)
{
	if (RootSig.GetSignature() == m_CurComputeRootSignature)
		return;

	m_CommandList->SetComputeRootSignature(m_CurComputeRootSignature = RootSig.GetSignature());

	m_DynamicViewDescriptorHeap.ParseComputeRootSignature(RootSig);
	m_DynamicSamplerDescriptorHeap.ParseComputeRootSignature(RootSig);
}

inline void CommandContext::SetPipelineState(const PSO& PSO)
{
	ID3D12PipelineState* PipelineState = PSO.GetPipelineStateObject();
	if (PipelineState == m_CurPipelineState)
		return;

	m_CommandList->SetPipelineState(PipelineState);
	m_CurPipelineState = PipelineState;
}

inline void GraphicsContext::SetViewportAndScissor(UINT x, UINT y, UINT w, UINT h)
{
	SetViewport((float)x, (float)y, (float)w, (float)h);
	SetScissor(x, y, x + w, y + h);
}

inline void GraphicsContext::SetScissor(UINT left, UINT top, UINT right, UINT bottom)
{
	SetScissor(CD3DX12_RECT(left, top, right, bottom));
}


inline void GraphicsContext::SetStencilRef(UINT ref)
{
	m_CommandList->OMSetStencilRef(ref);
}

inline void GraphicsContext::SetBlendFactor(Color BlendFactor)
{
	m_CommandList->OMSetBlendFactor(BlendFactor.GetPtr());
}

inline void GraphicsContext::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology)
{
	m_CommandList->IASetPrimitiveTopology(Topology);
}

// CS设置常量数组
inline void ComputeContext::SetConstantArray(UINT RootEntry, UINT NumConstants, const void* pConstants)
{
	m_CommandList->SetComputeRoot32BitConstants(RootEntry, NumConstants, pConstants, 0);
}


inline void ComputeContext::SetConstant(UINT RootEntry, UINT Offset, DWParam Val)
{
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Val.Uint, Offset);
}

inline void ComputeContext::SetConstants(UINT RootEntry, DWParam X)
{
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, X.Uint, 0);
}

inline void ComputeContext::SetConstants(UINT RootEntry, DWParam X, DWParam Y)
{
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, X.Uint, 0);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Y.Uint, 1);
}

inline void ComputeContext::SetConstants(UINT RootEntry, DWParam X, DWParam Y, DWParam Z)
{
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, X.Uint, 0);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Y.Uint, 1);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Z.Uint, 2);
}

inline void ComputeContext::SetConstants(UINT RootEntry, DWParam X, DWParam Y, DWParam Z, DWParam W)
{
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, X.Uint, 0);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Y.Uint, 1);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, Z.Uint, 2);
	m_CommandList->SetComputeRoot32BitConstant(RootEntry, W.Uint, 3);
}
//
inline void GraphicsContext::SetConstantArray(UINT RootIndex, UINT NumConstants, const void* pConstants)
{
	m_CommandList->SetGraphicsRoot32BitConstants(RootIndex, NumConstants, pConstants, 0);
}

inline void GraphicsContext::SetConstant(UINT RootEntry, UINT Offset, DWParam Val)
{
	m_CommandList->SetGraphicsRoot32BitConstant(RootEntry, Val.Uint, Offset);
}

inline void GraphicsContext::SetConstants(UINT RootIndex, DWParam X)
{
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, X.Uint, 0);
}

inline void GraphicsContext::SetConstants(UINT RootIndex, DWParam X, DWParam Y)
{
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, X.Uint, 0);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, Y.Uint, 1);
}

inline void GraphicsContext::SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z)
{
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, X.Uint, 0);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, Y.Uint, 1);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, Z.Uint, 2);
}

inline void GraphicsContext::SetConstants(UINT RootIndex, DWParam X, DWParam Y, DWParam Z, DWParam W)
{
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, X.Uint, 0);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, Y.Uint, 1);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, Z.Uint, 2);
	m_CommandList->SetGraphicsRoot32BitConstant(RootIndex, W.Uint, 3);
}

inline void ComputeContext::SetConstantBuffer(UINT RootIndex, D3D12_GPU_VIRTUAL_ADDRESS CBV)
{
	m_CommandList->SetComputeRootConstantBufferView(RootIndex, CBV);
}

inline void GraphicsContext::SetConstantBuffer(UINT RootIndex, D3D12_GPU_VIRTUAL_ADDRESS CBV)
{
	m_CommandList->SetGraphicsRootConstantBufferView(RootIndex, CBV);
}

inline void GraphicsContext::SetDynamicConstantBufferView(UINT RootIndex, size_t BufferSize, const void* BufferData)
{
	ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16)) // 16对齐
	DynAlloc cb = m_CpuLinearAllocator.Allocate(BufferSize);
	SIMDMemCopy(cb.DataPtr, BufferData, Math::AlignUp(BufferSize, 16) >> 4); // 针对WB往GPU端丢成批数据，64位用
	// memcpy(cb.DataPtr, BufferData, BufferSize);
	m_CommandList->SetGraphicsRootConstantBufferView(RootIndex, cb.GpuAddress);
}

inline void ComputeContext::SetDynamicConstantBufferView(UINT RootIndex, size_t BufferSize, const void* BufferData)
{
	ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16));
	DynAlloc cb = m_CpuLinearAllocator.Allocate(BufferSize);
	SIMDMemCopy(cb.DataPtr, BufferData, Math::AlignUp(BufferSize, 16) >> 4);
	// memcpy(cb.DataPtr, BufferData, BufferSize);
	m_CommandList->SetComputeRootConstantBufferView(RootIndex, cb.GpuAddress);
}

// VBV顶点缓冲区绑定
inline void GraphicsContext::SetDynamicVB(UINT Slot, size_t NumVertices, size_t VertexStride, const void* VertexData)
{
	ASSERT(VertexData != nullptr && Math::IsAligned(VertexData, 16));

	size_t BufferSize = Math::AlignUp(NumVertices * VertexStride, 16);
	DynAlloc vb = m_CpuLinearAllocator.Allocate(BufferSize);

	SIMDMemCopy(vb.DataPtr, VertexData, BufferSize >> 4);

	D3D12_VERTEX_BUFFER_VIEW VBView;
	VBView.BufferLocation = vb.GpuAddress;
	VBView.SizeInBytes = (UINT)BufferSize;
	VBView.StrideInBytes = (UINT)VertexStride;

	m_CommandList->IASetVertexBuffers(Slot, 1, &VBView);
}

// 索引缓冲区绑定
inline void GraphicsContext::SetDynamicIB(size_t IndexCount, const uint16_t* IndexData)
{
	ASSERT(IndexData != nullptr && Math::IsAligned(IndexData, 16));

	size_t BufferSize = Math::AlignUp(IndexCount * sizeof(uint16_t), 16);
	DynAlloc ib = m_CpuLinearAllocator.Allocate(BufferSize);

	SIMDMemCopy(ib.DataPtr, IndexData, BufferSize >> 4);

	D3D12_INDEX_BUFFER_VIEW IBView;
	IBView.BufferLocation = ib.GpuAddress;
	IBView.SizeInBytes = (UINT)(IndexCount * sizeof(uint16_t));
	IBView.Format = DXGI_FORMAT_R16_UINT;

	m_CommandList->IASetIndexBuffer(&IBView);
}

// 动态SRV绑定
inline void GraphicsContext::SetDynamicSRV(UINT RootIndex, size_t BufferSize, const void* BufferData)
{
	ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16));
	DynAlloc cb = m_CpuLinearAllocator.Allocate(BufferSize);
	SIMDMemCopy(cb.DataPtr, BufferData, Math::AlignUp(BufferSize, 16) >> 4);
	m_CommandList->SetGraphicsRootShaderResourceView(RootIndex, cb.GpuAddress);
}

inline void ComputeContext::SetDynamicSRV(UINT RootIndex, size_t BufferSize, const void* BufferData)
{
	ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16));
	DynAlloc cb = m_CpuLinearAllocator.Allocate(BufferSize);
	SIMDMemCopy(cb.DataPtr, BufferData, Math::AlignUp(BufferSize, 16) >> 4);
	m_CommandList->SetComputeRootShaderResourceView(RootIndex, cb.GpuAddress);
}

inline void GraphicsContext::SetBufferSRV(UINT RootIndex, const GpuBuffer& SRV, UINT64 Offset)
{
	ASSERT((SRV.m_UsageState & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0);
	m_CommandList->SetGraphicsRootShaderResourceView(RootIndex, SRV.GetGpuVirtualAddress() + Offset);
}

inline void ComputeContext::SetBufferSRV(UINT RootIndex, const GpuBuffer& SRV, UINT64 Offset)
{
	ASSERT((SRV.m_UsageState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0);
	m_CommandList->SetComputeRootShaderResourceView(RootIndex, SRV.GetGpuVirtualAddress() + Offset);
}

inline void GraphicsContext::SetBufferUAV(UINT RootIndex, const GpuBuffer& UAV, UINT64 Offset)
{
	ASSERT((UAV.m_UsageState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0);
	m_CommandList->SetGraphicsRootUnorderedAccessView(RootIndex, UAV.GetGpuVirtualAddress() + Offset);
}

inline void ComputeContext::SetBufferUAV(UINT RootIndex, const GpuBuffer& UAV, UINT64 Offset)
{
	ASSERT((UAV.m_UsageState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0);
	m_CommandList->SetComputeRootUnorderedAccessView(RootIndex, UAV.GetGpuVirtualAddress() + Offset);
}

inline void ComputeContext::Dispatch(size_t GroupCountX, size_t GroupCountY, size_t GroupCountZ)
{
	// 屏障提交
	FlushResourceBarriers();
	// shader-visible Heap，描述符上传至根签名cache？
	m_DynamicViewDescriptorHeap.CommitComputeRootDescriptorTables(m_CommandList);
	m_DynamicSamplerDescriptorHeap.CommitComputeRootDescriptorTables(m_CommandList);
	m_CommandList->Dispatch((UINT)GroupCountX, (UINT)GroupCountY, (UINT)GroupCountZ);
}

inline void ComputeContext::Dispatch1D(size_t ThreadCountX, size_t GroupSizeX)
{
	Dispatch(Math::DivideByMultiple(ThreadCountX, GroupSizeX), 1, 1);
}

inline void ComputeContext::Dispatch2D(size_t ThreadCountX, size_t ThreadCountY, size_t GroupSizeX, size_t GroupSizeY)
{
	Dispatch(
		Math::DivideByMultiple(ThreadCountX, GroupSizeX),
		Math::DivideByMultiple(ThreadCountY, GroupSizeY), 1);
}

inline void ComputeContext::Dispatch3D(size_t ThreadCountX, size_t ThreadCountY, size_t ThreadCountZ, size_t GroupSizeX, size_t GroupSizeY, size_t GroupSizeZ)
{
	Dispatch(
		Math::DivideByMultiple(ThreadCountX, GroupSizeX),
		Math::DivideByMultiple(ThreadCountY, GroupSizeY),
		Math::DivideByMultiple(ThreadCountZ, GroupSizeZ));
}

inline void CommandContext::SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type, ID3D12DescriptorHeap* HeapPtr)
{
	if (m_CurrentDescriptorHeaps[Type] != HeapPtr)
	{
		m_CurrentDescriptorHeaps[Type] = HeapPtr;
		BindDescriptorHeaps();
	}
}

inline void CommandContext::SetDescriptorHeaps(UINT HeapCount, D3D12_DESCRIPTOR_HEAP_TYPE Type[], ID3D12DescriptorHeap* HeapPtrs[])
{
	bool AnyChanged = false;

	for (UINT i = 0; i < HeapCount; ++i)
	{
		if (m_CurrentDescriptorHeaps[Type[i]] != HeapPtrs[i])
		{
			m_CurrentDescriptorHeaps[Type[i]] = HeapPtrs[i];
			AnyChanged = true;
		}
	}

	if (AnyChanged)
		BindDescriptorHeaps();
}

// 谓词渲染。允许 GPU 根据一个缓冲区的计算结果，动态决定是否跳过后续的一系列渲染命令，而无需 CPU 介入。没学过的优化手段
inline void CommandContext::SetPredication(ID3D12Resource* Buffer, UINT64 BufferOffset, D3D12_PREDICATION_OP Op)
{
	m_CommandList->SetPredication(Buffer, BufferOffset, Op);
}

inline void GraphicsContext::SetDynamicDescriptor(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
	SetDynamicDescriptors(RootIndex, Offset, 1, &Handle);
}

inline void ComputeContext::SetDynamicDescriptor(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
	SetDynamicDescriptors(RootIndex, Offset, 1, &Handle);
}

inline void GraphicsContext::SetDynamicDescriptors(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
	m_DynamicViewDescriptorHeap.SetGraphicsDescriptorHandles(RootIndex, Offset, Count, Handles);
}

inline void ComputeContext::SetDynamicDescriptors(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
	m_DynamicViewDescriptorHeap.SetComputeDescriptorHandles(RootIndex, Offset, Count, Handles);
}

inline void GraphicsContext::SetDynamicSampler(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
	SetDynamicSamplers(RootIndex, Offset, 1, &Handle);
}

inline void GraphicsContext::SetDynamicSamplers(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
	m_DynamicSamplerDescriptorHeap.SetGraphicsDescriptorHandles(RootIndex, Offset, Count, Handles);
}

inline void ComputeContext::SetDynamicSampler(UINT RootIndex, UINT Offset, D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
	SetDynamicSamplers(RootIndex, Offset, 1, &Handle);
}

inline void ComputeContext::SetDynamicSamplers(UINT RootIndex, UINT Offset, UINT Count, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
	m_DynamicSamplerDescriptorHeap.SetComputeDescriptorHandles(RootIndex, Offset, Count, Handles);
}

inline void GraphicsContext::SetDescriptorTable(UINT RootIndex, D3D12_GPU_DESCRIPTOR_HANDLE FirstHandle)
{
	m_CommandList->SetGraphicsRootDescriptorTable(RootIndex, FirstHandle);
}

inline void ComputeContext::SetDescriptorTable(UINT RootIndex, D3D12_GPU_DESCRIPTOR_HANDLE FirstHandle)
{
	m_CommandList->SetComputeRootDescriptorTable(RootIndex, FirstHandle);
}


inline void GraphicsContext::SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& IBView)
{
	m_CommandList->IASetIndexBuffer(&IBView);
}

inline void GraphicsContext::SetVertexBuffer(UINT Slot, const D3D12_VERTEX_BUFFER_VIEW& VBView)
{
	SetVertexBuffers(Slot, 1, &VBView);
}

inline void GraphicsContext::SetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW VBViews[])
{
	m_CommandList->IASetVertexBuffers(StartSlot, Count, VBViews);
}

inline void GraphicsContext::Draw(UINT VertexCount, UINT VertexStartOffset)
{
	DrawInstanced(VertexCount, 1, VertexStartOffset, 0);
}

inline void GraphicsContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	DrawIndexedInstanced(IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0);
}

inline void GraphicsContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
	UINT StartVertexLocation, UINT StartInstanceLocation)
{
	FlushResourceBarriers();
	m_DynamicViewDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_DynamicSamplerDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_CommandList->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

inline void GraphicsContext::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
	INT BaseVertexLocation, UINT StartInstanceLocation)
{
	FlushResourceBarriers();
	m_DynamicViewDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_DynamicSamplerDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_CommandList->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

/*
间接执行，没学过
inline void GraphicsContext::ExecuteIndirect(CommandSignature& CommandSig,
	GpuBuffer& ArgumentBuffer, uint64_t ArgumentStartOffset,
	uint32_t MaxCommands, GpuBuffer* CommandCounterBuffer, uint64_t CounterOffset)
{
	FlushResourceBarriers();
	m_DynamicViewDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_DynamicSamplerDescriptorHeap.CommitGraphicsRootDescriptorTables(m_CommandList);
	m_CommandList->ExecuteIndirect(CommandSig.GetSignature(), MaxCommands,
		ArgumentBuffer.GetResource(), ArgumentStartOffset,
		CommandCounterBuffer == nullptr ? nullptr : CommandCounterBuffer->GetResource(), CounterOffset);
}


inline void GraphicsContext::DrawIndirect(GpuBuffer& ArgumentBuffer, uint64_t ArgumentBufferOffset)
{
	ExecuteIndirect(Graphics::DrawIndirectCommandSignature, ArgumentBuffer, ArgumentBufferOffset);
}

inline void ComputeContext::ExecuteIndirect(CommandSignature& CommandSig,
	GpuBuffer& ArgumentBuffer, uint64_t ArgumentStartOffset,
	uint32_t MaxCommands, GpuBuffer* CommandCounterBuffer, uint64_t CounterOffset)
{
	FlushResourceBarriers();
	m_DynamicViewDescriptorHeap.CommitComputeRootDescriptorTables(m_CommandList);
	m_DynamicSamplerDescriptorHeap.CommitComputeRootDescriptorTables(m_CommandList);
	m_CommandList->ExecuteIndirect(CommandSig.GetSignature(), MaxCommands,
		ArgumentBuffer.GetResource(), ArgumentStartOffset,
		CommandCounterBuffer == nullptr ? nullptr : CommandCounterBuffer->GetResource(), CounterOffset);
}

inline void ComputeContext::DispatchIndirect(GpuBuffer& ArgumentBuffer, uint64_t ArgumentBufferOffset)
{
	ExecuteIndirect(Graphics::DispatchIndirectCommandSignature, ArgumentBuffer, ArgumentBufferOffset);
}
 */

inline void CommandContext::CopyBuffer(GpuResource& Dest, GpuResource& Src)
{
	TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushResourceBarriers();
	m_CommandList->CopyResource(Dest.GetResource(), Src.GetResource());
}

inline void CommandContext::CopyBufferRegion(GpuResource& Dest, size_t DestOffset, GpuResource& Src, size_t SrcOffset, size_t NumBytes)
{
	TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(Src, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushResourceBarriers();
	m_CommandList->CopyBufferRegion(Dest.GetResource(), DestOffset, Src.GetResource(), SrcOffset, NumBytes);
}

inline void CommandContext::CopyCounter(GpuResource& Dest, size_t DestOffset, StructuredBuffer& Src)
{
	TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(Src.GetCounterBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushResourceBarriers();
	m_CommandList->CopyBufferRegion(Dest.GetResource(), DestOffset, Src.GetCounterBuffer().GetResource(), 0, 4);
}

// 虚拟纹理，UI更新用。没学过
inline void CommandContext::CopyTextureRegion(GpuResource& Dest, UINT x, UINT y, UINT z, GpuResource& Source, RECT& Rect)
{
	TransitionResource(Dest, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(Source, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushResourceBarriers();

	D3D12_TEXTURE_COPY_LOCATION destLoc = CD3DX12_TEXTURE_COPY_LOCATION(Dest.GetResource(), 0);
	D3D12_TEXTURE_COPY_LOCATION srcLoc = CD3DX12_TEXTURE_COPY_LOCATION(Source.GetResource(), 0);

	D3D12_BOX box = {};
	box.back = 1;
	box.left = Rect.left;
	box.right = Rect.right;
	box.top = Rect.top;
	box.bottom = Rect.bottom;

	m_CommandList->CopyTextureRegion(&destLoc, x, y, z, &srcLoc, &box);
}

// GPU端计数器，如粒子系统，视锥体剔除用。没学过
inline void CommandContext::ResetCounter(StructuredBuffer& Buf, uint32_t Value)
{
	FillBuffer(Buf.GetCounterBuffer(), 0, Value, sizeof(uint32_t));
	TransitionResource(Buf.GetCounterBuffer(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// 时间戳，性能分析用。没学过
inline void CommandContext::InsertTimeStamp(ID3D12QueryHeap* pQueryHeap, uint32_t QueryIdx)
{
	m_CommandList->EndQuery(pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, QueryIdx);
}

inline void CommandContext::ResolveTimeStamps(ID3D12Resource* pReadbackHeap, ID3D12QueryHeap* pQueryHeap, uint32_t NumQueries)
{
	m_CommandList->ResolveQueryData(pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, NumQueries, pReadbackHeap, 0);
}
