// ============================================================================
// 模块：GraphicsCaps（硬件能力快照与显存预算守卫 · 实现）
// 架构层：Engine/02_RHI（契约 2 / WP-3A / TD-11c）
// ============================================================================
#include "00_Core/PCH.h"
#include "02_RHI/GraphicsCaps.h"

#include "00_Core/Utility/Utility.h"

#include <wrl/client.h>
#include <cstdio>

namespace Graphics
{
	namespace
	{
		GpuCaps                              s_GpuCaps;
		Microsoft::WRL::ComPtr<IDXGIAdapter3> s_Adapter3;
		bool                                 s_SimulateNoDxr = false;
	}

	void ProbeGpuCaps(ID3D12Device* device, IDXGIAdapter1* adapter,
		bool typedUavFp16, bool typedUavR11G11B10)
	{
		s_GpuCaps = GpuCaps{};
		s_Adapter3.Reset();

		if (adapter != nullptr)
		{
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(adapter->GetDesc1(&desc)))
			{
				s_GpuCaps.DedicatedVRAMBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
				s_GpuCaps.AdapterName        = desc.Description;
			}
			// 保存 IDXGIAdapter3 供运行期 QueryVramUsage() 使用（ShutdownGpuCaps 统一释放）
			adapter->QueryInterface(TY_IID_PPV_ARGS(&s_Adapter3));
		}

		s_GpuCaps.TypedUAVLoadFP16      = typedUavFp16;
		s_GpuCaps.TypedUAVLoadR11G11B10 = typedUavR11G11B10;

		if (device == nullptr)
			return;

