#pragma once


#include <cstdint>
#include <memory>
#include <string>

#include <d3d12.h>

class GraphicsContext;
class Scene;
class Basecamera;

namespace Renderer
{
	struct RendererCreateDesc
	{
		uint32_t width = 0;
		uint32_t height = 0;

		DXGI_FORMAT  backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // ?
		DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT;    // ?

		void* windowHandle = nullptr; // ?
	};

	struct RenderFrameDesc
	{
		const Scene* scene = nullptr; // ?
		const BaseCamera* camera = nullptr; // ?

		float deltaTime = 0.0f; // ?
		uint64_t frameIndex = 0; // ?
	};

	enum class RendererFeature : uint32_t // ?
	{
		None = 0,
		Forward = 1u << 0,
		Deferred = 1u << 1,
		Shadow = 1u << 2,
		SSAO = 1u << 3,
		TAA = 1u << 4,
		Transparent = 1u << 5,
		PostProcess = 1u << 6,
	};

	inline RendererFeature operator| (RendererFeature a, RendererFeature b)
	{
		return static_cast<RendererFeature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	inline RendererFeature operator& (RendererFeature a, RendererFeature b)
	{
		return static_cast<RendererFeature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}

	inline RendererFeature& operator|= (RendererFeature a, RendererFeature b)
	{
		a = a | b;
		return a;
	}

	inline bool HasFeature(RendererFeature mask, RendererFeature feature)
	{
		return static_cast<uint32_t>(mask & feature) != 0;
	}

	class BaseRenderer
	{
	public:
		virtual ~BaseRenderer() = default;

		virtual std::wstring GetName() const = 0; // ?


		virtual bool Initialize(const RendererCreateDesc& desc) = 0;
		virtual void Shutdown() = 0;
		virtual void OnResize(uint32_t width, uint32_t height) = 0; // ?

		virtual void BeginFrame(const RenderFrameDesc& frame) {} // ?
		virtual void Update(const RenderFrameDesc& frame) = 0;
		virtual void Render(GraphicsContext& context, const RenderFrameDesc& frame) = 0;
		virtual void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) {} // ?

		virtual RendererFeature GetFeatures() const = 0;

		bool IsInitialized() const noexcept { return m_Initialized; } // ?

	protected:
		RendererCreateDesc m_CreateDesc{};
		bool m_Initialized = false;
	};

	using RendererPtr = std::unique_ptr<BaseRenderer>; // ?
};

