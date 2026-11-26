// ============================================================================
// 模块：GFL2ShadowPass（M2 UE5 Reverse-Z 4096x4096 主方向光阴影 Pass）
// 架构层：Engine/04_Renderer/Passes/Common
// 职责：
//   - 管理并清除 4096x4096 D32_FLOAT Reverse-Z 阴影图（Clear=0.0f, GREATER_EQUAL）
//   - 消费 GFL2CharacterVertex 统一几何缓冲录制全角色 Shadow Caster (EID 2788..3178)
//   - 支持 F5 ShaderHotReloader 与层级 PIXBeginEvent/PIXEndEvent 标记
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "02_RHI/Pipeline/PipelineState.h"

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
	class GFL2ShadowPass
	{
	public:
		static constexpr uint32_t kMainShadowResolution = 4096u;

		GFL2ShadowPass() = default;
		~GFL2ShadowPass() { Shutdown(); }

		GFL2ShadowPass(const GFL2ShadowPass&) = delete;
		GFL2ShadowPass& operator=(const GFL2ShadowPass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		void Execute(
			GraphicsContext& context,
			const ::GFL2::CharacterManifestLoader& assets,
			const ::GFL2::GFL2FrameConstants& frameCB,
			DepthBuffer& shadowMap,
			Renderer::RenderServices& services,
			const wchar_t* pixSubLabel = L"MainLight_Shadow_4096 (ReverseZ)");

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(const void* vsBytecode, size_t vsSize, Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;
		GraphicsPSO m_PSO_CullBack;
		GraphicsPSO m_PSO_CullNone;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO_Back;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO_None;
		std::vector<uint8_t> m_CurrentVSBytecode;
	};
}
