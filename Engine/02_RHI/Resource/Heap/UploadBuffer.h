#pragma once

#include "02_RHI/Resource/GpuResource.h"

class UploadBuffer : public GpuResource
{
public:

    virtual ~UploadBuffer() { Destroy(); }

    // [TD-03b / 状态一致性不变量] Destroy 必须同步归零 m_BufferSize。
    // 原实现未覆写 Destroy() 且 m_BufferSize 无默认初值：释放底层 COM 后 m_pResource == nullptr
    // 但 m_BufferSize 仍保留旧值，复用方（如 DXR SBT）判断 GetBufferSize() == kTotalBytes 后
    // 跳过重建并直接调用 Map()，导致空指针解引用崩溃。
    virtual void Destroy() override
    {
        GpuResource::Destroy();
        m_BufferSize = 0;
    }

    void Create(const std::wstring& name, size_t BufferSize);

    void* Map(void);
    void Unmap(size_t begin = 0, size_t end = -1);

    size_t GetBufferSize() const { return m_BufferSize; }

protected:

    size_t m_BufferSize = 0;
};

