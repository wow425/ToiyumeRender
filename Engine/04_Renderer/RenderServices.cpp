#include "00_Core/PCH.h"
#include "04_Renderer/RenderServices.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Pipeline/SamplerManager.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
#include "05_Scene/Lighting/LightingSystem.h"

namespace Renderer
{
	RenderServices* RenderServices::s_ActiveServices = nullptr;

	bool RenderServices::Initialize(Graphics::DeferredReleaseQueue* releaseQueue)
	{
		if (m_Initialized)
			return true;

		m_ReleaseQueue = (releaseQueue != nullptr) ? releaseQueue : &Graphics::GetDeferredReleaseQueue();

		// 1. 初始化 4 分区单 Shader-Visible 堆（含 Zone A 槽 0..3 默认哨兵）
		if (!m_DescriptorHeap.Initialize())
		{
			LOG_ERROR(L"[RenderServices] UnifiedDescriptorHeap::Initialize 失败\n");
			return false;
		}

		// 2. 初始化全局纹理管理器（生命周期由 RenderServices 统一托管，切换 Renderer 不再清空）
		TextureManager::Initialize(L"");

		// 3. 初始化光照常量缓冲资源（生命周期由 RenderServices 统一托管）
		Scene::LightingSystem::InitializeResources();

		// 4. [WP-3A / 契约 7.1] 在 SM 6.6 + Tier 3 硬件上构造唯一共享 Bindless 根签名（20/64 DWORD）
		BuildBindlessRootSignatures();

		m_LastBeganFrame = 0;
		m_HasBeganFrame  = false;
		m_Initialized    = true;
		s_ActiveServices = this;

		LOG_INFO(L"[RenderServices] 鍒濆鍖栧畬鎴愶紙瀹炰緥绾ф湇鍔″凡婵€娲伙級\n");
		return true;
	}

	void RenderServices::Shutdown()
	{
		if (!m_Initialized)
			return;

		// 严格反序关闭（契约 2）：先光照 UBO -> 再纹理缓存 -> 最后销毁 4 分区单可见堆
		// 注意：调用方必须已先销毁 ImGuiLayer（归还 Zone D）、Renderer（含 EnvLighting HDR TextureRef）
		// 与全部场景 Model（归还 TextureRef 与 Zone A 材质块）。
		m_BindlessGraphicsRS.Reset(0, 0);
		m_BindlessComputeRS.Reset(0, 0);
		m_BindlessSM66Ready = false;

		Scene::LightingSystem::Shutdown();
		TextureManager::Shutdown();
		m_DescriptorHeap.Shutdown();

		if (s_ActiveServices == this)
			s_ActiveServices = nullptr;

		m_ReleaseQueue   = nullptr;
		m_LastBeganFrame = 0;
		m_HasBeganFrame  = false;
		m_Initialized    = false;

		LOG_INFO(L"[RenderServices] 宸叉寜鍙嶅簭鍏抽棴\n");
	}

	void RenderServices::BeginFrame(uint64_t frameIndex)
	{
		if (!m_Initialized)
			return;

		// 同帧幂等：避免宿主（RendererDemoApp）与 Renderer::Update 同一帧内重复推进 Zone B 游标。
		if (m_HasBeganFrame && m_LastBeganFrame == frameIndex)
			return;

		m_LastBeganFrame = frameIndex;
		m_HasBeganFrame  = true;
		m_DescriptorHeap.BeginFrame(static_cast<uint32_t>(frameIndex));
	}

	Graphics::DeferredReleaseQueue& RenderServices::GetReleaseQueue()
	{
		return (m_ReleaseQueue != nullptr) ? *m_ReleaseQueue : Graphics::GetDeferredReleaseQueue();
	}

	void RenderServices::BuildBindlessRootSignatures()
	{
		m_BindlessSM66Ready = false;
		m_BindlessGraphicsRS.Reset(0, 0);
		m_BindlessComputeRS.Reset(0, 0);

		const Graphics::GpuCaps& caps = Graphics::GetGpuCaps();
		if (!caps.SupportsBindlessSM66)
		{
			LOG_INFO(L"[RenderServices][SM6.6] 璁惧涓嶆敮鎸?SM 6.6 + Tier 3 Bindless锛屼繚鎸?SM 6.4 浼犵粺鍥為€€璺緞\n");
			return;
		}

		auto PopulateSharedLayout = [](RootSignature& rs)
		{
			// 20 / 64 DWORD锛?6 DWORD 鏍瑰父閲?(b0) + 2 DWORD Per-Frame CBV (b1) + 2 DWORD Per-Pass CBV (b2)
			rs.Reset(3, 7);
			rs[0].InitAsConstants(0, 16, D3D12_SHADER_VISIBILITY_ALL);
			rs[1].InitAsConstantBuffer(1, D3D12_SHADER_VISIBILITY_ALL);
			rs[2].InitAsConstantBuffer(2, D3D12_SHADER_VISIBILITY_ALL);

			SamplerDesc pointClamp;
			pointClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			pointClamp.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			pointClamp.MaxAnisotropy = 1;

			SamplerDesc linearClamp;
			linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			linearClamp.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			linearClamp.MaxAnisotropy = 1;

			SamplerDesc linearWrap;
			linearWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			linearWrap.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
			linearWrap.MaxAnisotropy = 1;

			SamplerDesc anisoWrap;
			anisoWrap.Filter = D3D12_FILTER_ANISOTROPIC;
			anisoWrap.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
			anisoWrap.MaxAnisotropy = 16;

			SamplerDesc shadowCmp;
			shadowCmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			shadowCmp.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			shadowCmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			shadowCmp.MaxAnisotropy = 1;

			SamplerDesc pointWrap;
			pointWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			pointWrap.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
			pointWrap.MaxAnisotropy = 1;

			// [GFL2 / UE5 Reverse-Z] 闈欐€佹瘮杈冮噰鏍峰櫒 s6锛圙REATER_EQUAL锛夛紝0 棰濆 RootSig DWORD 寮€閿€锛岄浂褰卞搷鏃㈡湁 s4 (LESS_EQUAL)
			SamplerDesc shadowCmpGreater;
			shadowCmpGreater.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			shadowCmpGreater.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			shadowCmpGreater.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			shadowCmpGreater.MaxAnisotropy = 1;

			rs.InitStaticSampler(0, pointClamp,       D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(1, linearClamp,      D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(2, linearWrap,       D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(3, anisoWrap,        D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(4, shadowCmp,        D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(5, pointWrap,        D3D12_SHADER_VISIBILITY_ALL);
			rs.InitStaticSampler(6, shadowCmpGreater, D3D12_SHADER_VISIBILITY_ALL);
		};

		PopulateSharedLayout(m_BindlessGraphicsRS);
		m_BindlessGraphicsRS.Finalize(L"BindlessGraphicsRootSignature (SM6.6)",
			D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		PopulateSharedLayout(m_BindlessComputeRS);
		m_BindlessComputeRS.Finalize(L"BindlessComputeRootSignature (SM6.6)",
			D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

		m_BindlessSM66Ready =
			(m_BindlessGraphicsRS.GetSignature() != nullptr) &&
			(m_BindlessComputeRS.GetSignature() != nullptr);

		LOG_INFO(L"[RenderServices][SM6.6] Global BindlessRootSignature ready=%d (20/64 DWORD, 7 static samplers s0..s6)\n",
			m_BindlessSM66Ready ? 1 : 0);
	}

	RenderServices* RenderServices::GetActiveServices()
	{
		return s_ActiveServices;
	}
}
