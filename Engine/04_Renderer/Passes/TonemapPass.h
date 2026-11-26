// ============================================================================
// 模块：TonemapPass（全管线唯一共享显示编码 Pass：LinearHDR -> SceneColorLDR）
// 架构层：Engine/04_Renderer/Passes（WP-2C / 契约 5）
// 核心职责：从 FrameBlackboard::Core.LinearHDRColor (R16G16B16A16_FLOAT) 读取场景线性 HDR，
//           执行统一 Exposure -> ACES Filmic -> sRGB 编码并写入 SceneColorLDR (R8G8B8A8_UNORM)；
//           支持整帧 DebugView 旁路与逐像素（alpha < 0.5）DXR 调试旁路。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/RootSignature.h"

class GraphicsContext;
namespace Graphics { class ShaderHotReloader; }

namespace Renderer
{
	class FrameBlackboard;
	class RenderServices;
	struct RenderParams;

	class TonemapPass
	{
	public:
		// 与 TonemapPS.hlsl cbuffer TonemapConstants : register(b0) 逐字节对齐
		struct alignas(16) Constants
		{
			float    Exposure            = 1.0f; // offset  0
			uint32_t EnableTonemap       = 1u;   // offset  4
			uint32_t EnableSRGBEncode    = 1u;   // offset  8
			uint32_t BypassDisplayEncode = 0u;   // offset 12
		};
		static_assert(sizeof(Constants) == 16 && sizeof(Constants) % 16 == 0,
			"TonemapPass::Constants must match HLSL TonemapConstants (16B)");

		TonemapPass() = default;
		~TonemapPass() = default;

		// services 非空且硬件支持 SM 6.6 Bindless 时自动启用全局 BindlessRootSignature + TonemapBindlessPS；
		// 否则无缝回退至 SM 6.4 传统描述符表路径。
		bool Initialize(DXGI_FORMAT ldrFormat, RenderServices* services = nullptr);
		void Shutdown();

		void Execute(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const RenderParams& params,
			bool bypassDisplayEncode,
			const D3D12_VIEWPORT& viewport,
			const D3D12_RECT& scissor,
			RenderServices& services) const;

		bool IsInitialized() const { return m_Initialized; }
		bool IsUsingBindlessSM66() const { return m_UseBindless; }

		// [WP-3B / 契约 7.2] 注册 F5 热重载并支持事务化热替换 PSO（旧 PSO 经 DeferredReleaseQueue 延迟释放）
		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);
		bool HotReloadPixelShader(const std::vector<uint8_t>& dxil, bool forBindless,
			RenderServices& services, std::string& outErr);

	private:
		RootSignature m_RootSig;
		GraphicsPSO   m_PSO;
		GraphicsPSO   m_BindlessPSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotPSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBindlessPSO;
		DXGI_FORMAT   m_LdrFormat   = DXGI_FORMAT_UNKNOWN;
		bool          m_UseBindless = false;
		bool          m_Initialized = false;
	};
}
