// ============================================================================
// 模块：CascadedShadowMap（2..4 级联阴影图 CSM · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/CascadedShadowMap.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Model/Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Renderer
{
	namespace
	{
		// 计算主相机视锥在 [sliceNear, sliceFar] 区间内的 8 个世界空间角点（右手系，前向 -Z）。
		void ComputeFrustumSliceCornersWS(const Scene::Camera::Camera& camera,
			float sliceNear, float sliceFar, Math::Vector3 outCorners[8])
		{
			using namespace Math;
			const Matrix4 invView = Invert(camera.GetViewMatrix());
			const Matrix4& proj   = camera.GetProjMatrix();
			// 从投影矩阵主对角线反推 X = 1/(aspect*tan(fov/2)), Y = 1/tan(fov/2)，兼容任意宽高比
			const float projX = (std::max)(std::fabs(static_cast<float>(proj.GetX().GetX())), 1e-4f);
			const float projY = (std::max)(std::fabs(static_cast<float>(proj.GetY().GetY())), 1e-4f);

			const float nearHalfW = sliceNear / projX;
			const float nearHalfH = sliceNear / projY;
			const float farHalfW  = sliceFar  / projX;
			const float farHalfH  = sliceFar  / projY;

			// 视空间角点（相机朝 -Z，故 z = -sliceNear / -sliceFar）
			const Vector3 cornersVS[8] = {
				Vector3(-nearHalfW, -nearHalfH, -sliceNear),
				Vector3( nearHalfW, -nearHalfH, -sliceNear),
				Vector3(-nearHalfW,  nearHalfH, -sliceNear),
				Vector3( nearHalfW,  nearHalfH, -sliceNear),
				Vector3(-farHalfW,  -farHalfH,  -sliceFar),
				Vector3( farHalfW,  -farHalfH,  -sliceFar),
				Vector3(-farHalfW,   farHalfH,  -sliceFar),
				Vector3( farHalfW,   farHalfH,  -sliceFar),
			};

			for (int i = 0; i < 8; ++i)
			{
				const Vector4 w = invView * Vector4(cornersVS[i], 1.0f);
				outCorners[i] = Vector3(w);
			}
		}

		void ComputeSceneSphere(const std::vector<Scene::Model::ModelInstance*>& models,
			Math::Vector3& outCenter, float& outRadius)
		{
			using namespace Math;
			bool hasAny = false;
			Vector3 minPt(FLT_MAX, FLT_MAX, FLT_MAX);
			Vector3 maxPt(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			for (const auto* inst : models)
			{
				if (inst == nullptr || inst->IsNull())
					continue;
				const Vector3 c = inst->GetCenter();
				const float r = (std::max)(static_cast<float>(inst->GetRadius()), 0.25f);
				const Vector3 ext(r, r, r);
				minPt = Min(minPt, c - ext);
				maxPt = Max(maxPt, c + ext);
				hasAny = true;
			}
			if (!hasAny)
			{
				outCenter = Vector3(0.0f, 0.0f, 0.0f);
				outRadius = 5.0f;
				return;
			}
			outCenter = (minPt + maxPt) * 0.5f;
			outRadius = (std::max)(static_cast<float>(Length((maxPt - minPt) * 0.5f)), 1.0f);
		}
	}

	AlgorithmCapsReport CascadedShadowMap::CheckSupport(const Graphics::GpuCaps& caps, const ShadowParams& params) const
	{
		(void)caps;
		AlgorithmCapsReport report;
		const uint32_t res      = (std::clamp)(params.Resolution, 256u, 4096u);
		const uint32_t cascades = (std::clamp)(params.CascadeCount, 2u, 4u);
		report.Supported = true;
		report.EstimatedVramBytes = static_cast<uint64_t>(res) * static_cast<uint64_t>(res) * 4ull * cascades;
		return report;
	}

	bool CascadedShadowMap::Initialize(RenderServices& services, const ShadowParams& params, std::string* outErr)
	{
		(void)services;
		const uint32_t res      = (std::clamp)(params.Resolution, 256u, 4096u);
		const uint32_t cascades = (std::clamp)(params.CascadeCount, 2u, 4u);

		auto buf = std::make_shared<DepthBuffer>(1.0f, 0);
		buf->CreateArray(L"ShadowMap CSM DepthArray (D32)", res, res, cascades, Renderer::Deferred::ShadowMapBufferFormat);
		if (buf->GetResource() == nullptr)
		{
			if (outErr) *outErr = "CascadedShadowMap: failed to create DepthBuffer Texture2DArray";
			return false;
		}
		m_ShadowMap    = std::move(buf);
		m_Resolution   = res;
		m_CascadeCount = cascades;
		return true;
	}

	void CascadedShadowMap::Shutdown(Graphics::DeferredReleaseQueue& releaseQueue)
	{
		if (m_ShadowMap != nullptr)
		{
			std::shared_ptr<DepthBuffer> retired = std::move(m_ShadowMap);
			releaseQueue.EnqueueCallback([retired = std::move(retired)]() mutable {
				retired.reset();
			});
		}
		m_Resolution   = 0;
		m_CascadeCount = 0;
	}

	ShadowOutput CascadedShadowMap::Render(GraphicsContext& ctx,
		const Scene::Camera::Camera& camera,
		const std::vector<Scene::Model::ModelInstance*>& models,
		const ShadowParams& params,
		RenderServices& services,
		const ShadowDrawCascadeFn& drawCascade)
	{
		ShadowOutput out;
		if (m_ShadowMap == nullptr || m_Resolution == 0 || m_CascadeCount == 0)
			return out;

		Math::Vector3 sceneCenter;
		float sceneRadius = 5.0f;
		ComputeSceneSphere(models, sceneCenter, sceneRadius);

		// 按相机到场景的距离自适应 CSM 阴影远距，避免在角色特写镜头中把级联浪费到 1000 米空旷处
		const float camDist = static_cast<float>(Math::Length(camera.GetPosition() - sceneCenter));
		const float shadowFar = (std::min)(camera.GetFarClip(), (std::max)(camDist + sceneRadius * 3.0f, sceneRadius * 6.0f));
		const float nearClip  = (std::max)(camera.GetNearClip(), 0.05f);
		const std::array<float, 4> splits = ShadowMath::ComputeCsmSplits(nearClip, shadowFar, m_CascadeCount, 0.72f);

		const Math::Vector3 sunDir(params.SunDirection[0], params.SunDirection[1], params.SunDirection[2]);
		const Math::Matrix4 lightView = ShadowMath::BuildLightViewMatrixRH(sunDir, Math::Vector3(0.0f, 0.0f, 0.0f));

		// 场景包围球在光源空间的 Z 上下界，保证视锥外的遮挡物仍能投下阴影
		const Math::Vector4 sceneCenterLS = lightView * Math::Vector4(sceneCenter, 1.0f);
		const float sceneMinZ = static_cast<float>(sceneCenterLS.GetZ()) - sceneRadius * 2.0f;
		const float sceneMaxZ = static_cast<float>(sceneCenterLS.GetZ()) + sceneRadius * 3.0f;

		ctx.PIXBeginEvent(L"ShadowPass::CSM");

		float prevSplit = nearClip;
		for (uint32_t c = 0; c < m_CascadeCount; ++c)
		{
			const float curSplit = splits[c];

			Math::Vector3 cornersWS[8];
			ComputeFrustumSliceCornersWS(camera, prevSplit, curSplit, cornersWS);

			// 计算视锥切片的旋转不变最小包围球半径，相机旋转时阴影纹素大小保持恒定（无抖动）
			Math::Vector3 frustumCenter(0.0f, 0.0f, 0.0f);
			for (int i = 0; i < 8; ++i)
				frustumCenter = frustumCenter + cornersWS[i];
			frustumCenter = frustumCenter * (1.0f / 8.0f);

			float radius = 0.0f;
			for (int i = 0; i < 8; ++i)
				radius = (std::max)(radius, static_cast<float>(Math::Length(cornersWS[i] - frustumCenter)));
			// 将半径向上取整到 1/16 米，消除浮点微抖
			radius = std::ceil(radius * 16.0f) / 16.0f;
			radius = (std::max)(radius, 0.5f);

			const Math::Vector4 centerLS = lightView * Math::Vector4(frustumCenter, 1.0f);
			float cx = static_cast<float>(centerLS.GetX());
			float cy = static_cast<float>(centerLS.GetY());
			const float cz = static_cast<float>(centerLS.GetZ());

			const float worldUnitsPerTexel = (2.0f * radius) / static_cast<float>(m_Resolution);
			cx = std::floor(cx / worldUnitsPerTexel) * worldUnitsPerTexel;
			cy = std::floor(cy / worldUnitsPerTexel) * worldUnitsPerTexel;

			const float minZ = (std::min)(cz - radius * 2.0f, sceneMinZ);
			const float maxZ = (std::max)(cz + radius * 3.0f, sceneMaxZ);

			const Math::Matrix4 lightProj = ShadowMath::BuildLightOrthoMatrixRH(
				cx - radius, cx + radius, cy - radius, cy + radius, minZ, maxZ);
			const Math::Matrix4 lightViewProj = lightView * lightProj;

			wchar_t eventName[64] = {};
			swprintf_s(eventName, L"CSM_Cascade_%u", c);
			ctx.PIXBeginEvent(eventName);

			// [契约 3 子资源按需状态展开] 对每个级联切片独立切 DEPTH_WRITE 并清空绘制
			ctx.TransitionSubresource(*m_ShadowMap, c, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
			ctx.ClearDepthSlice(*m_ShadowMap, c);
			ctx.SetViewportAndScissor(0u, 0u, m_Resolution, m_Resolution);
			ctx.SetDepthStencilTarget(m_ShadowMap->GetSliceDSV(c));
			if (drawCascade)
				drawCascade(ctx, lightViewProj);

			ctx.PIXEndEvent();

			out.LightViewProj[c] = lightViewProj;
			out.SplitDepths[c]   = curSplit;
			prevSplit            = curSplit;
		}

		ctx.PIXEndEvent();

		const BindlessAllocation staged =
			services.GetDescriptorHeap().StageTransientDescriptor(m_ShadowMap->GetDepthSRV());

		out.ShadowMap            = m_ShadowMap.get();
		out.ShadowMapBindlessIdx = staged.HeapIndex;
		out.CascadeCount         = m_CascadeCount;
		out.Resolution           = m_Resolution;
		out.NormalBias           = params.NormalBias;
		out.DepthBias            = params.DepthBias;
		out.PCFRadius            = params.PCFRadius;
		out.Strength             = params.Strength;
		return out;
	}
}
