#pragma once

// constexpr constant expression 编译期常量

namespace Renderer::Deferred
{
	// 将“slot”和“format”形成强绑定。Resource Description
	static constexpr DXGI_FORMAT GBufferFormats[] =
	{
		DXGI_FORMAT_R8G8B8A8_UNORM,		// BaseColor
		DXGI_FORMAT_R16G16B16A16_FLOAT, // Normal
		DXGI_FORMAT_R16G16B16A16_FLOAT,	// Material
		DXGI_FORMAT_R8G8B8A8_UNORM,		// Emission

	};

	static constexpr DXGI_FORMAT SceneColorBufferFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	static constexpr DXGI_FORMAT SceneDepthBufferFormat = DXGI_FORMAT_D32_FLOAT;
	static constexpr DXGI_FORMAT VelocityBufferFormat = DXGI_FORMAT_R16G16_FLOAT;



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
