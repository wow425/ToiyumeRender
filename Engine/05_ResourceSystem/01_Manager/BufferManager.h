#pragma once


#include "03_RHI/Resource/ColorBuffer.h"
#include "03_RHI/Resource/DepthBuffer.h"
#include "03_RHI/Resource/ShadowBuffer.h"
#include "03_RHI/Resource/GpuBuffer.h"
#include "03_RHI/GraphicsCore.h"
namespace Graphics
{
	// extern声明关键字，告知编译器该变量存在，等连接时去别处寻找
	extern DepthBuffer g_DepthBuffer;
	extern ColorBuffer g_BaseColorBuffer;
	extern ColorBuffer g_NormalBuffer;
	extern ColorBuffer g_MaterialBuffer;

	extern ColorBuffer g_SceneColorBuffer;





	void InitializeRenderingBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
	void ResizeDisplayDependentBuffers(uint32_t NativeWidth, uint32_t NativeHeight);
	void DestroyRenderingBuffers();

} // namespace Graphics
