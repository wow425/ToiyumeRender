// ============================================================================
// 模块：ShaderHotReloader（开发期单线程按键 Shader 热重载器 · F5 / Shift+F5）
// 架构层：Engine/02_RHI/Pipeline（契约 7.2 / WP-3B）
// 设计定位：
//   - 零后台文件监听线程、零跨线程竞态、零 Release 性能负担；
//   - 启动仍 100% 消费构建期内嵌 .h 字节码（秒开、零磁盘 IO）；
//   - 仅在开发交互期按 F5（增量重编 mtime 变更的 .hlsl 及其递归 #include .hlsli）
//     或 Shift+F5（全量重编）时，于 RendererHost 帧边界单线程同步调用 dxcompiler.dll
//     在内存中编译 DXIL，再由 Pass 回调事务化重建候选 PSO 并将旧 PSO 压入 DeferredReleaseQueue。
//   - 传入 -Experiment / -Screenshot 或定义 TOOIYUME_SHIPPING 时自动 No-Op 禁用。
// 安全铁律：
//   1) DXC 编译语法/语义报错时记录完整诊断并返回 LastError 供 ImGui 顶部红条展示，
//      绝不调用 RebuildPipeline，原 PSO 纹丝不动继续渲染；
//   2) 候选 PSO 创建失败时同样回滚保留旧 PSO；
//   3) 替换成功的旧 ID3D12PipelineState 必须进入 DeferredReleaseQueue，严禁当场析构在途 PSO！
// ============================================================================
#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Graphics
{
	struct ShaderHotReloadDesc
	{
		std::string               Id;            // 唯一标识，如 "TonemapBindlessPS" / "LightingPS"
		std::wstring              SourceRelPath; // 相对仓库根路径，如 L"Engine/Shader/04_PostProcess/TonemapBindlessPS.hlsl"
		std::wstring              EntryPoint;    // 如 L"MainPS"；DXR lib_6_6 传空串
		std::wstring              TargetProfile; // 如 L"ps_6_6" / "ps_6_4" / "cs_6_6" / "lib_6_6"
		std::vector<std::wstring> Defines;

		// 仅当 DXC 内存编译成功时触发；回调内事务化创建候选 PSO，成功替换并将旧 PSO 压入 DeferredReleaseQueue
		std::function<bool(const std::vector<uint8_t>& dxil, std::string& outErr)> RebuildPipeline;
	};

	struct ShaderReloadReport
	{
		uint32_t    Checked   = 0;
		uint32_t    Skipped   = 0;
		uint32_t    Succeeded = 0;
		uint32_t    Failed    = 0;
		std::string LastError;
		std::string Summary;
	};

	class ShaderHotReloader
	{
	public:
		ShaderHotReloader() = default;
		~ShaderHotReloader() { Shutdown(); }

		ShaderHotReloader(const ShaderHotReloader&) = delete;
		ShaderHotReloader& operator=(const ShaderHotReloader&) = delete;

		// enabled=false（如 -Experiment / -Screenshot / TOOIYUME_SHIPPING）时保持安全 No-Op
		bool Initialize(const std::wstring& repoRootDir, bool enabled);
		void Shutdown();

		bool IsEnabled() const { return m_Enabled && m_DxcReady; }
		void ClearRegistrations();
		void Register(ShaderHotReloadDesc desc);

		// 帧边界调用：forceAll=false (F5) 仅重编源码或其 #include .hlsli mtime 变更的条目；
		// forceAll=true (Shift+F5) 强制全量重编所有已注册条目。
		ShaderReloadReport ReloadModified(bool forceAll = false);

		const std::wstring& GetRepoRootDir() const { return m_RepoRootDir; }
		size_t GetRegisteredCount() const { return m_Entries.size(); }

	private:
		struct RegisteredEntry
		{
			ShaderHotReloadDesc Desc;
			std::wstring        FullPath;
			uint64_t            LastWriteStamp = 0;
		};

		bool CompileInMemory(const RegisteredEntry& entry, std::vector<uint8_t>& outDxil, std::string& outErr);
		uint64_t ComputeRecursiveWriteStamp(const std::wstring& fullFilePath) const;

		std::wstring                 m_RepoRootDir;
		std::wstring                 m_ShaderIncludeRoot;
		bool                         m_Enabled = false;
		bool                         m_DxcReady = false;
		HMODULE                      m_DxcModule = nullptr;
		void*                        m_DxcCreateInstanceFn = nullptr; // DxcCreateInstanceProc
		std::vector<RegisteredEntry> m_Entries;
	};
}
