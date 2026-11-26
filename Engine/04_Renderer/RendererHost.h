// ============================================================================
// 模块：RendererHost（渲染管线宿主与帧边界先备后切控制器）
// 架构层：Engine/04_Renderer（契约 2 / WP-2B）
// 核心职责：统一托管实例级 RenderServices 与当前活跃 IPipeline（BaseRenderer），
//           提供原子「先建新、初始化成功再帧边界替换、失败 100% 保留旧管线」的切换协议，
//           并对外直接暴露统一输出 ColorBuffer 与可选统计查询，彻底消除 Demo 层多路 dynamic_cast。
// 所有权反序：ShutdownPipeline() 必须在场景模型释放前/后按契约 2 反序关闭，
//           ShutdownServices() 必须在所有 Model/TextureRef 释放之后调用。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/Base/BaseRenderer.h"

class ColorBuffer;
class GraphicsContext;

namespace Renderer
{
	class RendererHost
	{
	public:
		RendererHost() = default;
		~RendererHost() = default;

		RendererHost(const RendererHost&) = delete;
		RendererHost& operator=(const RendererHost&) = delete;

		bool InitializeServices(bool enableShaderHotReload = true, const std::wstring& repoRoot = L"");
		// 先销毁活跃管线（保留 RenderServices 供外层继续释放模型），再按需关闭 RenderServices
		void ShutdownPipeline();
		void ShutdownServices();

		// 契约 2 先备后切协议：候选管线 Initialize 失败时丢弃候选、保留旧管线与 UI 零损伤。
		bool SwitchPipeline(const std::wstring& name, uint32_t width, uint32_t height, bool simulateFailure = false);

		void OnResize(uint32_t width, uint32_t height);
		void BeginFrame(uint64_t frameIndex, const RenderFrameDesc& frame);
		void Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext);
		void Render(GraphicsContext& context, const RenderFrameDesc& frame, DrawPass pass = kOpaque);
		void EndFrame(GraphicsContext& context, const RenderFrameDesc& frame);

		IPipeline* GetActivePipeline() const { return m_ActivePipeline.get(); }
		const std::wstring& GetActivePipelineName() const { return m_ActiveName; }
		const std::string& GetLastSwitchError() const { return m_LastSwitchError; }
		std::shared_ptr<ColorBuffer> GetOutputColorBuffer() const;

		RenderServices& GetServices() { return m_Services; }
		const RenderServices& GetServices() const { return m_Services; }

		// [WP-3B / 契约 7.2] 帧边界单线程按键 Shader 热重载入口（F5 / Shift+F5 / UI 按钮）
		Graphics::ShaderReloadReport TriggerShaderHotReload(bool forceAll = false);
		bool IsShaderHotReloadEnabled() const { return m_HotReloader.IsEnabled(); }
		const Graphics::ShaderReloadReport& GetLastReloadReport() const { return m_LastReloadReport; }
		const Graphics::ShaderHotReloader& GetHotReloader() const { return m_HotReloader; }

	private:
		RenderServices m_Services;
		RendererPtr    m_ActivePipeline;
		std::wstring   m_ActiveName;
		std::string                   m_LastSwitchError;
		Graphics::ShaderHotReloader   m_HotReloader;
		Graphics::ShaderReloadReport  m_LastReloadReport;
	};
}
