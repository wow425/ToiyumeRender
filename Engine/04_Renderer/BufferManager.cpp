
/* 管理各类Buffer
*
*
*
*/






#include "00_Core/PCH.h"
#include "04_Renderer/BufferManager.h"
#include "02_RHI/Command/CommandContext.h"




#define DEPTH_FORMAT DXGI_FORMAT_D32_FLOAT
#define BASE_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define NORMAL_FORMAT DXGI_FORMAT_R16G16_FLOAT
#define MATERIAL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

#define HDR_COLOR_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT
#define SDR_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

#define NORMAL_COLOR_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM
#define DSV_FORMAT DXGI_FORMAT_D24_UNORM_S8_UINT

namespace Graphics
{
	std::unordered_map<std::wstring, std::shared_ptr<ColorBuffer>> BufferManager::s_ColorBuffers;
	std::unordered_map<std::wstring, std::shared_ptr<DepthBuffer>> BufferManager::s_DepthBuffers;

	std::shared_ptr<ColorBuffer>
		BufferManager::CreateColorBuffer(const std::wstring& name, uint32_t width, uint32_t height,
			DXGI_FORMAT format, uint32_t mipCount, const Color* clearColor)
	{
		// GraphicsContext& InitContext = GraphicsContext::Begin(L"BufferManager::CreateColorBuffer");
		// 需要对format做检查

		auto buffer = std::make_shared<ColorBuffer>();

		// R4.5：清屏色必须在 Create 之前设好 —— Create 会把它写进
		// D3D12_CLEAR_VALUE 固化到资源上；事后再 SetClearColor 只改 CPU 副本，
		// 会导致 ClearRenderTargetView 与创建值不匹配（#820 警告）。
		if (clearColor != nullptr)
			buffer->SetClearColor(*clearColor);

		buffer->Create(name.c_str(), width, height, mipCount, format);
		// InitContext.Finish();
		s_ColorBuffers[name] = buffer;

		return buffer;
	}

	std::shared_ptr<DepthBuffer>
		BufferManager::CreateDepthBuffer(const std::wstring& name, uint32_t width, uint32_t height, DXGI_FORMAT format)
	{
		// GraphicsContext& InitContext = GraphicsContext::Begin(L"BufferManager::CreateColorBuffer");
		auto buffer = std::make_shared<DepthBuffer>();
		buffer->Create(name.c_str(), width, height, format);
		// InitContext.Finish();
		s_DepthBuffers[name] = buffer;

		return buffer;
	}

	void BufferManager::DestroyAll()
	{
		// ============================================================
		// R2：显式 Destroy，而不是只 clear 容器
		// ------------------------------------------------------------
		// clear() 只是释放「本容器持有的那一份引用」。若别处还持有
		// 同一个 shared_ptr（例如 m_GBuffers 或 Display::SceneColorBuffer），
		// 资源对象根本不会析构，其 RTV/SRV/UAV 描述符也就不会归还。
		// 这里先对每个资源调用 Destroy()，把「释放描述符」与
		// 「何时被最后一个 shared_ptr 释放」解耦。
		// Destroy() 幂等（句柄会先被置 UNKNOWN），重复调用安全。
		// ============================================================
		for (auto& kv : s_ColorBuffers)
		{
			if (kv.second) kv.second->Destroy();
		}
		for (auto& kv : s_DepthBuffers)
		{
			if (kv.second) kv.second->Destroy();
		}

		s_ColorBuffers.clear();
		s_DepthBuffers.clear();
	}

	void BufferManager::DestroyColorBuffer(const std::wstring& ResourceID)
	{
		s_ColorBuffers.erase(ResourceID);
	}

	void BufferManager::DestroyDepthBuffer(const std::wstring& ResourceID)
	{
		s_DepthBuffers.erase(ResourceID);
	}
}


