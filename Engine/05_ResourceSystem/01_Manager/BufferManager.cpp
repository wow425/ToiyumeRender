
/* 管理各类Buffer
* Color Buffer：存储颜色数据，供渲染管线使用，支持RTV和UAV
* Normal Buffer：存储法线数据，供渲染管线使用，支持RTV和UAV
* Depth Buffer：存储深度数据，供渲染管线使用，支持DSV和SRV
*
*
*
*/






#include "00_Core/PCH.h"
#include "05_ResourceSystem/01_Manager/BufferManager.h"
#include "01_Application/Display.h"
#include "03_RHI/CommandSystem/CommandContext.h"
// #include "TemporalEffects.h"

namespace Graphics
{
    // 延迟渲染管线
    DepthBuffer g_SceneDepthBuffer; // 深度图
    ColorBuffer g_SceneColorBuffer; // 颜色图
    ColorBuffer g_SceneNormalBuffer; // 法线图
}

#define T2X_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM

#define SDR_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define HDR_COLOR_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT

#define NORMAL_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM

#define DSV_FORMAT DXGI_FORMAT_D24_UNORM_S8_UINT

// 初始化Color，Normal，Depth Buffer（创建对应堆与描述符）
void Graphics::InitializeRenderingBuffers(uint32_t bufferWidth, uint32_t bufferHeight)
{
    GraphicsContext& InitContext = GraphicsContext::Begin();

    g_SceneColorBuffer.Create(L"Main Color Buffer", bufferWidth, bufferHeight, 1, SDR_COLOR_FORMAT);
    g_SceneNormalBuffer.Create(L"Normals Buffer", bufferWidth, bufferHeight, 1, NORMAL_COLOR_FORMAT);
    g_SceneDepthBuffer.Create(L"Scene Depth Buffer", bufferWidth, bufferHeight, DSV_FORMAT);


    InitContext.Finish();
}

void Graphics::ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight)
{

}

void Graphics::DestroyRenderingBuffers()
{
    g_SceneDepthBuffer.Destroy();
    g_SceneColorBuffer.Destroy();
    g_SceneNormalBuffer.Destroy();
}
