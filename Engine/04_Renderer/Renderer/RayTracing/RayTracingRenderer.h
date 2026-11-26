#pragma once
// ============================================================================
// 模块：RayTracingRenderer（DXR 光追管线薄编排器 · WP-2B）
// 架构层：Engine/04_Renderer/Renderer/RayTracing
// 核心职责：持有 DXR 输出缓冲、AccelerationStructureManager 与 RayTracingMainPass，
//           并通过 IPipeline::GetOutputColorBuffer / QueryRayTracingStats 向宿主提供无 RTTI 接口。
// ============================================================================

#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "04_Renderer/AccelerationStructureManager.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/LinearPassList.h"
#include "04_Renderer/Passes/PrimaryRayPass.h"
#include "04_Renderer/Passes/RayTracingMainPass.h"
#include "04_Renderer/Passes/TonemapPass.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "00_Core/Math/VectorMath.h"

#include <memory>
#include <string>

namespace Renderer::RayTracing
{
	// 52 DWORD = 208 字节常量布局（WP-2C 将迁至 2 DWORD 动态 CBV）
	struct RaytracingConstants
	{
		Math::Matrix4 InvViewProj;    // offset   0 (64 B)
		Math::Vector4 CameraPosition; // offset  64 (16 B)
		Math::Vector4 OutputSize;     // offset  80 (16 B)
		Math::Vector4 DisplayParams;  // offset  96 (16 B)
		Math::Vector4 DebugParams;    // offset 112 (16 B)
		Math::Vector4 LightDirection; // offset 128 (16 B)
		Math::Vector4 LightColor;     // offset 144 (16 B)
		Math::Vector4 RTParams;       // offset 160 (16 B)
		Math::Vector4 RTParams2;      // offset 176 (16 B)
		Math::Vector4 RTParams3;      // offset 192 (16 B)
	};
	static_assert(sizeof(RaytracingConstants) == 208, "必须与 HLSL 的 52 个 DWORD 根常量一致");

	// 兼容旧代码命名空间别名
	using RayTracingFrameStats = ::Renderer::RayTracingFrameStats;

	class RayTracingRenderer final : public BaseRenderer
	{
	public:
		std::wstring GetName() const override { return L"RayTracingRenderer"; }

		bool Initialize(const RendererCreateDesc& desc) override;
		void Shutdown() override;
		void OnResize(uint32_t width, uint32_t height) override;

		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext) override;
		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass,
			BatchType batchType = kDefault) override;
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame) override;

		std::shared_ptr<ColorBuffer> GetSceneColorBuffer(void) const { return m_SceneColorBuffer; }
		std::shared_ptr<ColorBuffer> GetOutputColorBuffer() const override { return m_SceneColorBuffer; }

		const RayTracingFrameStats& GetFrameStats(void) const { return m_Stats; }
		bool QueryRayTracingStats(::Renderer::RayTracingFrameStats& outStats) const override
		{
			outStats = m_Stats;
			return true;
		}
		const PrimarySurfaceDesc* GetPrimarySurface() const override
		{
			return m_Blackboard.PrimarySurface.IsComplete() ? &m_Blackboard.PrimarySurface : nullptr;
		}

		// [WP-2C] DXR 接入统一共享 TonemapPass 后诚实声明 PostProcess，使 UI Exposure/ACES/sRGB 生效
		AlgorithmCapsReport CheckSupport(const Graphics::GpuCaps& caps, uint32_t width, uint32_t height) const override;
		void RegisterHotReloadShaders(Graphics::ShaderHotReloader& reloader, RenderServices& services) override;

		RendererFeature GetFeatures() const override
		{
			return RendererFeature::Raytracing | RendererFeature::PostProcess;
		}

	private:
		bool CreateOutputTargets(uint32_t width, uint32_t height);
		void DestroyOutputTargets();

	private:
		AccelerationStructureManager m_ASManager;
		PrimaryRayPass               m_PrimaryRayPass;
		RayTracingMainPass           m_MainPass;
		TonemapPass                  m_TonemapPass;
		FrameBlackboard              m_Blackboard;
		LinearPassList               m_PassList;
		// [契约 4] DXR 统一 PrimarySurface 缓冲（与光栅 GBuffer + D32 SRV 格式语义严格同构）
		std::shared_ptr<ColorBuffer> m_PrimaryDepthBuffer;    // R32_FLOAT (NDC z ∈ [0,1], 1.0=miss)
		std::shared_ptr<ColorBuffer> m_PrimaryAlbedoBuffer;   // R8G8B8A8_UNORM (RGB=BaseColor, A=AO)
		std::shared_ptr<ColorBuffer> m_PrimaryNormalBuffer;   // R16G16_FLOAT (RG=OctNormal)
		std::shared_ptr<ColorBuffer> m_PrimaryMaterialBuffer; // R16G16B16A16_FLOAT (M/R/Spec/MatID)
		std::shared_ptr<ColorBuffer> m_PrimaryEmissionBuffer; // R8G8B8A8_UNORM (RGB=Emissive)
		std::shared_ptr<ColorBuffer> m_LinearHDRBuffer;       // [WP-2C] R16G16B16A16_FLOAT UAV 目标
		std::shared_ptr<ColorBuffer> m_SceneColorBuffer;      // R8G8B8A8_UNORM Tonemap 后 LDR 目标
		RayTracingFrameStats         m_Stats;
	};
}
