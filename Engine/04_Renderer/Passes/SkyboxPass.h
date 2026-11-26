// ============================================================================
// 模块：SkyboxPass（环境天空盒全屏背景 Pass）
// 架构层：Engine/04_Renderer/Passes（WP-2A 垂直试点，零依赖 LinearPassList）
// 核心职责：在 DebugView == Final 且环境 Cubemap 就绪时，以只读深度（GetDSV_DepthReadOnly）
//           与 DepthFunc=LESS_EQUAL 绘制无限远天空盒背景，保护已写入几何像素不被覆盖。
// DX12 铁律：此时 SceneDepth 处于 DEPTH_READ | PIXEL_SHADER_RESOURCE 态，严禁绑定可写 GetDSV()！
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"

class GraphicsContext;
namespace Math { class Matrix4; }
namespace Graphics { class ShaderHotReloader; }

namespace Scene::Camera { class Camera; }

namespace Renderer
{
	class FrameBlackboard;
	class RenderServices;
	struct RenderParams;

	class SkyboxPass
	{
	public:
		SkyboxPass() = default;
		~SkyboxPass() = default;

		bool Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
		void Shutdown();

		// 仅在 params.Debug == DebugView::Final 且 blackboard 含有效 EnvCube 时绘制。
		void Execute(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const Scene::Camera::Camera& camera,
			const RenderParams& params,
			RenderServices& services,
			const Math::Matrix4* projOverride = nullptr) const;

		bool IsInitialized() const { return m_Initialized; }

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);
		bool HotReloadPixelShader(const std::vector<uint8_t>& dxil, RenderServices& services, std::string& outErr);

	private:
		RootSignature m_RootSig;
		GraphicsPSO   m_PSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO;
		DXGI_FORMAT   m_ColorFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT   m_DepthFormat = DXGI_FORMAT_UNKNOWN;
		bool          m_Initialized = false;
	};
}
