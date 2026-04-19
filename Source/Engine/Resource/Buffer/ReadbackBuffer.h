#pragma once


#include "GpuBuffer.h"



// 回读堆是专用桥接对象，将数据跨越PCIE从GPU显存传回CPU内存，必须分配在特定的回读堆而不是默认堆，故文件分离，不放在GpuRersouce
class ReadbackBuffer : public GpuBuffer
{
public:
    virtual ~ReadbackBuffer() { Destroy(); }

    void Create(const std::wstring& name, uint32_t NumElements, uint32_t ElementSize);

    void* Map(void);
    void Unmap(void);

protected:

    void CreateDerivedViews(void) {}

};