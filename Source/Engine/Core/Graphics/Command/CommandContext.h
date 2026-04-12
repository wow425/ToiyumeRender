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
 */
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
};



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

	static void DesrtoyAllContexts(void);

	static CommandContext& Begin(const std::wstring ID = L"");
	// 刷新存在的命令但保持上下文活跃 ?
	uint64_t Flush(bool WaitForCompletion = false);
	// 刷新存在的命令和释放当前上下文
	uint64_t Finish(bool WaitForCompletion = false);
	// 准备渲染通过预留命令列表和命令分配器 ?
	void Initialize(void);

	GraphicsContext& GetGraphicsContext()
	{
		ASSERT(m_Type != D3D12_COMMAND_LIST_TYPE_COMPUTE, "Cannont convert async compute context to graphics不能将计算上下文转为图形上下文");
		return <GraphicsContext&>(*this);
	}



	static void InitializeBuffer(GpuBuffer& Dest, const UploadBuffer& Src, size_t SrcOffset, size_t NumBytes = -1, size_t DestOffset = 0);

	void TransitionResource(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate = false);

	static void InitializeTexture(GpuResource& Dest, UINT NumSubresources, D3D12_SUBRESOURCE_DATA SubData[]);

	uint32_t ReadbackTexture(ReadbackBuffer& DstBuffer, PixelBuffer& SrcBuffer);

	void SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type, ID3D12DescriptorHeap* HeapPtr);

protected:

	void BindDescriptorHeaps(void);

	CommandListManager* m_OwningManager; // 专属命令列表分配器
	ID3D12GraphicsCommandList* m_CommandList; // 图形命令列表
	ID3D12CommandAllocator* m_CurrentAllocator; // 当前命令分配器

	ID3D12RootSignature* m_CurGraphicsRootSignature; // 当前图形根签名
	ID3D12RootSignature* m_CurComputeRootSignature; // 当前计算根签名
	ID3D12PipelineState* m_CurPipelineState;       // 当前管线状态

	DynamicDescriptorHeap m_DynamicViewDescriptorHeap; // GPU端描述符堆shader-visible-Heap（CBV,SRV,UAV)
	DynamicDescriptorHeap m_DynamicSamplerDescriptorHeap; // GPU端描述符堆shader-visible-Heap（Sampler)

	D3D12_RESOURCE_BARRIER m_ResourceBarrierBuffer[16]; // 延迟资源转换所用的屏障缓冲区
	UINT m_NumBarriersToFlush; // 已存储屏障计数器

	ID3D12DescriptorHeap* m_CurrentDescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES]; // 当前CPU端描述符堆
	LinearAllocator m_CpuLinearAllocator; // 用于CPU端的内存池
	LinearAllocator m_GpiLinearAllocator; // 用于GPU端的内存池

	std::wstring m_ID; // 该上下文ID
	void SetID(const std::wstring& ID) { m_ID = ID; }

	D3D12_COMMAND_LIST_TYPE m_Type; // 该上下文类型
};

inline void CommandContext::SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE Type, ID3D12DescriptorHeap* HeapPtr)
{

}