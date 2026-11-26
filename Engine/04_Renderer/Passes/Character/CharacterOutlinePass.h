// ============================================================================
// 模块：CharacterOutlinePass（M5 背面平滑法线挤出描边 Pass）
// 架构层：Engine/04_Renderer/Passes/Character
// 职责：
//   - 绘制 EID 3365..3577 全部 19 组 CullFront 描边
//   - EID 3577 (前刘海描边) 使用 Stencil Back NotEqual 204 避让眉眼区域
//   - 写入 LinearHDR + AuxMask.a = 0.499 (SSAO 豁免)
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
	class CharacterOutlinePass
	{
	public:
		CharacterOutlinePass() = default;
		~CharacterOutlinePass() { Shutdown(); }

		CharacterOutlinePass(const CharacterOutlinePass&) = delete;
		CharacterOutlinePass& operator=(const CharacterOutlinePass&) = delete;

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
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;
		GraphicsPSO m_PsoOutlineDefault;     // CullFront, Stencil Always Keep
		GraphicsPSO m_PsoOutlineNotEqual204; // CullFront, Stencil Back NotEqual 204 (EID 3577)

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotDefault;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotNotEqual204;

		std::vector<uint8_t> m_VS;
		std::vector<uint8_t> m_PS;
	};
}
