#include "PCH.h"
#include "CommandContext.h"
#include "../GpuResource/ColorBuffer.h"
#include "../GpuResource/DepthBuffer.h"
#include "../GraphicsCore.h"
#include "../GpuResource/UploadBuffer.h"

#pragma warning(push) // 将编译器的警告状态压入内部栈
#pragma warning(disable:4100) // 关闭未引用形参警告
#include <pix3.h> // 引入PIX
#pragma warning(pop) // 栈中弹处警告




using namespace Graphics;

CommandContext::CommandContext(D3D12_COMMAND_LIST_TYPE Type) : m_Type(Type) {}

//CommandContext& CommandContext::Begin(const std::wstring ID)
//{
//    CommandContext* NewContext = g_ContextManager.AllocateContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
//    NewContext->SetID(ID);
//    if (ID.length() > 0)
//        EngineProfiling::BeginBlock(ID, NewContext);
//    return *NewContext;
//}

//CommandContext& CommandContext::Begin(const std::wstring ID)
//{
//
//}

void CommandContext::TransitionResource(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate)
{

}

//void CommandContext::InitializeBuffer(GpuBuffer& Dest, const void* BufferData, size_t NumBytes, size_t DestOffset)
//{
//	CommandContext& InitContext = CommandContext::Begin();
//
//}

void InitializeTexture(GpuResource& Dest, UINT NumSubresources, D3D12_SUBRESOURCE_DATA SubData[])
{
	// 计算uploadbuffer大小。资源，起始子资源索引，子资源数量
	UINT64 uploadBufferSize = GetRequiredIntermediateSize(Dest.GetResource(), 0, NumSubresources);

	CommandContext& InitContext = CommandContext::Begin();



}