

// 该类职责目前为Resource Allocator，负责创建和销毁全局资源
// 未来升级为Transient Resource - > FrameGraph设计

// miniegnine里该类是设计成持有并管理全局资源的
// 但该设计不合理且无法扩展
// 现代renderer设计是自己持有资源，buffermanager负责管理资源
// 转变为Pass持有资源，BufferManager负责管理资源的思想

#pragma once

#include <memory>
#include <string>

#include "03_RHI/Resource/ColorBuffer.h"
#include "03_RHI/Resource/DepthBuffer.h"
#include "03_RHI/Resource/ShadowBuffer.h"
#include "03_RHI/Resource/GpuBuffer.h"

namespace Graphics
{
	class BufferManager
	{
	public:

		static std::shared_ptr<ColorBuffer> CreateColorBuffer(
			const std::wstring& name,
			uint32_t width,
			uint32_t height,
			DXGI_FORMAT format,
			uint32_t mipCount = 1);

		static std::shared_ptr<DepthBuffer> CreateDepthBuffer(
			const std::wstring& name,
			uint32_t width,
			uint32_t height,
			DXGI_FORMAT format);

		static void DestroyAll();

	private:

		static std::vector<std::shared_ptr<ColorBuffer>> s_ColorBuffers;
		static std::vector<std::shared_ptr<DepthBuffer>> s_DepthBuffers;
	};

} // namespace Graphics