		// 1) Feature Level 上限
		static constexpr D3D_FEATURE_LEVEL kLevels[] = {
			D3D_FEATURE_LEVEL_12_2,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};
		D3D12_FEATURE_DATA_FEATURE_LEVELS flData = {};
		flData.NumFeatureLevels = _countof(kLevels);
		flData.pFeatureLevelsRequested = kLevels;
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &flData, sizeof(flData))))
		{
			s_GpuCaps.FeatureLevel = flData.MaxSupportedFeatureLevel;
		}

		// 2) Shader Model 上限（DX12 不变量：CheckFeatureSupport 总是返回 S_OK，必须检查出参 HighestShaderModel）
		D3D12_FEATURE_DATA_SHADER_MODEL smData = {};
		smData.HighestShaderModel = D3D_SHADER_MODEL_6_7;
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smData, sizeof(smData))))
		{
			s_GpuCaps.MaxShaderModel = smData.HighestShaderModel;
		}
		else
		{
			smData.HighestShaderModel = D3D_SHADER_MODEL_6_6;
			if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smData, sizeof(smData))))
				s_GpuCaps.MaxShaderModel = smData.HighestShaderModel;
		}

		// 3) Resource Binding Tier (OPTIONS)
		D3D12_FEATURE_DATA_D3D12_OPTIONS opt0 = {};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt0, sizeof(opt0))))
		{
			s_GpuCaps.BindingTier = opt0.ResourceBindingTier;
		}

		// 4) WaveOps (OPTIONS1)
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1 = {};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1))))
		{
			s_GpuCaps.WaveOps = (opt1.WaveOps == TRUE);
		}

		// 5) Native 16-bit Shader Ops (OPTIONS4，面向 Phase 4 FP16 神经计算)
		D3D12_FEATURE_DATA_D3D12_OPTIONS4 opt4 = {};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &opt4, sizeof(opt4))))
		{
			s_GpuCaps.Native16BitOps = (opt4.Native16BitShaderOpsSupported == TRUE);
		}

		// 6) DXR Tier (OPTIONS5)
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))))
		{
			s_GpuCaps.RaytracingTier = s_SimulateNoDxr
				? D3D12_RAYTRACING_TIER_NOT_SUPPORTED
				: opt5.RaytracingTier;
		}

		// 7) SM 6.6 Bindless 准入：SM >= 6.6 && ResourceBindingTier >= Tier 3
		s_GpuCaps.SupportsBindlessSM66 =
			(s_GpuCaps.MaxShaderModel >= D3D_SHADER_MODEL_6_6) &&
			(s_GpuCaps.BindingTier >= D3D12_RESOURCE_BINDING_TIER_3);

		const VramUsageInfo vram = QueryVramUsage();
		LOG_INFO(L"[GpuCaps] Adapter=\"%ws\" VRAM=%lluMB (Usage=%lluMB / Budget=%lluMB) "
			L"SM=0x%X BindingTier=%u BindlessSM66=%d DXR=%u WaveOps=%d FP16=%d TypedUAV_FP16=%d\n",
			s_GpuCaps.AdapterName.c_str(),
			(unsigned long long)(s_GpuCaps.DedicatedVRAMBytes >> 20),
			(unsigned long long)(vram.CurrentUsageBytes >> 20),
			(unsigned long long)(vram.BudgetBytes >> 20),
			(unsigned)s_GpuCaps.MaxShaderModel,
			(unsigned)s_GpuCaps.BindingTier,
			s_GpuCaps.SupportsBindlessSM66 ? 1 : 0,
			(unsigned)s_GpuCaps.RaytracingTier,
			s_GpuCaps.WaveOps ? 1 : 0,
			s_GpuCaps.Native16BitOps ? 1 : 0,
			s_GpuCaps.TypedUAVLoadFP16 ? 1 : 0);
	}

	void ShutdownGpuCaps()
	{
		s_Adapter3.Reset();
		s_GpuCaps = GpuCaps{};
	}

	const GpuCaps& GetGpuCaps()
	{
		return s_GpuCaps;
	}

	VramUsageInfo QueryVramUsage()
	{
		VramUsageInfo info = {};
		if (s_Adapter3 == nullptr)
			return info;

		DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
		if (SUCCEEDED(s_Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo)))
		{
			info.CurrentUsageBytes = static_cast<uint64_t>(memInfo.CurrentUsage);
			info.BudgetBytes       = static_cast<uint64_t>(memInfo.Budget);
		}
		return info;
	}

	bool EvaluateVramBudget(const VramUsageInfo& usage, uint64_t additionalBytes,
		float safetyRatio, std::string* outReason)
	{
		// 当 OS 无法给出有效本地预算（BudgetBytes == 0，如无硬件单测/软件适配器）时放行
		if (usage.BudgetBytes == 0)
			return true;

		const float clampedRatio = (safetyRatio <= 0.0f) ? 0.90f : (safetyRatio > 1.0f ? 1.0f : safetyRatio);
		const double safeLimitBytes = static_cast<double>(usage.BudgetBytes) * static_cast<double>(clampedRatio);

		// 防 uint64_t 加法溢出
		const double projectedBytes = static_cast<double>(usage.CurrentUsageBytes) + static_cast<double>(additionalBytes);
		if (projectedBytes > safeLimitBytes)
		{
			if (outReason != nullptr)
			{
				char buf[256] = {};
				std::snprintf(buf, sizeof(buf),
					"VRAM budget exceeded: current=%.1f MB + requested=%.1f MB = %.1f MB > %.0f%% budget (%.1f / %.1f MB)",
					static_cast<double>(usage.CurrentUsageBytes) / (1024.0 * 1024.0),
					static_cast<double>(additionalBytes) / (1024.0 * 1024.0),
					projectedBytes / (1024.0 * 1024.0),
					clampedRatio * 100.0f,
					safeLimitBytes / (1024.0 * 1024.0),
					static_cast<double>(usage.BudgetBytes) / (1024.0 * 1024.0));
				*outReason = buf;
			}
			return false;
		}
		return true;
	}

	bool CheckVramBudgetForAllocation(uint64_t additionalBytes, float safetyRatio, std::string* outReason)
	{
		return EvaluateVramBudget(QueryVramUsage(), additionalBytes, safetyRatio, outReason);
	}

	void SetSimulatedNoDxrForTest(bool disableDxr)
	{
		s_SimulateNoDxr = disableDxr;
		if (disableDxr)
		{
			s_GpuCaps.RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		}
	}

	bool IsSimulatedNoDxrForTest()
	{
		return s_SimulateNoDxr;
	}
}
