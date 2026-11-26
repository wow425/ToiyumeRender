// ============================================================================
// 模块：SimpleShadowMap（单级联方向光 Shadow Map · 实现）
// 架构层：Engine/04_Renderer/Passes（WP-3C）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/SimpleShadowMap.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "05_Scene/Camera/Camera.h"
#include "05_Scene/Model/Model.h"

#include <algorithm>
#include <cmath>

namespace Renderer
{
	namespace
	{
		void ComputeCombinedSceneBounds(const std::vector<Scene::Model::ModelInstance*>& models,
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
			const Vector3 halfExt = (maxPt - minPt) * 0.5f;
			outRadius = (std::max)(static_cast<float>(Length(halfExt)), 1.0f);
		}
	}

	AlgorithmCapsReport SimpleShadowMap::CheckSupport(const Graphics::GpuCaps& caps, const ShadowParams& params) const
	{
		(void)caps;
		AlgorithmCapsReport report;
		const uint32_t res = (std::clamp)(params.Resolution, 256u, 4096u);
		report.Supported = true;
		report.EstimatedVramBytes = static_cast<uint64_t>(res) * static_cast<uint64_t>(res) * 4ull; // D32_FLOAT = 4B/px
		return report;
	}

	bool SimpleShadowMap::Initialize(RenderServices& services, const ShadowParams& params, std::string* outErr)
	{
		(void)services;
		const uint32_t res = (std::clamp)(params.Resolution, 256u, 4096u);
		auto buf = std::make_shared<DepthBuffer>(1.0f, 0);
		buf->CreateArray(L"ShadowMap Simple (1-Slice D32)", res, res, 1u, Renderer::Deferred::ShadowMapBufferFormat);
		if (buf->GetResource() == nullptr)
		{
			if (outErr) *outErr = "SimpleShadowMap: failed to create DepthBuffer array";
			return false;
		}
		m_ShadowMap = std::move(buf);
		m_Resolution = res;
		return true;
	}

	void SimpleShadowMap::Shutdown(Graphics::DeferredReleaseQueue& releaseQueue)
	{
		// [DX12 时钟 1+2 安全铁律] 旧阴影图 COM 与 CPU DSV/SRV 描述符打包延后至帧尾 Fence 完成后释放，
		// 彻底防止热切换 Shadow 分辨率或算法时在途命令列表悬空引用。
		if (m_ShadowMap != nullptr)
		{
			std::shared_ptr<DepthBuffer> retired = std::move(m_ShadowMap);
			releaseQueue.EnqueueCallback([retired = std::move(retired)]() mutable {
				retired.reset();
			});
		}
		m_Resolution = 0;
	}

	ShadowOutput SimpleShadowMap::Render(GraphicsContext& ctx,
		const Scene::Camera::Camera& camera,
		const std::vector<Scene::Model::ModelInstance*>& models,
		const ShadowParams& params,
		RenderServices& services,
		const ShadowDrawCascadeFn& drawCascade)
	{
		(void)camera;
		ShadowOutput out;
		if (m_ShadowMap == nullptr || m_Resolution == 0)
			return out;

		Math::Vector3 sceneCenter;
		float sceneRadius = 5.0f;
		ComputeCombinedSceneBounds(models, sceneCenter, sceneRadius);

		// 留 15% 边界余量，防止模型贴近视锥边缘被裁剪
		const float extent = sceneRadius * 1.15f;
		const Math::Vector3 sunDir(params.SunDirection[0], params.SunDirection[1], params.SunDirection[2]);
		const Math::Matrix4 lightView = ShadowMath::BuildLightViewMatrixRH(sunDir, Math::Vector3(0.0f, 0.0f, 0.0f));

		// 将场景球心变换到光源视空间并按单纹素步长对齐，消除相机/场景微动带来的阴影游泳
		const Math::Vector4 centerLS = lightView * Math::Vector4(sceneCenter, 1.0f);
		float cx = static_cast<float>(centerLS.GetX());
		float cy = static_cast<float>(centerLS.GetY());
		const float cz = static_cast<float>(centerLS.GetZ());

		const float worldUnitsPerTexel = (2.0f * extent) / static_cast<float>(m_Resolution);
		cx = std::floor(cx / worldUnitsPerTexel) * worldUnitsPerTexel;
		cy = std::floor(cy / worldUnitsPerTexel) * worldUnitsPerTexel;

		// 深度范围向光源方向拉长（+2*extent），确保阴影投射体即便在球心后方也不被近平面裁掉
		const float minZ = cz - extent * 2.0f;
		const float maxZ = cz + extent * 3.0f;
		const Math::Matrix4 lightProj = ShadowMath::BuildLightOrthoMatrixRH(
			cx - extent, cx + extent, cy - extent, cy + extent, minZ, maxZ);
		const Math::Matrix4 lightViewProj = lightView * lightProj;

		ctx.PIXBeginEvent(L"ShadowPass::SimpleShadowMap");
		ctx.TransitionSubresource(*m_ShadowMap, 0u, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		ctx.ClearDepthSlice(*m_ShadowMap, 0u);
		ctx.SetViewportAndScissor(0u, 0u, m_Resolution, m_Resolution);
		ctx.SetDepthStencilTarget(m_ShadowMap->GetSliceDSV(0u));
		if (drawCascade)
			drawCascade(ctx, lightViewProj);
		ctx.PIXEndEvent();

		// 经统一单可见堆 Zone B 暂存 Texture2DArray SRV，供下游 SM 6.6 Bindless LightingPass 直索引
		const BindlessAllocation staged =
			services.GetDescriptorHeap().StageTransientDescriptor(m_ShadowMap->GetDepthSRV());

		out.ShadowMap            = m_ShadowMap.get();
		out.ShadowMapBindlessIdx = staged.HeapIndex;
		out.LightViewProj[0]     = lightViewProj;
		out.SplitDepths[0]       = 1000.0f;
		out.CascadeCount         = 1u;
		out.Resolution           = m_Resolution;
		out.NormalBias           = params.NormalBias;
		out.DepthBias            = params.DepthBias;
		out.PCFRadius            = params.PCFRadius;
		out.Strength             = params.Strength;
		return out;
	}
}
