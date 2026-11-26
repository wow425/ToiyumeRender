// ============================================================================
// 模块：LightingPass（延迟光照全屏着色 Pass · 支持 SM 6.6 Bindless Shadow + SSAO）
// 架构层：Engine/04_Renderer/Passes（WP-2A -> WP-2C -> WP-3C）
// 核心职责：
//   1) 从 FrameBlackboard 读取 GBuffer 0..3、SceneDepth 与可选 IBL 三贴图；
//   2) 在 SM 6.6 + Tier 3 硬件上自动走全局共享 BindlessGraphicsRootSignature +
//      LightingBindlessPS，直接消费 ShadowMap (Texture2DArray 3x3 PCF) 与 SSAOMap；
//   3) 保留 SM 6.4 传统描述符表路径作为硬件回退，并支持 F5 热重载；
//   4) 当 Shadow/SSAO/TAA 均关闭时与 WP-3B Golden 保持严格 0 像素差。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "00_Core/Math/VectorMath.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/Passes/IShadowAlgorithm.h"

class ColorBuffer;
class GraphicsContext;
namespace Graphics { class ShaderHotReloader; }
namespace Scene::Camera { class Camera; }

namespace Renderer
{
	class FrameBlackboard;
	class RenderServices;

	class LightingPass
	{
	public:
		// 传统 SM 6.4 回退路径的根参数槽位（t0..t7 + b0 + b1）
		enum class Slot : uint8_t
		{
			GBuffer_BaseColor = 0, // t0
			GBuffer_Normal,        // t1
			GBuffer_Material,      // t2
			GBuffer_Emission,      // t3
			Depth,                 // t4
			IBL_Irradiance,        // t5
			IBL_Prefilter,         // t6
			IBL_BRDFLUT,           // t7
			Light,                 // b0
			Camera,                // b1
			Count
		};

		// [WP-3C] 与 LightingBindlessPS.hlsl cbuffer LightingExtraCB : register(b2) 逐字节对齐（464B）
		struct alignas(16) ExtraConstants
		{
			Math::Vector4     CameraPos;                       // offset   0
			Math::Matrix4     InvViewProj;                     // offset  16
			DirectX::XMFLOAT2 RenderTargetSize    = { 1, 1 };  // offset  80
			DirectX::XMFLOAT2 InvRenderTargetSize = { 1, 1 };  // offset  88
			Math::Matrix4     ViewMatrix;                      // offset  96
			Math::Matrix4     LightViewProj[4];                // offset 160
			DirectX::XMFLOAT4 CascadeSplits       = {};        // offset 416
			DirectX::XMFLOAT4 ShadowParams0       = {};        // offset 432: x=CascadeCount y=NormalBias z=DepthBias w=PCFRadius
			DirectX::XMFLOAT4 ShadowParams1       = {};        // offset 448: x=ShadowInvRes y=ShadowStrength z=SSAOEnabled w=Unused
		};
		static_assert(sizeof(ExtraConstants) == 464 && sizeof(ExtraConstants) % 16 == 0,
			"LightingPass::ExtraConstants must match LightingBindlessPS.hlsl LightingExtraCB (464B)");

		// 每帧由 DeferredRenderer 传入的可选 WP-3C 扩展上下文（nullptr 表示全关）
		struct ExecutionExtras
		{
			const ShadowOutput*  Shadow              = nullptr;
			const ColorBuffer*   SSAOMap             = nullptr;
			bool                 EnableSSAO          = false;
			const Math::Matrix4* InvViewProjOverride = nullptr; // TAA 启用时传入抖动逆视图投影矩阵
		};

		LightingPass() = default;
		~LightingPass() = default;

		bool Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, RenderServices* services = nullptr);
		void Shutdown();

		void Execute(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const Scene::Camera::Camera& camera,
			uint32_t rtWidth,
			uint32_t rtHeight,
			RenderServices& services,
			const ExecutionExtras* extras = nullptr) const;

		bool IsInitialized() const { return m_Initialized; }
		bool IsUsingBindlessSM66() const { return m_UseBindless; }

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);
		bool HotReloadPixelShader(const std::vector<uint8_t>& dxil, bool forBindless,
			RenderServices& services, std::string& outErr);

	private:
		RootSignature                               m_RootSig;
		GraphicsPSO                                 m_PSO;
		GraphicsPSO                                 m_BindlessPSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBindlessPSO;
		DXGI_FORMAT                                 m_ColorFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT                                 m_DepthFormat = DXGI_FORMAT_UNKNOWN;
		bool                                        m_UseBindless = false;
		bool                                        m_Initialized = false;
	};
}
