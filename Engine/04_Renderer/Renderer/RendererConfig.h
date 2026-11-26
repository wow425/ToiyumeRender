#pragma once

// constexpr constant expression 编译期常量

namespace Renderer::Deferred
{
	// 将“slot”和“format”形成强绑定。Resource Description
	static constexpr DXGI_FORMAT GBufferFormats[] =
	{
		DXGI_FORMAT_R8G8B8A8_UNORM,		// BaseColor
		DXGI_FORMAT_R16G16_FLOAT,		// Normal
		DXGI_FORMAT_R16G16B16A16_FLOAT,	// Material
		DXGI_FORMAT_R8G8B8A8_UNORM,		// Emission

	};

	// [WP-2C / 契约 5] 全管线统一的场景线性 HDR 中间格式（RGBA16F）与最终 LDR 上屏格式（RGBA8）
	static constexpr DXGI_FORMAT LinearHDRBufferFormat  = DXGI_FORMAT_R16G16B16A16_FLOAT;
	static constexpr DXGI_FORMAT SceneColorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	static constexpr DXGI_FORMAT SceneDepthBufferFormat = DXGI_FORMAT_D32_FLOAT;
	static constexpr DXGI_FORMAT VelocityBufferFormat   = DXGI_FORMAT_R16G16_FLOAT;
	// [WP-3C] 延迟管线新增 ShadowMap / SSAO 目标格式（R8_UNORM 支持 Compute UAV 直写且带宽减半）
	static constexpr DXGI_FORMAT ShadowMapBufferFormat     = DXGI_FORMAT_D32_FLOAT;
	static constexpr DXGI_FORMAT SSAOBufferFormat          = DXGI_FORMAT_R8_UNORM;
	// [契约 4] DXR PrimaryRayPass UAV 浮点深度格式（与光栅 D32_FLOAT SRV R32_FLOAT 值域严格同构）
	static constexpr DXGI_FORMAT PrimarySurfaceDepthFormat = DXGI_FORMAT_R32_FLOAT;



} // namespace Renderer::Deferred

namespace Renderer::Forward
{
	struct Config
	{
		static constexpr uint32_t ForwardBufferCount = 2;
		static constexpr DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		static constexpr DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT;
	};
} // namespace Renderer::Forward
