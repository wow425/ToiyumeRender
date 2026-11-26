// ============================================================================
// 模块：GraphicsCaps（硬件能力快照 GpuCaps + 本地显存预算守卫 VramBudgetGuard）
// 架构层：Engine/02_RHI（契约 2 / WP-3A / TD-11c）
// 核心职责：
//   1) 在 Graphics::Initialize 期间一次性探测 FeatureLevel / MaxShaderModel /
//      RaytracingTier / ResourceBindingTier / SupportsBindlessSM66 / WaveOps /
//      Native16BitOps / TypedUAVLoad / DedicatedVRAM 等只读能力快照；
//   2) 经 IDXGIAdapter3::QueryVideoMemoryInfo(LOCAL) 提供实时显存占用与预算查询；
//   3) 在管线或神经算法热切换双驻留之前做显存预检，超限时明确拒绝并说明原因，
//      严禁静默降分辨率或冒名回退。
// DX12 不变量：
//   - CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL) 总是返回 S_OK，真实结果在出参
//     HighestShaderModel 中；SM 6.6 Bindless 必须同时满足 HighestShaderModel >= 6_6
//     且 ResourceBindingTier >= TIER_3。
//   - 内部持有的 IDXGIAdapter3 必须在 Graphics::Shutdown 中于 ReportLiveDeviceObjects
//     之前显式释放（ShutdownGpuCaps），防止残留 DXGI/设备引用。
// ============================================================================
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

namespace Graphics
{
	struct GpuCaps
	{
		D3D_FEATURE_LEVEL           FeatureLevel          = D3D_FEATURE_LEVEL_12_1;
		D3D_SHADER_MODEL            MaxShaderModel        = D3D_SHADER_MODEL_6_4;
		D3D12_RAYTRACING_TIER       RaytracingTier        = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		D3D12_RESOURCE_BINDING_TIER BindingTier           = D3D12_RESOURCE_BINDING_TIER_1;
		bool                        SupportsBindlessSM66  = false; // MaxShaderModel >= 6_6 && BindingTier >= TIER_3
		bool                        WaveOps               = false; // D3D12_OPTIONS1::WaveOps
		bool                        Native16BitOps        = false; // D3D12_OPTIONS4::Native16BitShaderOpsSupported
		bool                        TypedUAVLoadFP16      = false;
		bool                        TypedUAVLoadR11G11B10 = false;
		uint64_t                    DedicatedVRAMBytes    = 0;
		std::wstring                AdapterName;
	};

	struct VramUsageInfo
	{
		uint64_t CurrentUsageBytes = 0;
		uint64_t BudgetBytes       = 0;
	};

	// 由 Graphics::Initialize 在设备与 TypedUAVLoad 探测之后调用。
	void ProbeGpuCaps(ID3D12Device* device, IDXGIAdapter1* adapter,
		bool typedUavFp16, bool typedUavR11G11B10);

	// 由 Graphics::Shutdown 在 ReportLiveDeviceObjects 之前调用，释放内部 IDXGIAdapter3。
	void ShutdownGpuCaps();

	const GpuCaps& GetGpuCaps();

	// 经 IDXGIAdapter3::QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL) 查询实时显存占用/预算。
	VramUsageInfo QueryVramUsage();

	// 纯 CPU 预算评估函数（供 CheckVramBudgetForAllocation 与独立 CPU CTest 直接单测）：
	// 当 BudgetBytes > 0 且 CurrentUsageBytes + additionalBytes > BudgetBytes * safetyRatio 时返回 false。
	bool EvaluateVramBudget(const VramUsageInfo& usage, uint64_t additionalBytes,
		float safetyRatio, std::string* outReason);

	// 热切换双驻留前显存预检：超预算返回 false 并填写可读原因，严禁静默降画质或静默降分辨率！
	bool CheckVramBudgetForAllocation(uint64_t additionalBytes,
		float safetyRatio = 0.90f, std::string* outReason = nullptr);

	// 自动化回归测试钩子（CLI -SimulateNoDXR）：在支持 DXR 的机器上模拟无 DXR 硬件，
	// 验证 CheckSupport 优雅拒绝、UI 禁用与 CLI 批跑非零退出码。
	void SetSimulatedNoDxrForTest(bool disableDxr);
	bool IsSimulatedNoDxrForTest();
}
