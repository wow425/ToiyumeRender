#pragma once


#include "Resource/Buffer/ColorBuffer.h"
#include "Resource/Buffer/DepthBuffer.h"
#include "Resource/Buffer/ShadowBuffer.h"
#include "Resource/Buffer/GpuBuffer.h"
#include "RHI/GraphicsCore.h"

namespace Graphics
{
    // extern声明关键字，告知编译器该变量存在，等连接时去别处寻找
    // 打破文件重名，遵守ODR
    extern DepthBuffer g_SceneDepthBuffer;  // 场景深度缓冲区（Depth/Stencil Test）。D32_FLOAT_S8_UINT
    extern ColorBuffer g_SceneColorBuffer;  // 场景颜色缓冲区SDR。DXGI_FORMAT_R8G8B8A8_UNORM
    extern ColorBuffer g_SceneNormalBuffer; // 场景法线缓冲区（延迟渲染/PBR，屏幕空间效果）。R16G16B16A16_FLOAT




    void InitializeRenderingBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
    void ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
    void DestroyRenderingBuffers();

} // namespace Graphics
