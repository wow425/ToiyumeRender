// ============================================================================
// 模块：CharacterFaceHairEyePass（M4 AK-12 面/发/眼特化与 4 态 Stencil 状态机）
// 架构层：Engine/04_Renderer/Passes/Character
// 职责：
//   5.1 Face_Skin_EID3031       (SDF + 鼻梁高光, Always -> Replace 200)
//   5.2 Eyebrow_3062 & HairBack_3100 (Write Stencil 204 / 203, HairBack CullNone)
//   5.3 EyeBase_EID3137         (视差虹膜, Write Stencil 204, AuxMask.a=0.999)
//   5.4 HairBangs_Opaque_EID3178 (NotEqual 204 -> Replace 204)
//   5.5 BangsShadowStencil_3198 (偏移投影, 0 RTV, Equal 200 -> IncSat 201)
//   5.6 EyeBlend_Add3229 & Mul3252 (1 RTV HDR, One/One & DstColor/Zero)
//   5.7 FaceShadowRedraw_EID3295 (Equal 201 -> DecSat 200, 强制采样暗端 Ramp)
//   5.8 BangsOverEye_EID3335    (Equal 204 -> Keep, 49% Alpha 眉眼透刘海)
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
	struct RenderParams;
}

namespace Renderer::GFL2
{
	class CharacterFaceHairEyePass
	{
	public:
		CharacterFaceHairEyePass() = default;
		~CharacterFaceHairEyePass() { Shutdown(); }

		CharacterFaceHairEyePass(const CharacterFaceHairEyePass&) = delete;
		CharacterFaceHairEyePass& operator=(const CharacterFaceHairEyePass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		void Execute(
			GraphicsContext& context,
			const ::GFL2::CharacterManifestLoader& assets,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			ColorBuffer& linearHDR,
			ColorBuffer& characterAuxMask,
			DepthBuffer& sceneDepth,
			uint32_t mainShadowSrvIdx,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildAllPSOs(Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;

		// 9 个状态机专用 PSO
		GraphicsPSO m_PsoFaceOpaque;         // 3031 / 3062 (2 RTV, DepthRW, Stencil Always->Replace)
		GraphicsPSO m_PsoFaceShadowRedraw;   // 3295 (2 RTV, DepthRO, Stencil Equal 201->DecSat)
		GraphicsPSO m_PsoHairBack;           // 3100 (2 RTV, CullNone, DepthRW, Stencil Always->Replace 203)
		GraphicsPSO m_PsoHairBangsOpaque;    // 3178 (2 RTV, CullBack, DepthRW, Stencil NotEqual 204->Replace)
		GraphicsPSO m_PsoHairBangsAlpha;     // 3335 (1 RTV, CullBack, DepthRO, BlendTraditional, Stencil Equal 204->Keep)
		GraphicsPSO m_PsoBangsShadowStencil; // 3198 (0 RTV, CullBack, DepthRO, Stencil Equal 200->IncSat)
		GraphicsPSO m_PsoEyeBase;            // 3137 (2 RTV, CullBack, DepthRW, Stencil Always->Replace 204)
		GraphicsPSO m_PsoEyeAdd;             // 3229 (1 RTV, CullBack, DepthRO, BlendAdditive)
		GraphicsPSO m_PsoEyeMul;             // 3252 (1 RTV, CullBack, DepthRO, Blend DstColor/Zero)

		// 热重载槽位
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotFaceOpaque;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotFaceRedraw;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotHairBack;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotHairBangsOpaque;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotHairBangsAlpha;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBangsShadowStencil;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEyeBase;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEyeAdd;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEyeMul;

		std::vector<uint8_t> m_OpaqueVS;
		std::vector<uint8_t> m_BangsShadowVS;
		std::vector<uint8_t> m_FacePS;
		std::vector<uint8_t> m_HairPS;
		std::vector<uint8_t> m_EyePS;
	};
}
