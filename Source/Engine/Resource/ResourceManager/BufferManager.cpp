#include "PCH.h"
#include "BufferManager.h"
#include "Display.h"
#include "../RHI/Command/CommandContext.h"
// #include "TemporalEffects.h"

namespace Graphics
{
    // 延迟渲染管线
    DepthBuffer g_SceneDepthBuffer; // 深度图
    ColorBuffer g_SceneColorBuffer; // 颜色图
    ColorBuffer g_SceneNormalBuffer; // 法线图

    DXGI_FORMAT DefaultHdrColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

#define T2X_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM
#define HDR_MOTION_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT
#define DSV_FORMAT DXGI_FORMAT_D32_FLOAT

void Graphics::InitializeRenderingBuffers(uint32_t bufferWidth, uint32_t bufferHeight)
{
    GraphicsContext& InitContext = GraphicsContext::Begin();

    g_SceneColorBuffer.Create(L"Main Color Buffer", bufferWidth, bufferHeight, 1, DefaultHdrColorFormat);
    g_SceneNormalBuffer.Create(L"Normals Buffer", bufferWidth, bufferHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
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
