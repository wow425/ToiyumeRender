// ============================================================================
// 模块：CharacterTransparentPass（M5 5组双面半透明布料/挂件/瞄具镜片 Pass）
// 架构层：Engine/04_Renderer/Passes/Character
// 职责：
//   - 按原帧顺序成对执行 CullFront -> CullBack (EID 4128..4421)
//   - RT0 使用 BlendPreMultiplied (One, InvSrcAlpha) 保持高光不衰减
//   - RT1 使用 IndependentBlendEnable 覆盖写入 AuxMask.a = 0.498 (SSAO 豁免)
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

namespace GFL2
{
	class CharacterManifestLoader;
	struct GFL2FrameConstants;
}

namespace Renderer
{
	class RenderServices;
}

namespace Renderer::GFL2
{
	class CharacterTransparentPass
	{
	public:
		CharacterTransparentPass() = default;
		~CharacterTransparentPass() { Shutdown(); }

		CharacterTransparentPass(const CharacterTransparentPass&) = delete;
		CharacterTransparentPass& operator=(const CharacterTransparentPass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		void Execute(
			GraphicsContext& context,
			const ::GFL2::CharacterManifestLoader& assets,
			const ::GFL2::GFL2FrameConstants& frameCB,
			ColorBuffer& linearHDR,
			ColorBuffer& characterAuxMask,
			DepthBuffer& sceneDepth,
			uint32_t mainShadowSrvIdx,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;
		GraphicsPSO m_PsoCullFront; // Stencil Always Keep
		GraphicsPSO m_PsoCullBack;  // Stencil Always Replace 200

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotFront;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBack;

		std::vector<uint8_t> m_VS;
		std::vector<uint8_t> m_PS;
	};
}
