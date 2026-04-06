#pragma once

#include "PCH.h"
#include "CommandListManager.h"
#include "Color.h"
#include "../GpuResource/GpuBuffer.h"
#include "../GpuResource/Texture.h"
#include "../GpuResource/PixelBuffer.h"
#include "../GpuResource/ReadbackBuffer.h"
#include "../GpuResource/LinearAllocator.h"
#include "../GraphicsCore.h"
#include <vector>

class ColorBuffer;
class DepthBuffer;
class Texture;
class GraphicsContext;
class ComputeContext;
class UploadBuffer;

//不可复制基类，防止对象被非法复制的一种设计模式。
// 确保资源唯一性，避免资源管理混乱和潜在的内存泄漏问题。
struct NonCopyable
{
	// 默认构造函数，允许对象被正常创建
	NonCopyable() = default;
	// 禁用拷贝构造函数和拷贝赋值运算符，防止对象被复制
	NonCopyable(const NonCopyable&) = delete;
	NonCopyable& operator = (const NonCopyable&) = delete;
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

	uint64_t Flush(bool WaitForCompletion = false);

	uint64_t Finish(bool WaitForCompletion = false);

	void Initialize(void);

	static void InitializeBuffer(GpuBuffer& Dest, const UploadBuffer& Src, size_t SrcOffset, size_t NumBytes = -1, size_t DestOffset = 0);

	void TransitionResource(GpuResource& Resource, D3D12_RESOURCE_STATES NewState, bool FlushImmediate = false);

	static void InitializeTexture(GpuResource& Dest, UINT NumSubresources, D3D12_SUBRESOURCE_DATA SubData[]);

	uint32_t ReadbackTexture(ReadbackBuffer& DstBuffer, PixelBuffer& SrcBuffer);


protected:

	D3D12_COMMAND_LIST_TYPE m_Type;

	ID3D12GraphicsCommandList* m_CommandList;
};

