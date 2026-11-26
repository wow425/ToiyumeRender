// ============================================================================
// 模块：SSAOPass（SM 6.6 Bindless 屏幕空间环境光遮蔽 + 双边深度/法线感知模糊 Pass）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 7.1）
// 核心职责：
//   1) ExecuteCompute：以 SceneDepth + GBuffer_Normal 为输入，使用 Fibonacci 半球采样 +
//      Interleaved Gradient Noise (IGN) 旋转输出原始遮蔽图 SSAORaw (R8_UNORM)；
//   2) ExecuteBlur：执行 5x5 空间/法线/线性深度联合双边滤波，输出平滑且保边的 SSAOMap；
//   3) 复用 RenderServices::GetBindlessComputeRootSignature()，零独立 RootSignature 维护、
//      整帧零额外 SetDescriptorHeaps 切堆，并接入 F5 ShaderHotReloader。
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "00_Core/Math/VectorMath.h"
#include "02_RHI/Pipeline/PipelineState.h"

class GraphicsContext;

namespace Graphics { class ShaderHotReloader; }
namespace Scene::Camera { class Camera; }

namespace Renderer
{
	class FrameBlackboard;
	class RenderServices;
	struct RenderParams;

	class SSAOPass
	{
	public:
		// 与 SSAOComputeCS.hlsl cbuffer SsaoComputeCB : register(b2) 逐字节对齐（224B）
		struct alignas(16) ComputeConstants
		{
			Math::Matrix4     InvViewProj;              // offset   0
			Math::Matrix4     ViewMatrix;               // offset  64
			Math::Matrix4     ProjMatrix;               // offset 128
			DirectX::XMFLOAT2 ScreenSize    = { 1.0f, 1.0f }; // offset 192
			DirectX::XMFLOAT2 InvScreenSize = { 1.0f, 1.0f }; // offset 200
			float             Radius        = 0.5f;     // offset 208
			float             Bias          = 0.025f;   // offset 212
			float             Power         = 1.5f;     // offset 216
			uint32_t          FrameIndex    = 0u;       // offset 220
		};
		static_assert(sizeof(ComputeConstants) == 224 && sizeof(ComputeConstants) % 16 == 0,
			"SSAOPass::ComputeConstants must match SSAOComputeCS.hlsl SsaoComputeCB (224B)");

		// 与 SSAOBlurCS.hlsl cbuffer SsaoBlurCB : register(b2) 逐字节对齐（32B）
		struct alignas(16) BlurConstants
		{
			DirectX::XMFLOAT2 ScreenSize     = { 1.0f, 1.0f }; // offset  0
			DirectX::XMFLOAT2 InvScreenSize  = { 1.0f, 1.0f }; // offset  8
			float             NearClip       = 0.1f;           // offset 16
			float             FarClip        = 1000.0f;        // offset 20
			float             DepthSharpness = 12.0f;          // offset 24
			float             Pad            = 0.0f;           // offset 28
		};
		static_assert(sizeof(BlurConstants) == 32 && sizeof(BlurConstants) % 16 == 0,
			"SSAOPass::BlurConstants must match SSAOBlurCS.hlsl SsaoBlurCB (32B)");

		SSAOPass() = default;
		~SSAOPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown();

		// 阶段 A：计算原始半球 SSAO -> SSAORawBuffer (UAV)
		void ExecuteCompute(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const Scene::Camera::Camera& camera,
			const RenderParams& params,
			uint32_t width,
			uint32_t height,
			uint32_t frameIndex,
			const Math::Matrix4* invViewProjOverride,
			const Math::Matrix4* projOverride,
			RenderServices& services) const;

		// 阶段 B：5x5 双边深度/法线感知模糊 SSAORawBuffer -> SSAOBlurBuffer (UAV)
		void ExecuteBlur(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const Scene::Camera::Camera& camera,
			uint32_t width,
			uint32_t height,
			RenderServices& services) const;

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);
		bool HotReloadComputeShader(const std::vector<uint8_t>& dxil, bool isBlur,
			RenderServices& services, std::string& outErr);

		bool IsInitialized() const { return m_Initialized; }

	private:
		ComputePSO                                  m_ComputePSO;
		ComputePSO                                  m_BlurPSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotComputePSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotBlurPSO;
		bool                                        m_Initialized = false;
	};
}
