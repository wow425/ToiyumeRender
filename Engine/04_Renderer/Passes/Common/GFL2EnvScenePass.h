// ============================================================================
// 模块：GFL2EnvScenePass（M6 整备室静物建筑 + 天空盒 + 地板平面反射 Pass）
// 架构层：Engine/04_Renderer/Passes/Common
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
	class EnvSceneLoader;
	struct GFL2FrameConstants;
}

namespace Renderer
{
	class RenderServices;
	struct RenderParams;
}

namespace Renderer::GFL2
{
	class GFL2EnvScenePass
	{
	public:
		GFL2EnvScenePass() = default;
		~GFL2EnvScenePass() { Shutdown(); }

		GFL2EnvScenePass(const GFL2EnvScenePass&) = delete;
		GFL2EnvScenePass& operator=(const GFL2EnvScenePass&) = delete;

		bool Initialize(Renderer::RenderServices& services);
		void Shutdown();

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, Renderer::RenderServices& services);

		// 渲染 186 组静物架构 (AuxMask.a = 0.0) + 3604 天空盒 (AuxMask.a = 0.50)
		void ExecuteOpaqueAndSkybox(
			GraphicsContext& context,
			const ::GFL2::EnvSceneLoader& envAssets,
			const ::GFL2::GFL2FrameConstants& frameCB,
			const Renderer::RenderParams& params,
			ColorBuffer& linearHDR,
			ColorBuffer& characterAuxMask,
			DepthBuffer& sceneDepth,
			uint32_t mainShadowSrvIdx,
			uint32_t addLight1ShadowSrvIdx,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services);

		// 渲染镜像角色倒影至 FloorMirrorRT 并合成到地板 (EID 3843)
		void ExecuteFloorReflection(
			GraphicsContext& context,
			const ::GFL2::EnvSceneLoader& envAssets,
			const ::GFL2::CharacterManifestLoader& charAssets,
			const ::GFL2::GFL2FrameConstants& frameCB,
			ColorBuffer& floorMirrorRT,
			DepthBuffer& floorMirrorDepth,
			ColorBuffer& linearHDR,
			DepthBuffer& sceneDepth,
			uint32_t mainShadowSrvIdx,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			Renderer::RenderServices& services,
			float reflectionStrength = 0.85f);

		bool IsInitialized() const { return m_Initialized; }

	private:
		bool RebuildPSOs(Renderer::RenderServices& services, std::string& outErr);

	private:
		bool m_Initialized = false;

		GraphicsPSO m_PsoEnvCullBack;
		GraphicsPSO m_PsoEnvCullFront;
		GraphicsPSO m_PsoEnvCullNone;
		GraphicsPSO m_PsoSkybox;
		GraphicsPSO m_PsoMirrorCharBack;
		GraphicsPSO m_PsoMirrorCharFront;
		GraphicsPSO m_PsoFloorComposite;

		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEnvBack;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEnvFront;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotEnvNone;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotSkybox;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotMirrorBack;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotMirrorFront;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotFloorComposite;

		std::vector<uint8_t> m_OpaqueVS;
		std::vector<uint8_t> m_EnvPS;
		std::vector<uint8_t> m_SkyboxPS;
		std::vector<uint8_t> m_CharBodyPS;
		std::vector<uint8_t> m_FloorReflPS;
	};
}
