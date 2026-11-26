// ============================================================================
// 模块：LinearPassList（三阶段轻量线性 Pass 调度器 + TransientRTMap）
// 架构层：Engine/04_Renderer（契约 3 / WP-3A）
// 设计裁决（LADR-05）：
//   针对个人研发规模（~30K LOC），坚决拒绝重型工业 DAG 拓扑排序与 Placed Resource 别名；
//   仅用 ~280 行实现严格三阶段确定性线性调度：
//     阶段 1 Declare             : AddPass / AddNativePass 显式登记 Pass 顺序与资源访问意图；
//     阶段 2 Validate/Lifetime   : 悬空读阻断校验（瞬态资源首次访问不可为纯读）+ O(N) [First, Last] 扫描；
//     阶段 3 Execute             : PassAccess -> DX12 Transition/UAVBarrier 自动翻译、按需子资源状态展开、
//                                  AddNativePass 逃逸护栏（清状态缓存/保单堆）、PIX + 多区间 GpuTimer 自动包络。
// DX12 不变量：
//   - 启用 SM 6.6 Bindless 后，Shader 可直接按下标访问任意描述符，但仍必须在此完整声明 ResourceAccess！
//   - AccelerationStructure 永不发常规 Transition 屏障，仅在前序写/构建后插 InsertUAVBarrier。
//   - 只读深度测试（SkyboxPass 等）必须经 BindReadOnlyDepth 绑定 GetDSV_DepthReadOnly()，杜绝写冲突。
// ============================================================================
#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ColorBuffer;
class DepthBuffer;
class GpuResource;
class GraphicsContext;

namespace Renderer
{
	class GpuTimer;
	class RenderServices;

	// 访问语义枚举（语义对齐 DX12 Enhanced Barriers，便于未来按需演进）
	enum class PassAccess : uint8_t
	{
		ReadSRV_Pixel = 0,
		ReadSRV_NonPixel,
		ReadSRV_All,
		ReadDepthOnly,
		ReadDepthAndSRV,
		WriteRTV,
		WriteDepth,
		WriteUAV,
		ReadWriteUAV,
		CopySrc,
		CopyDst,
		AccelerationStructure,
	};

	struct ResourceAccess
	{
		GpuResource* Res         = nullptr;
		PassAccess   Access      = PassAccess::ReadSRV_Pixel;
		uint32_t     Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		// false 表示帧内瞬态资源：阶段 2 若发现首次出现即为纯读（无前序写入），立即阻断报错！
		bool         IsExternal  = true;
	};

	// O(N) 生命周期扫描记录：当前驱动 TransientRTMap 缓存；未来显存吃紧时可直接驱动 Placed Aliasing
	struct PassResourceLifetime
	{
		GpuResource* Resource     = nullptr;
		uint32_t     FirstPassIdx = 0;
		uint32_t     LastPassIdx  = 0;
		bool         FirstIsRead  = false;
		bool         IsExternal   = true;
	};

	// 纯辅助函数（与 D3D12 设备解耦，支持 CPU CTest 直接单测）
	D3D12_RESOURCE_STATES TranslatePassAccess(PassAccess access);
	bool IsReadAccess(PassAccess access);
	bool IsWriteAccess(PassAccess access);

	// 契约 3 强制只读深度绑定辅助：固定走 depth.GetDSV_DepthReadOnly()，彻底避免 TD-04c 类错误
	void BindReadOnlyDepth(GraphicsContext& ctx, ColorBuffer* rtv, DepthBuffer& depth);

	// 瞬态 RT 描述键：严格包含 ClearRGBA[4]，匹配 D3D12_CLEAR_VALUE，杜绝 Debug Layer #820 警告
	struct TransientRTDesc
	{
		uint32_t              Width     = 0;
		uint32_t              Height    = 0;
		uint32_t              MipCount  = 1;
		DXGI_FORMAT           Format    = DXGI_FORMAT_UNKNOWN;
		std::array<float, 4>  ClearRGBA = { 0.0f, 0.0f, 0.0f, 0.0f };

		bool operator==(const TransientRTDesc& rhs) const;
	};

	// 轻量瞬态 RT 缓存池（60 帧未用自动淘汰；不做 Placed Aliasing）
	class TransientRTMap
	{
	public:
		TransientRTMap();
		~TransientRTMap();

		TransientRTMap(const TransientRTMap&) = delete;
		TransientRTMap& operator=(const TransientRTMap&) = delete;

		// 每帧开头推进帧计数并淘汰超过 kEvictFrames 未使用的条目
		void BeginFrame(uint64_t frameIndex);
		ColorBuffer* Acquire(const std::wstring& debugName, const TransientRTDesc& desc);
		void Clear();
		size_t GetCachedCount() const { return m_Entries.size(); }

		static constexpr uint64_t kEvictFrames = 60;

	private:
		struct Entry
		{
			Entry();
			~Entry();
			Entry(Entry&&) noexcept;
			Entry& operator=(Entry&&) noexcept;

			TransientRTDesc              Desc;
			std::unique_ptr<ColorBuffer> Buffer;
			uint64_t                     LastUsedFrame = 0;
			bool                         InUseThisFrame = false;
		};
		uint64_t           m_CurrentFrame = 0;
		std::vector<Entry> m_Entries;
	};

	class LinearPassList
	{
	public:
		using PassCallback = std::function<void(GraphicsContext&)>;

		LinearPassList() = default;
		~LinearPassList() = default;

		// ---- 阶段 1：Declare ----
		void Reset();

		// 显式声明某资源为帧内瞬态（用于悬空读校验）
		void MarkTransient(GpuResource* res);

		// 注册标准 Pass（执行前自动下发 ResourceAccess 屏障并包络 PIX + GpuTimer）
		void AddPass(std::string name, std::vector<ResourceAccess> accesses, PassCallback fn);

		// 注册 Native Escape Pass（专治 DXR / DirectML / ImGui）：
		//   进入前自动 FlushResourceBarriers() + 确保 4 分区单可见堆已绑定；
		//   退出后自动 InvalidateCachedPipelineState()，防 DXR SetPipelineState1 污染后续光栅缓存。
		void AddNativePass(std::string name, std::vector<ResourceAccess> accesses, PassCallback fn);

		// ---- 阶段 2：Validate & LifetimeScan ----
		// 返回 true 表示校验通过；若发现未初始化瞬态资源悬空读，返回 false 并填写 outError。
		bool ValidateAndScanLifetimes(std::string* outError = nullptr);

		// ---- 阶段 3：Execute ----
		// 若尚未 Validate，内部先执行阶段 2；随后按线性登记顺序逐 Pass 翻译屏障并执行。
		void Execute(GraphicsContext& ctx, RenderServices* services = nullptr, GpuTimer* timer = nullptr);

		const std::vector<PassResourceLifetime>& GetLifetimes() const { return m_Lifetimes; }
		uint32_t GetPassCount() const { return static_cast<uint32_t>(m_Passes.size()); }
		const std::string& GetLastValidationError() const { return m_LastValidationError; }

	private:
		struct PassNode
		{
			std::string                 Name;
			std::vector<ResourceAccess> Accesses;
			PassCallback                Callback;
			bool                        IsNative = false;
		};

		bool IsMarkedTransient(GpuResource* res) const;

		std::vector<PassNode>             m_Passes;
		std::vector<GpuResource*>         m_TransientResources;
		std::vector<PassResourceLifetime> m_Lifetimes;
		std::string                       m_LastValidationError;
		bool                              m_Validated = false;
	};
}
