// ============================================================================
// 模块：IShadowAlgorithm（可插拔阴影算法接口契约 + 纯 CPU 阴影数学辅助）
// 架构层：Engine/04_Renderer/Passes（WP-3C · 契约 4）
// 核心职责：
//   1) 定义 ShadowParams / ShadowOutput / IShadowAlgorithm 双实现插件接口
//      （SimpleShadowMap 单级联 vs CascadedShadowMap 2..4 级联）；
//   2) 提供与 D3D12 解耦的纯 CPU 数学函数（BuildLightOrthoMatrixRH /
//      ComputeCsmSplits / ComputeSceneBounds），供运行期与 CTest 单元测试复用。
// 线程与同步前提：单线程主循环帧边界调用；旧算法资源通过 DeferredReleaseQueue 延迟退休。
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "00_Core/Math/VectorMath.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

class DepthBuffer;
class GraphicsContext;

namespace Graphics
{
	class DeferredReleaseQueue;
	struct GpuCaps;
}

namespace Scene::Camera { class Camera; }
namespace Scene::Model  { class ModelInstance; }

namespace Renderer
{
	class RasterDrawHelper;
	class RenderServices;
	struct RenderParams;

	struct ShadowParams
	{
		uint32_t Algorithm    = 0;       // 0 = SimpleShadowMap, 1 = CSM
		uint32_t Resolution   = 2048;    // 阴影图分辨率（正方形）
		uint32_t CascadeCount = 4;       // CSM 级联数 [2..4]
		float    NormalBias   = 0.005f;
		float    DepthBias    = 0.001f;
		float    PCFRadius    = 1.5f;
		float    Strength     = 1.0f;
		float    SunDirection[3] = { 0.3f, -1.0f, -0.5f };

		static ShadowParams FromRenderParams(const RenderParams& p);
	};

	struct ShadowOutput
	{
		DepthBuffer*   ShadowMap            = nullptr; // Texture2DArray (ArraySize = 1 或 2..4)
		uint32_t       ShadowMapBindlessIdx = kInvalidBindlessIndex;
		Math::Matrix4  LightViewProj[4]     = {};
		float          SplitDepths[4]       = { 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t       CascadeCount         = 0;       // 0 = 关闭, 1 = SimpleShadowMap, 2..4 = CSM
		uint32_t       Resolution           = 0;
		float          NormalBias           = 0.005f;
		float          DepthBias            = 0.001f;
		float          PCFRadius            = 1.5f;
		float          Strength             = 1.0f;
	};

	// ShadowPass 注入给具体阴影算法的深度绘制回调（由 ShadowPass 统一维护 RS 与 PSO 缓存）
	using ShadowDrawCascadeFn = std::function<void(GraphicsContext& ctx, const Math::Matrix4& lightViewProj)>;

	class IShadowAlgorithm
	{
	public:
		virtual ~IShadowAlgorithm() = default;

		virtual const char* GetId() const = 0; // "SimpleShadowMap" | "CSM"
		virtual AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, const ShadowParams& params) const = 0;
		virtual bool Initialize(RenderServices& services, const ShadowParams& params, std::string* outErr) = 0;
		virtual void Shutdown(Graphics::DeferredReleaseQueue& releaseQueue) = 0;
		virtual DepthBuffer* GetShadowDepthBuffer() const = 0;
		virtual uint32_t GetResolution() const = 0;
		virtual uint32_t GetCascadeCount() const = 0;

		virtual ShadowOutput Render(GraphicsContext& ctx,
			const Scene::Camera::Camera& camera,
			const std::vector<Scene::Model::ModelInstance*>& models,
			const ShadowParams& params,
			RenderServices& services,
			const ShadowDrawCascadeFn& drawCascade) = 0;
	};

	// =========================================================================
	// 纯 CPU 阴影数学辅助（零 D3D12 依赖，供 SimpleShadowMap / CSM / CTest 共用）
	// =========================================================================
	namespace ShadowMath
	{
		// 构造右手系光源视图矩阵（行向量约定：v_view = mul(v_world, LightView)，前向 = -Z）。
		// lightForward 为光线传播方向（即 RenderParams.SunDirection 归一化结果，向下照射为 -Y）。
		inline Math::Matrix4 BuildLightViewMatrixRH(const Math::Vector3& lightForwardRaw, const Math::Vector3& focusCenter)
		{
			using namespace Math;
			const float lenSq = static_cast<float>(LengthSquare(lightForwardRaw));
			Vector3 forward = (lenSq > 1e-8f) ? Normalize(lightForwardRaw) : Normalize(Vector3(0.3f, -1.0f, -0.5f));

			Vector3 upRef = (std::fabs(static_cast<float>(forward.GetY())) > 0.98f)
				? Vector3(0.0f, 0.0f, 1.0f)
				: Vector3(0.0f, 1.0f, 0.0f);

			Vector3 right = Normalize(Cross(forward, upRef));
			Vector3 up    = Cross(right, forward);

			// 与 BaseCamera::UpdateCameraToWorld 完全一致：r0=Right, r1=Up, r2=-Forward, r3=Origin
			Matrix4 lightToWorld(right, up, -forward, focusCenter);
			return Invert(lightToWorld);
		}

		// 构造右手系正交投影矩阵（行向量约定，非反转 Z：vz=maxZ -> ndc.z=0，vz=minZ -> ndc.z=1）。
		// 在光源视空间中，相机沿 -Z 看去，因此更靠近光源的平面具有更大的 vz（maxZ），
		// 更远离光源的平面具有更小的 vz（minZ）。
		inline Math::Matrix4 BuildLightOrthoMatrixRH(float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
		{
			const float invW = 1.0f / (std::max)(maxX - minX, 1e-4f);
			const float invH = 1.0f / (std::max)(maxY - minY, 1e-4f);
			const float invD = 1.0f / (std::max)(maxZ - minZ, 1e-4f);

			const float sx = 2.0f * invW;
			const float sy = 2.0f * invH;
			const float sz = -invD;
			const float tx = -(maxX + minX) * invW;
			const float ty = -(maxY + minY) * invH;
			const float tz = maxZ * invD;

			return Math::Matrix4(
				Math::Vector4(sx,   0.0f, 0.0f, 0.0f),
				Math::Vector4(0.0f, sy,   0.0f, 0.0f),
				Math::Vector4(0.0f, 0.0f, sz,   0.0f),
				Math::Vector4(tx,   ty,   tz,   1.0f));
		}

		// Practical Split Scheme（PSSM，Zhang et al. 2006）：均匀分割与对数分割按 lambda 混合。
		// 返回 cascadeCount 个单调递增的视空间远平面距离（正数，单位米）。
		inline std::array<float, 4> ComputeCsmSplits(float nearZ, float farZ, uint32_t cascadeCount, float lambda = 0.75f)
		{
			std::array<float, 4> splits = { farZ, farZ, farZ, farZ };
			const uint32_t count = (std::clamp)(cascadeCount, 1u, 4u);
			const float n = (std::max)(nearZ, 0.05f);
			const float f = (std::max)(farZ, n + 0.5f);
			const float ratio = f / n;

			for (uint32_t i = 0; i < count; ++i)
			{
				const float p = static_cast<float>(i + 1) / static_cast<float>(count);
				const float logSplit = n * std::pow(ratio, p);
				const float uniSplit = n + (f - n) * p;
				splits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
			}
			return splits;
		}
	}
}
