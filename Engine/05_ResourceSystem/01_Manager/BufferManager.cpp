
/* 管理各类Buffer
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
	DepthBuffer g_DepthBuffer;
	ColorBuffer g_BaseColorBuffer;
	ColorBuffer g_NormalBuffer;
	ColorBuffer g_MaterialBuffer;

	ColorBuffer g_SceneColorBuffer;
}

#define DEPTH_FORMAT DXGI_FORMAT_D32_FLOAT
#define BASE_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define NORMAL_FORMAT DXGI_FORMAT_R16G16_FLOAT
#define MATERIAL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define HDR_COLOR_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT



#define SDR_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define NORMAL_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM
#define DSV_FORMAT DXGI_FORMAT_D24_UNORM_S8_UINT

// 初始化Color，Normal，Depth Buffer（创建对应堆与描述符）
void Graphics::InitializeRenderingBuffers(uint32_t bufferWidth, uint32_t bufferHeight)
{
	GraphicsContext& InitContext = GraphicsContext::Begin();
	// Gbuffer Pass Buffers
	{
		g_DepthBuffer.Create(L"Gbuffer Pass Depth Buffer", bufferWidth, bufferHeight, DEPTH_FORMAT);
		g_BaseColorBuffer.Create(L"Gbuffer Pass Base Color Buffer", bufferWidth, bufferHeight, 1, BASE_COLOR_FORMAT);
		g_NormalBuffer.Create(L"Gbuffer Pass Normal Buffer", bufferWidth, bufferHeight, 1, NORMAL_FORMAT);
		g_MaterialBuffer.Create(L"Gbuffer Pass Material Buffer", bufferWidth, bufferHeight, 1, MATERIAL_FORMAT);
	}

	// Lighting Pass Buffers
	{
		g_SceneColorBuffer.Create(L"Main Color Buffer", bufferWidth, bufferHeight, 1, HDR_COLOR_FORMAT);
	}





	InitContext.Finish();
}

void Graphics::ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight)
{

}

void Graphics::DestroyRenderingBuffers()
{
	// Gbuffer Pass Buffers
	{
		g_DepthBuffer.Destroy();
		g_BaseColorBuffer.Destroy();
		g_NormalBuffer.Destroy();
		g_MaterialBuffer.Destroy();
	}

	// Lighting Pass Buffers
	{
		g_SceneColorBuffer.Destroy();
	}
}
