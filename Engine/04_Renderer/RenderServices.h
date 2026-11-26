#pragma once

// ============================================================================
// RenderServices —— 契约 2 / TD-01c：实例级渲染基础设施服务容器
// ----------------------------------------------------------------------------
// 为什么必须引入实例级 RenderServices：
//   旧架构把全局堆（s_TextureHeap/s_SamplerHeap）、纹理缓存（TextureManager）、
//   光照缓冲（LightingSystem）的生命周期散落在各个具体 Renderer::Initialize/Shutdown 中。
//   结果：每次在 Deferred / Forward / RayTracing 间热切换管线，旧 Renderer::Shutdown()
//   都会把全局堆与 TextureManager 连根拔起，迫使所有已加载模型被迫卸载并重新从磁盘解析、
//   且旧管线失败时无法无损保底（TD-01b / TD-01c / TD-08a）。
//
// 职责与所有权（严格反序 Shutdown 契约）：
//   - 生命周期长于单个 Renderer 实例（与宿主应用 / RendererHost 同寿）。
//   - 持有 4 分区单 Shader-Visible 堆 UnifiedDescriptorHeap，以及帧尾延迟释放队列引用。
//   - 统一托管 TextureManager 与 LightingSystem 的启动/关闭顺序：
//       Startup : UnifiedDescriptorHeap -> TextureManager -> LightingSystem
//       Shutdown: (宿主须先关闭 UI、Renderer 与全部场景 Model/TextureRef)
//                 -> LightingSystem -> TextureManager -> UnifiedDescriptorHeap
//   - 提供 ActiveServices 访问器，使尚未改造的资产加载入口（ModelLoader）与独立旧 Demo
//     （DeferredApp / ForwardApp / RayTracingApp）均能无缝命中同一套 4 分区可见堆。
// ============================================================================

#include <cstdint>

#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

namespace Graphics
{
	class DeferredReleaseQueue;
}

namespace Renderer
{
	class RenderServices
	{
	public:
		RenderServices() = default;
		~RenderServices() { Shutdown(); }

		RenderServices(const RenderServices&) = delete;
		RenderServices& operator=(const RenderServices&) = delete;

		// 初始化 4 分区可见堆、TextureManager 与 LightingSystem。
		// 若 releaseQueue 为 nullptr，则默认挂接全局 Graphics::GetDeferredReleaseQueue()。
		bool Initialize(Graphics::DeferredReleaseQueue* releaseQueue = nullptr);

		// 严格反序关闭：LightingSystem -> TextureManager -> UnifiedDescriptorHeap。
		// 调用前置契约：宿主已执行 IdleGPU()，并已销毁 ImGuiLayer、全部 Renderer 与全部场景 Model。
		void Shutdown();

		bool IsInitialized() const { return m_Initialized; }

		// 每帧开头推进 Zone B 环形槽区（同帧重复调用幂等，兼容宿主与独立 Demo 双路径）。
		void BeginFrame(uint64_t frameIndex);

		UnifiedDescriptorHeap& GetDescriptorHeap() { return m_DescriptorHeap; }
		const UnifiedDescriptorHeap& GetDescriptorHeap() const { return m_DescriptorHeap; }

		// [WP-3A / 契约 7.1] 全局唯一共享 SM 6.6 Bindless 根签名（20 / 64 DWORD）：
		//   RootParam[0] = b0 (16 DWORD 根常量：Bindless 资源下标 + 小标量)
		//   RootParam[1] = b1 (2 DWORD 动态 CBV：Per-Frame / Per-View)
		//   RootParam[2] = b2 (2 DWORD 动态 CBV：Per-Pass)
		//   StaticSamplers s0..s6 : PointClamp / LinearClamp / LinearWrap / AnisoWrap / ShadowCmp(LE) / PointWrap / ShadowCmpGreater(GE)
		bool IsBindlessSM66Ready() const { return m_BindlessSM66Ready; }
		const RootSignature& GetBindlessGraphicsRootSignature() const { return m_BindlessGraphicsRS; }
		const RootSignature& GetBindlessComputeRootSignature() const { return m_BindlessComputeRS; }

		Graphics::DeferredReleaseQueue& GetReleaseQueue();

		// 当前活跃服务实例（供 ModelLoader 等跨层加载入口获取当前 4 分区堆）。
		static RenderServices* GetActiveServices();

	private:
		void BuildBindlessRootSignatures();

	private:
		UnifiedDescriptorHeap           m_DescriptorHeap;
		RootSignature                   m_BindlessGraphicsRS;
		RootSignature                   m_BindlessComputeRS;
		bool                            m_BindlessSM66Ready = false;
		Graphics::DeferredReleaseQueue* m_ReleaseQueue = nullptr;
		uint64_t                        m_LastBeganFrame = 0;
		bool                            m_HasBeganFrame = false;
		bool                            m_Initialized = false;

		static RenderServices*          s_ActiveServices;
	};
}
