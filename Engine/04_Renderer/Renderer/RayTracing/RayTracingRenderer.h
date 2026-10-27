#pragma once

// ============================================================================
//  RayTracingRenderer.h —— R6 / D3：DXR 光追渲染器（硬阴影 + 反射）
// ----------------------------------------------------------------------------
//  设计定位（严格对齐路线图 §8 总纲）：
//    「DXR 要作为新增 Renderer 接入，而不是让现有光栅路径依赖光追。
//      光栅路径始终应可独立构建、运行和回退。」
//    → 本类只实现 BaseRenderer 的接口，不修改 Deferred / Forward 的任何行为。
//    → 通过 RendererRegistry 注册，由 RendererDemo 的 UI 切换。
//
//  与 Deferred 的边界：
//    * 输出缓冲：自己建一张 R8G8B8A8_UNORM 的 SceneColorBuffer，
//      与 Deferred 的 SceneColorBufferFormat 相同 → 走同一条
//      Display::PreparePresentSDR 统一 Present 路径（格式家族一致）。
//    * 显示编码：RayGen 里调用 ColorPipeline.hlsli 的 EncodeDisplayColor，
//      与光栅路径共用 tonemap + sRGB 传递函数。
//    * 灯光：方向光数据取自 RenderParams（与光栅路径同一真源），
//      语义也一致（SunDirection 是**传播方向**，着色时取反）。
//
//  能力演进：
//    R5 / D2 —— 最小闭环：主射线 → 面法线色。只是"证明 DXR 通了"。
//    R6 / D3 —— 本文件：主射线 → 硬阴影射线 + 单次反射射线（递归深度 2），
//               Lambert 着色、法线偏差、可开关、可调试通道、逐帧统计。
// ============================================================================

#include "04_Renderer/Renderer/Base/BaseRenderer.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "04_Renderer/Renderer/RayTracing/RaytracingScene.h"
#include "02_RHI/Pipeline/RaytracingPSO.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "05_Scene/Camera/Camera.h"
#include "00_Core/Math/VectorMath.h"

#include <memory>
#include <string>
#include <vector>
#include <d3d12.h>

namespace Renderer::RayTracing
{
	// ============================================================
	//  根常量（b0）：必须与 HLSL 的 cbuffer RaytracingConstants 逐字段一致
	//  R6 起共 48 个 DWORD = 192 字节（R5 是 32 个 DWORD = 128 字节）
	// ============================================================
	struct RaytracingConstants
	{
		Math::Matrix4 InvViewProj;    // 64 B
		Math::Vector4 CameraPosition; // 16 B
		Math::Vector4 OutputSize;     // 16 B：xy = 分辨率, zw = 1/分辨率
		Math::Vector4 DisplayParams;  // 16 B：x = exposure, y = tonemap, z = srgb
		Math::Vector4 DebugParams;    // 16 B：保留

		// ---- R6 / D3 追加（只能追加在末尾）----
		Math::Vector4 LightDirection; // 16 B：xyz = 光传播方向, w = 强度
		Math::Vector4 LightColor;     // 16 B：rgb = 颜色
		Math::Vector4 RTParams;       // 16 B：x = 阴影法线偏差, y = 阴影最大距离,
		                              //       z = 反射最大距离, w = 反射强度
		Math::Vector4 RTParams2;      // 16 B：x = 开阴影, y = 开反射, z = 调试通道
	};
	static_assert(sizeof(RaytracingConstants) == 192, "必须与 HLSL 的 48 个 DWORD 根常量一致");

	// 每帧的射线统计。数值是**发射数**（不是命中数）：
	//   主射线 = 输出像素数；阴影射线 = 主射线命中且开阴影的像素数；
	//   反射射线 = 主射线命中且开反射的像素数。
	// 想拿命中率必须靠调试通道，本阶段不做逐射线回读（那需要额外 UAV + 读回，
	// 属于性能分析工具的范畴，不在 D3 的"最小可比较"范围内）。
	struct RayTracingFrameStats
	{
		uint64_t PrimaryRays = 0;
		uint64_t ShadowRays = 0;
		uint64_t ReflectionRays = 0;
		uint64_t TotalRays = 0;
		uint32_t TriangleCount = 0;
		uint32_t BlasCount = 0;
		uint32_t InstanceCount = 0;
	};

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

		// 供 RendererDemoApp::BindRendererOutput 取输出缓冲（与 Deferred/Forward 同构）
		std::shared_ptr<ColorBuffer> GetSceneColorBuffer(void) const { return m_SceneColorBuffer; }

		// 供 UI 读统计（Render 之后每帧更新）
		const RayTracingFrameStats& GetFrameStats(void) const { return m_Stats; }

		RendererFeature GetFeatures() const override
		{
			// 只声明 Raytracing。不声明 Deferred/Shadow/SSAO 等 ——
			// 声明了就会让 UI 把这些开关当成"已接通"，而它们在本渲染器里
			// 根本没有对应的 GPU 命令（那正是 AGENTS.md 禁止的"空壳当已实现"）。
			return RendererFeature::Raytracing;
		}

		// DXR 不走光栅 PSO。这三个接口是 BaseRenderer 的纯虚契约，
		// 这里给出无副作用实现，绝不被调用（Render 里没有任何 Draw）。
		const GraphicsPSO& GetPSO(const PipelineDesc& desc) override { (void)desc; return m_DefaultPSO; }
		void BindRenderState(GraphicsContext& context) override { (void)context; }
		void BindMaterial(GraphicsContext& context, const Scene::Material::Material& material) override
		{
			(void)context; (void)material;
		}

	private:
		bool BuildRootSignature();
		bool BuildStateObject();
		bool CreateOutputTargets(uint32_t width, uint32_t height);
		void DestroyOutputTargets();
		void RefreshDescriptorHeap();

		// 判断"模型集合或几何数据"是否变化 —— 变了才重建 BLAS。
		bool GeometryChanged(const std::vector<Scene::Model::ModelInstance*>& models) const;

	private:
		RootSignature  m_GlobalRootSig;
		RaytracingPSO  m_StateObject;

		DescriptorHeap m_DescriptorHeap;   // 自有 shader-visible 堆（4 个描述符）

		std::shared_ptr<ColorBuffer> m_SceneColorBuffer;

		RaytracingScene m_Scene;

		// 几何变化检测用的签名（模型指针 + 数据缓冲版本号）
		std::vector<uint64_t> m_GeometrySignature;

		bool m_StateObjectReady = false;
		bool m_GeometryReady = false;

		// 统计（供 UI / 日志）
		uint32_t m_DispatchedFrames = 0;
		RayTracingFrameStats m_Stats;

		// 上一帧实际生效的开关。用于检测「用户刚拨动开关」并在首帧打日志，
		// 让「拨了没反应」这类问题在日志里立刻可见（而不是靠肉眼看画面差异）。
		bool m_LastEnableShadow = false;
		bool m_LastEnableReflection = false;
	};
}
