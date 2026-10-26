

// 该类职责目前为Resource Allocator，负责创建和销毁全局资源
// 未来升级为Transient Resource - > FrameGraph设计

// miniegnine里该类是设计成持有并管理全局资源的
// 但该设计不合理且无法扩展
// 现代renderer设计是自己持有资源，buffermanager负责管理资源
// 转变为Pass持有资源，BufferManager负责管理资源的思想

#pragma once

#include <memory>
#include <string>

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Resource/ShadowBuffer.h"
#include "02_RHI/Resource/GpuBuffer.h"

namespace Graphics
{
	class BufferManager
	{
	public:

		// R4.5：新增 clearColor 参数（可选）。
		// ------------------------------------------------------------
		// D3D12 的 ClearRenderTargetView 必须使用与资源**创建时**
		// D3D12_CLEAR_VALUE 相同的值，否则 Debug Layer 每帧报
		//   EXECUTION WARNING #820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE
		// 并让清屏走慢路径。
		//
		// 原实现是「先 Create 再 SetClearColor」，但 Create 内部已经把
		// 当时的 m_ClearColor（默认全 0）固化进 D3D12_CLEAR_VALUE ——
		// 事后改成员变量只影响 CPU 侧，改不动资源描述。
		// 因此清屏色必须作为**创建参数**传入。
		static std::shared_ptr<ColorBuffer> CreateColorBuffer(
			const std::wstring& name,
			uint32_t width,
			uint32_t height,
			DXGI_FORMAT format,
			uint32_t mipCount = 1,
			const Color* clearColor = nullptr);

		static std::shared_ptr<DepthBuffer> CreateDepthBuffer(
			const std::wstring& name,
			uint32_t width,
			uint32_t height,
			DXGI_FORMAT format);

		static void DestroyAll();
		static void DestroyColorBuffer(const std::wstring& ResourceID);
		static void DestroyDepthBuffer(const std::wstring& ResourceID);


	private:

		static std::unordered_map<std::wstring, std::shared_ptr<ColorBuffer>> s_ColorBuffers;
		static std::unordered_map<std::wstring, std::shared_ptr<DepthBuffer>> s_DepthBuffers;
	};

} // namespace Graphics
