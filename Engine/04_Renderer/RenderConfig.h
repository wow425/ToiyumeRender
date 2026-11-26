// ============================================================================
// 妯″潡锛歊enderConfig锛堝垎鍩熷己绫诲瀷娓叉煋閰嶇疆 + ExperimentManifest JSON 濂戠害锛?// 鏋舵瀯灞傦細Engine/04_Renderer锛堝绾?6 / WP-3B / TD-09d锛?// 鏍稿績鑱岃矗锛?//   1) 灏嗗钩閾虹殑 RenderParams 鎷嗗垎涓?6 涓竻鏅板瓙鍩燂紙Scene / Tonemap / Shadow / DXR /
//      IndirectLight / Experiment锛夛紝骞朵笌鐜版湁 RenderParams 鍙屽悜鏃犳崯浜掕浆锛?//   2) 鍩轰簬浠撳簱鍐呮棦鏈?nlohmann/json.hpp锛圗ngine/03_AssetSystem/Assets/Importers/json.hpp锛?//      鎻愪緵寮虹被鍨?JSON Preset 璇诲啓锛?Preset / -Experiment锛夛紝鍧忔枃浠?缂哄瓧娈?閿欑被鍨嬪潎
//      瀹夊叏鎷︽埅杩斿洖 false 骞跺啓鏄庨敊璇師鍥狅紝缁濅笉鎶涙湭鎹曡幏寮傚父鎴栧穿婧冿紱
//   3) 鎻愪緵 -StatsOut <json> 瀹㈣閬ユ祴瀵煎嚭锛堝叏甯т笌閫?Pass GPU ms銆丆PU ms銆乂RAM 鍗犵敤/棰勭畻锛夈€?// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "04_Renderer/RenderParams.h"

namespace Renderer
{
	struct SceneConfig
	{
		std::array<float, 3> sun_direction   = { 0.3f, -1.0f, -0.5f };
		float                sun_intensity   = 3.0f;
		std::array<float, 3> sun_color       = { 1.0f, 0.95f, 0.9f };
		bool                 enable_sun      = true;

		std::array<float, 3> point_position  = { 0.0f, 2.0f, -2.0f };
		float                point_range     = 30.0f;
		float                point_intensity = 4.0f;
		std::array<float, 3> point_color     = { 1.0f, 0.8f, 0.6f };
		bool                 enable_point    = true;

		bool                 enable_ibl      = true;
		float                ibl_intensity   = 1.0f;
		std::string          hdr_asset       = "Skybox/grasslands_sunset_2k.dds";
	};

	struct TonemapConfig
	{
		float exposure       = 1.0f;
		bool  enable_tonemap = true;
		bool  enable_srgb    = true;
	};

	struct ShadowConfig
	{
		bool        enabled       = false;
		std::string algorithm     = "SimpleShadowMap";
		uint32_t    resolution    = 2048;
		uint32_t    cascade_count = 4;
		float       normal_bias   = 0.005f;
		float       depth_bias    = 0.001f;
		float       pcf_radius    = 1.5f;
		float       strength      = 1.0f;
	};

	struct SsaoConfig
	{
		bool     enabled      = false;
		float    radius       = 0.5f;
		float    bias         = 0.025f;
		float    power        = 1.5f;
		uint32_t sample_count = 16;
		bool     enable_blur  = true;
	};

	struct TaaConfig
	{
		bool  enabled        = false;
		float blend_factor   = 0.10f;
		float variance_gamma = 1.25f;
		float jitter_scale   = 1.0f;
	};

	struct DxrConfig
	{
		bool  enable_shadow           = true;
		bool  enable_reflection       = false;
		float shadow_normal_bias      = 0.002f;
		float shadow_max_distance     = 100.0f;
		float reflection_max_distance = 200.0f;
		float reflection_intensity    = 0.6f;
		bool  enable_material_textures = true;
		bool  enable_ggx              = true;
	};

	struct IndirectLightConfig
	{
		bool        enabled             = false;
		std::string estimator           = "DXR_PathContinuation";
		uint32_t    max_bounce_depth    = 2;
		uint32_t    samples_per_pixel   = 1;
		std::string model_manifest_path;
		uint32_t    tile_dispatch_size  = 0;
	};

	struct ExperimentConfig
	{
		std::string camera_trajectory_path;
		uint32_t    random_seed       = 42;
		uint32_t    warmup_frames     = 30;
		uint32_t    measure_frames    = 120;
		uint32_t    interleave_rounds = 3;
		std::string reference_hdr_path;
	};

	struct RenderConfig
	{
		uint32_t            config_version = 2;
		std::string         pipeline       = "Deferred";
		SceneConfig         scene;
		TonemapConfig       tonemap;
		ShadowConfig        shadow;
		SsaoConfig          ssao;
		TaaConfig           taa;
		DxrConfig           dxr;
		IndirectLightConfig indirect_light;
		ExperimentConfig    experiment;
		uint32_t            deferred_debug = 0;
		uint32_t            rt_debug       = 0;

		// 涓庣幇鏈?RenderParams 鍞竴 CPU 鐪熸簮鍙屽悜鏃犳崯杞崲
		RenderParams ToRenderParams() const;
		void FromRenderParams(const RenderParams& params);

		// "Deferred"/"Forward"/"RayTracing" <-> L"DeferredRenderer"/L"ForwardRenderer"/L"RayTracingRenderer"
		std::wstring GetPipelineRendererName() const;
		void SetPipelineFromRendererName(const std::wstring& rendererName);

		// 瀹夊叏 JSON 瀛楃涓茶В鏋?搴忓垪鍖栵紙鍐呴儴 try/catch锛屽潖 JSON 杩斿洖 false锛屾案涓嶅穿婧冿級
		bool ParseFromJsonString(const std::string& jsonText, std::string* outErr = nullptr);
		std::string DumpToJsonString() const;

		// 纾佺洏鏂囦欢璇诲啓锛歋aveToFile 閲囩敤涓存椂鏂囦欢 + MoveFileExW 鍘熷瓙鏇挎崲锛岄伩鍏嶅崐鍐欏叆鎹熷潖
		bool LoadFromFile(const std::wstring& filePath, std::string* outErr = nullptr);
		bool SaveToFile(const std::wstring& filePath, std::string* outErr = nullptr) const;
	};

	// -StatsOut <json> 客观帧统计导出结构
	struct FrameStatsPassEntry
	{
		std::string Name;
		float       GpuMs = 0.0f;
	};

	struct FrameStatsReport
	{
		std::string                      Pipeline;
		uint32_t                         Width          = 0;
		uint32_t                         Height         = 0;
		uint64_t                         FrameIndex     = 0;
		float                            CpuFrameMs     = 0.0f;
		float                            GpuFrameMs     = 0.0f;
		double                           VramUsageMB    = 0.0;
		double                           VramBudgetMB   = 0.0;
		bool                             BindlessSM66   = false;
		uint32_t                         SetHeapCalls   = 0;
		std::vector<FrameStatsPassEntry> Passes;
	};

	bool WriteStatsReportJson(const std::wstring& filePath, const FrameStatsReport& report,
		std::string* outErr = nullptr);
}
