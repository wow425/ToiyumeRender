// ============================================================================
// 模块：CharacterOpaquePass（AK-12 角色不透明几何 Pass · M1 BaseColor / M3 PBR+NPR）
// 架构层：Engine/04_Renderer/Passes/Character
// 职责：
//   - 消费 RenderServices::GetBindlessGraphicsRootSignature()（全程零切堆 SetDescriptorHeaps=0）
//   - 维护 Back / Front / None 三组光栅 PSO（FrontCounterClockwise = TRUE + Reverse-Z GREATER_EQUAL）
//   - 支持 ShaderHotReloader (F5) 事务化热重载与 DeferredReleaseQueue 延迟退役旧 PSO
//   - 按部位分 PIXBeginEvent/PIXEndEvent 录制 DrawIndexedInstanced 命令
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
	class CharacterOpaquePass
	{
	public:
		CharacterOpaquePass() = default;
		~CharacterOpaquePass() { Shutdown(); }

		CharacterOpaquePass(const CharacterOpaquePass&) = delete;
		CharacterOpaquePass& operator=(const CharacterOpaquePass&) = delete;

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
			Renderer::RenderServices& services,
			bool includePreM4FaceHairEye = true);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(
			const void* vsBytecode, size_t vsSize,
			const void* psBytecode, size_t psSize,
			Renderer::RenderServices& services,
			std::string& outErr);

	private:
		bool m_Initialized = false;
		GraphicsPSO m_PSO_CullBack;
		GraphicsPSO m_PSO_CullFront;
		GraphicsPSO m_PSO_CullNone;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO_Back;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO_Front;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO_None;

		std::vector<uint8_t> m_CurrentVSBytecode;
		std::vector<uint8_t> m_CurrentPSBytecode;
	};
}
