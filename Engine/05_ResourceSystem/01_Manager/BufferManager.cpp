
/* 管理各类Buffer
*
*
*
*/






#include "00_Core/PCH.h"
#include "05_ResourceSystem/01_Manager/BufferManager.h"
#include "03_RHI/CommandSystem/CommandContext.h"




#define DEPTH_FORMAT DXGI_FORMAT_D32_FLOAT
#define BASE_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define NORMAL_FORMAT DXGI_FORMAT_R16G16_FLOAT
#define MATERIAL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define HDR_COLOR_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT



#define SDR_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define NORMAL_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM
#define DSV_FORMAT DXGI_FORMAT_D24_UNORM_S8_UINT

// 初始化Color，Normal，Depth Buffer（创建对应堆与描述符）
//void Graphics::InitializeRenderingBuffers(uint32_t bufferWidth, uint32_t bufferHeight)
//{
//	GraphicsContext& InitContext = GraphicsContext::Begin();
//
//
//
//
//
//
//	InitContext.Finish();
//}

namespace Graphics
{
	std::vector<std::shared_ptr<ColorBuffer>> BufferManager::s_ColorBuffers; // ?
	std::vector<std::shared_ptr<DepthBuffer>> BufferManager::s_DepthBuffers;

	std::shared_ptr<ColorBuffer>
		BufferManager::CreateColorBuffer(const std::wstring& name, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t mipCount)
	{
		GraphicsContext& InitContext = GraphicsContext::Begin(L"BufferManager::CreateColorBuffer");
		// 需要对format做检查

		auto buffer = std::make_shared<ColorBuffer>();
		buffer->Create(name.c_str(), width, height, mipCount, format);
		InitContext.Finish();
		s_ColorBuffers.push_back(buffer);

		return buffer;
	}

	std::shared_ptr<DepthBuffer>
		BufferManager::CreateDepthBuffer(const std::wstring& name, uint32_t width, uint32_t height, DXGI_FORMAT format)
	{
		GraphicsContext& InitContext = GraphicsContext::Begin(L"BufferManager::CreateColorBuffer");
		auto buffer = std::make_shared<DepthBuffer>();
		buffer->Create(name.c_str(), width, height, format);
		InitContext.Finish();
		s_DepthBuffers.push_back(buffer);

		return buffer;
	}

	void BufferManager::DestroyAll()
	{
		s_ColorBuffers.clear();
		s_DepthBuffers.clear();
	}
}


