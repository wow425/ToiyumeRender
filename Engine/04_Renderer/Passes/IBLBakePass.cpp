// ============================================================================
// 模块：IBLBakePass（环境图与 IBL 预计算 Pass 封装 · 实现）
// 架构层：Engine/04_Renderer/Passes
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/IBLBakePass.h"

#include "00_Core/Utility/AssetRoot.h"
#include "02_RHI/Command/CommandContext.h"
#include "04_Renderer/FrameBlackboard.h"
#include "05_Scene/Lighting/LightingSystem.h"

namespace Renderer
{
	bool IBLBakePass::Initialize(DXGI_FORMAT sceneColorFormat, DXGI_FORMAT depthFormat, const std::wstring& hdrRelativeAssetPath)
	{
		if (m_Initialized)
			return true;

		m_LoggedReady = false;
		m_EnvManager.Initialize(sceneColorFormat, depthFormat);

		const std::wstring hdrPath = Asset::Resolve(hdrRelativeAssetPath);
		if (!Asset::Exists(hdrRelativeAssetPath))
		{
			Utility::Printf(L"[Asset] 错误：缺少环境图 %ws\n", hdrPath.c_str());
			Utility::Printf(L"[Asset] 影响：Skybox 与 IBL 不可用；场景仍继续渲染，但没有环境光照。\n");
		}
		m_EnvManager.LoadHDR(hdrPath);
		m_Initialized = true;
		return true;
	}

	void IBLBakePass::Shutdown()
	{
		if (!m_Initialized)
			return;

		// [TD-03a] 必须先释放 HDRTexture TextureRef 与 4 张烘焙缓冲，确保先于 TextureManager::Shutdown。
		m_EnvManager.Shutdown();
		m_LoggedReady = false;
		m_Initialized = false;
	}

	void IBLBakePass::ExecuteBake(GraphicsContext& gfxContext)
	{
		if (!m_Initialized)
			return;

		const bool wasReady = m_EnvManager.IsReady();
		m_EnvManager.BakeEnvironmentTextures(gfxContext);

		if (m_EnvManager.IsReady())
		{
			if (!wasReady && !m_LoggedReady)
			{
				m_LoggedReady = true;
				const uint32_t mipCount = m_EnvManager.GetPrefilterMipCount();
				LOG_INFO(L"[R4] IBL 烘焙完成：HDRLoaded=%d PrefilterMips=%u (roughness 映射 0..%u)\n",
					m_EnvManager.IsHDRLoaded() ? 1 : 0,
					mipCount,
					mipCount > 1 ? mipCount - 1 : 1);
			}

			const uint32_t mipCount = m_EnvManager.GetPrefilterMipCount();
			Scene::LightingSystem::SetPrefilterMaxMip(mipCount > 1 ? static_cast<float>(mipCount - 1) : 1.0f);
		}
	}

	void IBLBakePass::PublishToBlackboard(FrameBlackboard& blackboard) const
	{
		if (!m_EnvManager.IsReady())
			return;

		blackboard.Set(FrameResourceKey::EnvCube,        const_cast<ColorBuffer*>(m_EnvManager.GetEnvironmentCube()));
		blackboard.Set(FrameResourceKey::IBL_Irradiance, const_cast<ColorBuffer*>(m_EnvManager.GetIrradianceCube()));
		blackboard.Set(FrameResourceKey::IBL_Prefilter,  const_cast<ColorBuffer*>(m_EnvManager.GetPrefilterCube()));
		blackboard.Set(FrameResourceKey::IBL_BRDFLUT,    const_cast<ColorBuffer*>(m_EnvManager.GetBRDFLUT()));
	}
}
