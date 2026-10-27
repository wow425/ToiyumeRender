#pragma once

// ============================================================================
//  RayTracingRenderer.h —— R5 / D2：DXR 最小闭环渲染器
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
//
//  本阶段（D2）能力：
//    只发一条主射线，命中返回世界空间面法线色，未命中返回背景色。
//    这是**诊断画面**，不是路径追踪器（D3 才加阴影/反射）。
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
	//  共 32 个 DWORD = 128 字节
	// ============================================================
	struct RaytracingConstants
	{
		Math::Matrix4 InvViewProj;    // 64 B
		Math::Vector4 CameraPosition; // 16 B
		Math::Vector4 OutputSize;     // 16 B：xy = 分辨率, zw = 1/分辨率
		Math::Vector4 DisplayParams;  // 16 B：x = exposure, y = tonemap, z = srgb
		Math::Vector4 DebugParams;    // 16 B：保留
	};
	static_assert(sizeof(RaytracingConstants) == 128, "必须与 HLSL 的 32 个 DWORD 根常量一致");

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
	};
}
