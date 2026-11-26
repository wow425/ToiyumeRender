// ============================================================================
// 模块：TAAPass（SM 6.6 Bindless 时域抗锯齿 Pass · Halton 抖动 + 重投影 + YCoCg 方差裁剪）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 7.1）
// 核心职责：
//   1) 生成 16 相 Halton(2, 3) 亚像素抖动偏移，并按右手行主序投影矩阵契约构造 JitteredProj；
//   2) 维护上一帧无抖动视图投影矩阵（m_PrevUnjitteredViewProj）与双缓冲 Ping-Pong 下标，
//      避免 Camera::Update 早退导致静止帧重投影漂移；
//   3) 经 SM 6.6 Bindless Compute 执行 3x3 最近深度膨胀 + 无抖动重投影 + Catmull-Rom 历史采样 +
//      Karis 压缩 YCoCg 方差盒裁剪，在 Linear HDR 空间输出抗锯齿图像供 TonemapPass 编码；
//   4) 支持 Resize / 开关切换 / UI 按钮触发 ResetHistory() 立即清空历史有效性标志。
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

	// 纯 CPU TAA 数学辅助（零 D3D12 依赖，供 TAAPass 与 CTest 单元测试复用）
	namespace TAAMath
	{
		// 生成第 index 项（1-based）Halton 低差异序列值 ∈ (0, 1)
		inline float Halton(uint32_t index, uint32_t base)
		{
			float f = 1.0f;
			float r = 0.0f;
			uint32_t i = index;
			while (i > 0u)
			{
				f /= static_cast<float>(base);
				r += f * static_cast<float>(i % base);
				i /= base;
			}
			return r;
		}

		// 计算 16 相居中亚像素抖动（单位：像素，范围 [-0.5, +0.5] * jitterScale）
		inline DirectX::XMFLOAT2 ComputeHaltonJitterPixels(uint32_t phaseIndex, float jitterScale = 1.0f)
		{
			const uint32_t seqIdx = (phaseIndex % 16u) + 1u;
			const float jx = (Halton(seqIdx, 2u) - 0.5f) * jitterScale;
			const float jy = (Halton(seqIdx, 3u) - 0.5f) * jitterScale;
			return DirectX::XMFLOAT2(jx, jy);
		}

		inline Math::Matrix4 OffsetProjMatrixRow2(const Math::Matrix4& proj, float offsetX, float offsetY)
		{
			Math::Matrix4 jittered = proj;
			Math::Vector4 row2 = jittered.GetZ();
			row2.SetX(static_cast<float>(row2.GetX()) + offsetX);
			row2.SetY(static_cast<float>(row2.GetY()) + offsetY);
			jittered.SetZ(row2);
			return jittered;
		}

		// 在右手系行主序透视投影矩阵上注入像素级亚像素抖动：
		//   Camera::UpdateProjMatrix 的第 2 行为 (0, 0, Q1, -1)，行向量乘法下 clip.w = -view.z。
		//   因此将 row2.x = -jitterNdcX、row2.y = -jitterNdcY 后，
		//   clip.x / clip.w = unjittered_ndc.x + jitterNdcX，精确偏移 +jitterPxX 像素！
		inline Math::Matrix4 ApplyJitterToProjMatrixRH(const Math::Matrix4& unjitteredProj,
			uint32_t width, uint32_t height, float jitterPxX, float jitterPxY)
		{
			if (width == 0 || height == 0)
				return unjitteredProj;

			const float jitterNdcX = (2.0f * jitterPxX) / static_cast<float>(width);
			const float jitterNdcY = (-2.0f * jitterPxY) / static_cast<float>(height);
			return OffsetProjMatrixRow2(unjitteredProj, -jitterNdcX, -jitterNdcY);
		}

		// 在左手系行主序透视投影矩阵上注入像素级亚像素抖动（如 UE5/GFL2 LH Reverse-Z）：
		//   Camera::UpdateProjMatrix (m_LeftHanded=true) 第 2 行为 (0, 0, Q1, +1)，行向量乘法下 clip.w = +view.z。
		//   因此 row2.x += +jitterNdcX、row2.y += +jitterNdcY 即可使 clip.xy / clip.w 精确偏移 +jitterPx 像素。
		inline Math::Matrix4 ApplyJitterToProjMatrixLH(const Math::Matrix4& unjitteredProj,
			uint32_t width, uint32_t height, float jitterPxX, float jitterPxY)
		{
			if (width == 0 || height == 0)
				return unjitteredProj;

			const float jitterNdcX = (2.0f * jitterPxX) / static_cast<float>(width);
			const float jitterNdcY = (-2.0f * jitterPxY) / static_cast<float>(height);
			return OffsetProjMatrixRow2(unjitteredProj, +jitterNdcX, +jitterNdcY);
		}
	}

	class TAAPass
	{
	public:
		// 与 TAABindlessCS.hlsl cbuffer TaaConstants : register(b2) 逐字节对齐（224B）
		struct alignas(16) Constants
		{
			Math::Matrix4     InvJitteredViewProj;             // offset   0
			Math::Matrix4     CurUnjitteredViewProj;           // offset  64
			Math::Matrix4     PrevUnjitteredViewProj;          // offset 128
			DirectX::XMFLOAT2 ScreenSize      = { 1.0f, 1.0f };// offset 192
			DirectX::XMFLOAT2 InvScreenSize   = { 1.0f, 1.0f };// offset 200
			float             BlendFactor     = 0.10f;         // offset 208
			float             VarianceGamma   = 1.25f;         // offset 212
			uint32_t          HistoryValid    = 0u;            // offset 216
			uint32_t          Pad             = 0u;            // offset 220
		};
		static_assert(sizeof(Constants) == 224 && sizeof(Constants) % 16 == 0,
			"TAAPass::Constants must match TAABindlessCS.hlsl TaaConstants (224B)");

		struct FrameJitterState
		{
			Math::Matrix4     JitteredProj;
			Math::Matrix4     JitteredViewProj;
			Math::Matrix4     InvJitteredViewProj;
			Math::Matrix4     UnjitteredViewProj;
			DirectX::XMFLOAT2 JitterPixels = { 0.0f, 0.0f };
		};

		TAAPass() = default;
		~TAAPass() = default;

		bool Initialize(RenderServices& services);
		void Shutdown();

		// 失效历史缓冲（Resize / TAA 开关从关到开 / 切管线 / UI 手动 Reset 时调用）
		void ResetHistory();

		// 计算本帧抖动投影矩阵（在 GBufferPass 之前调用，仅当 TAA 激活时推进相位）
		FrameJitterState PrepareFrame(const Scene::Camera::Camera& camera,
			uint32_t width, uint32_t height, float jitterScale);

		// 返回当前帧的 Ping-Pong 读/写槽索引（Read = 上一帧历史, Write = 本帧输出）
		uint32_t GetReadHistorySlot()  const { return m_HistoryWriteSlot ^ 1u; }
		uint32_t GetWriteHistorySlot() const { return m_HistoryWriteSlot; }
		bool     IsHistoryValid()      const { return m_HistoryValid; }

		// 执行 TAA Resolve（LinearHDR + HistoryPrev + SceneDepth -> HistoryWrite），并推进历史状态
		void Execute(GraphicsContext& context,
			const FrameBlackboard& blackboard,
			const FrameJitterState& jitterState,
			const RenderParams& params,
			uint32_t width,
			uint32_t height,
			RenderServices& services);

		void RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services);
		bool HotReloadComputeShader(const std::vector<uint8_t>& dxil, RenderServices& services, std::string& outErr);

		bool IsInitialized() const { return m_Initialized; }

	private:
		ComputePSO                                  m_ComputePSO;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_HotComputePSO;
		Math::Matrix4                               m_PrevUnjitteredViewProj = Math::Matrix4(Math::kIdentity);
		uint32_t                                    m_JitterPhaseIndex = 0;
		uint32_t                                    m_HistoryWriteSlot = 0;
		bool                                        m_HistoryValid     = false;
		bool                                        m_Initialized      = false;
	};
}
