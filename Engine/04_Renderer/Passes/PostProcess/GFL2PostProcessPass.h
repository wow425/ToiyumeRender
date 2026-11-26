// ============================================================================
// 模块：GFL2PostProcessPass（M6 Reverse-Z SSAO + TAA + Bloom + 7281 3D LUT）
// 架构层：Engine/04_Renderer/Passes/PostProcess
// 职责：
//   - SSAO: Compute Reverse-Z AO + 双边滤波乘法合成 (严格豁免 AuxMask.a >= 0.495 角色)
//   - TAA : Compute Reverse-Z ClosestDepth(max) 重投影 + 1.5σ 方差裁剪 + 历史双缓冲
//   - Bloom: Karis 软阈值 Prefilter + 3 级 13-Tap Downsample + 3 级 Tent Additive Upsample
//   - UberPostLUT: Bloom 叠加 + Vignette + LinearToLogC + 7281 32³ 3D LUT
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "00_Core/Math/VectorMath.h"
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
	struct GFL2FrameConstants;
}

namespace Renderer
{
	class RenderServices;
	struct RenderParams;
}

namespace Renderer::GFL2
{
	struct GFL2PostTargets
	{
		static constexpr uint32_t kBloomMipCount = 4u;

		std::shared_ptr<ColorBuffer> SSAORaw;                    // Half-res R16_FLOAT
		std::shared_ptr<ColorBuffer> TAAHistory[2];              // Full-res R16G16B16A16_FLOAT
		std::shared_ptr<ColorBuffer> PostWorkHDR;                // Full-res R16G16B16A16_FLOAT
		std::shared_ptr<ColorBuffer> BloomMips[kBloomMipCount];  // 1/2, 1/4, 1/8, 1/16 R11G11B10_FLOAT
		uint32_t Width  = 0;
		uint32_t Height = 0;

		void Create(uint32_t w, uint32_t h);
		void Destroy();
	};

	class GFL2PostProcessPass
	{
	public:
		GFL2PostProcessPass() = default;
		~GFL2PostProcessPass() { Shutdown(); }

		GFL2PostProcessPass(const GFL2PostProcessPass&) = delete;
		GFL2PostProcessPass& operator=(const GFL2PostProcessPass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();
		void ResetHistory();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		void ExecuteSSAO(
			GraphicsContext& context,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			GFL2PostTargets& targets,
			ColorBuffer& linearHDR,
			ColorBuffer& auxMask,
			DepthBuffer& sceneDepth,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		void ExecuteTAA(
			GraphicsContext& context,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			GFL2PostTargets& targets,
			ColorBuffer& linearHDR,
			DepthBuffer& sceneDepth,
			Renderer::RenderServices& services);

		void ExecuteBloom(
			GraphicsContext& context,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			GFL2PostTargets& targets,
			ColorBuffer& linearHDR,
			Renderer::RenderServices& services);

		void ExecuteUberPostLUT(
			GraphicsContext& context,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			GFL2PostTargets& targets,
			ColorBuffer& linearHDR,
			uint32_t lut3DSrvIdx,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;

		ComputePSO  m_PsoSSAOCompute;
		ComputePSO  m_PsoTAACompute;
		GraphicsPSO m_PsoSSAOComposite;
		GraphicsPSO m_PsoBloomOpaque;
		GraphicsPSO m_PsoBloomUpsampleAdd;
		GraphicsPSO m_PsoUberPostLUT;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotSSAOCompute;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotTAACompute;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotSSAOComposite;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBloomOpaque;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBloomUpsampleAdd;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotUberPostLUT;

		std::vector<uint8_t> m_SSAOCS;
		std::vector<uint8_t> m_SSAOCompPS;
		std::vector<uint8_t> m_TAACS;
		std::vector<uint8_t> m_BloomPS;
		std::vector<uint8_t> m_UberPostPS;

		bool          m_TaaHistoryValid = false;
		uint32_t      m_TaaReadSlot     = 0;
		Math::Matrix4 m_PrevViewProj    = Math::Matrix4(Math::kIdentity);
		float         m_PrevCamPos[3]   = {};
	};
}
