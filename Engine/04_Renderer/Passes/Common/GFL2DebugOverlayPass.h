// ============================================================================
// 模块：GFL2DebugOverlayPass（M2 屏幕空间硬件 8-bit Stencil 等全屏 X 光可视化 Pass）
// 架构层：Engine/04_Renderer/Passes/Common
// 职责：
//   - 当 DebugView == StencilMask (9) 时读取 SceneDepth::GetStencilSRV() 真实模板值
//   - 输出高对比伪彩色至 LinearHDRColor 供下游 TonemapPass 旁路展示
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "02_RHI/Pipeline/PipelineState.h"

class ColorBuffer;
class DepthBuffer;
class GraphicsContext;

namespace Graphics
{
	class ShaderHotReloader;
}

namespace Renderer
{
	class RenderServices;
}

namespace Renderer::GFL2
{
	class GFL2DebugOverlayPass
	{
	public:
		GFL2DebugOverlayPass() = default;
		~GFL2DebugOverlayPass() { Shutdown(); }

		GFL2DebugOverlayPass(const GFL2DebugOverlayPass&) = delete;
		GFL2DebugOverlayPass& operator=(const GFL2DebugOverlayPass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		void Execute(
			GraphicsContext& context,
			ColorBuffer& linearHDR,
			DepthBuffer& sceneDepth,
			uint32_t debugViewMode,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSO(const void* psBytecode, size_t psSize, Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;
		GraphicsPSO m_PSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO;
	};
}
