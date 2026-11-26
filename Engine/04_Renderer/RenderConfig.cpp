// ============================================================================
// 妯″潡锛歊enderConfig锛堝垎鍩熷己绫诲瀷娓叉煋閰嶇疆涓?JSON 搴忓垪鍖?路 瀹炵幇锛?// 鏋舵瀯灞傦細Engine/04_Renderer锛堝绾?6 / WP-3B / TD-09d锛?// 璇存槑锛?//   浠撳簱鍐?Engine/03_AssetSystem/Assets/Importers/json.hpp 涓烘棭鏈?3.x 鐗堟湰锛堟棤 3.11 鏂板鐨?//   NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT 瀹忥級锛屾澶勬墜鍐欑畝娲佺殑 ADL
//   to_json / from_json 杈呭姪妯℃澘锛屽仛鍒扮己瀛楁鑷姩娌跨敤榛樿鍊笺€侀敊绫诲瀷涓ユ牸鎶涘紓甯稿苟瀹夊叏鎹曡幏銆?// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/RenderConfig.h"

#include "00_Core/Utility/Utility.h"
#include "03_AssetSystem/Assets/Importers/json.hpp"

#include <Windows.h>
#include <fstream>
#include <sstream>

namespace Renderer
{
	using nlohmann::json;

	namespace
	{
		template <typename T>
		void ReadOpt(const json& j, const char* key, T& dst)
		{
			auto it = j.find(key);
			if (it != j.end() && !it->is_null())
			{
				dst = it->get<T>();
			}
		}

		bool WriteFileAtomically(const std::wstring& filePath, const std::string& content, std::string* outErr)
		{
			if (filePath.empty())
			{
				if (outErr) *outErr = "Empty output file path";
				return false;
			}

			const size_t slashPos = filePath.find_last_of(L"\\/");
			if (slashPos != std::wstring::npos && slashPos > 0)
			{
				const std::wstring dir = filePath.substr(0, slashPos);
				::CreateDirectoryW(dir.c_str(), nullptr);
			}

			const std::wstring tmpPath = filePath + L".tmp";
			{
				std::ofstream ofs(tmpPath, std::ios::out | std::ios::binary | std::ios::trunc);
				if (!ofs.is_open())
				{
					if (outErr) *outErr = "Failed to open temporary file for writing";
					return false;
				}
				ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
				ofs.flush();
				if (!ofs.good())
				{
					if (outErr) *outErr = "Failed while flushing JSON content to temporary file";
					return false;
				}
			}

			if (!::MoveFileExW(tmpPath.c_str(), filePath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				::DeleteFileW(tmpPath.c_str());
				if (outErr) *outErr = "MoveFileExW atomic rename failed";
				return false;
			}
			return true;
		}
	}

	// ---- ADL to_json / from_json锛堢己瀛楁淇濈暀榛樿鍊硷紝閿欑被鍨嬭Е鍙?json::type_error 琚畨鍏ㄦ崟鑾凤級----

	void to_json(json& j, const SceneConfig& c)
	{
		j = json{
			{ "sun_direction", c.sun_direction },
			{ "sun_intensity", c.sun_intensity },
			{ "sun_color", c.sun_color },
			{ "enable_sun", c.enable_sun },
			{ "point_position", c.point_position },
			{ "point_range", c.point_range },
			{ "point_intensity", c.point_intensity },
			{ "point_color", c.point_color },
			{ "enable_point", c.enable_point },
			{ "enable_ibl", c.enable_ibl },
			{ "ibl_intensity", c.ibl_intensity },
			{ "hdr_asset", c.hdr_asset },
		};
	}

	void from_json(const json& j, SceneConfig& c)
	{
		c = SceneConfig{};
		ReadOpt(j, "sun_direction", c.sun_direction);
		ReadOpt(j, "sun_intensity", c.sun_intensity);
		ReadOpt(j, "sun_color", c.sun_color);
		ReadOpt(j, "enable_sun", c.enable_sun);
		ReadOpt(j, "point_position", c.point_position);
		ReadOpt(j, "point_range", c.point_range);
		ReadOpt(j, "point_intensity", c.point_intensity);
		ReadOpt(j, "point_color", c.point_color);
		ReadOpt(j, "enable_point", c.enable_point);
		ReadOpt(j, "enable_ibl", c.enable_ibl);
		ReadOpt(j, "ibl_intensity", c.ibl_intensity);
		ReadOpt(j, "hdr_asset", c.hdr_asset);
	}

	void to_json(json& j, const TonemapConfig& c)
	{
		j = json{
			{ "exposure", c.exposure },
			{ "enable_tonemap", c.enable_tonemap },
			{ "enable_srgb", c.enable_srgb },
		};
	}

	void from_json(const json& j, TonemapConfig& c)
	{
		c = TonemapConfig{};
		ReadOpt(j, "exposure", c.exposure);
		ReadOpt(j, "enable_tonemap", c.enable_tonemap);
		ReadOpt(j, "enable_srgb", c.enable_srgb);
	}

	void to_json(json& j, const ShadowConfig& c)
	{
		j = json{
			{ "enabled", c.enabled },
			{ "algorithm", c.algorithm },
			{ "resolution", c.resolution },
			{ "cascade_count", c.cascade_count },
			{ "normal_bias", c.normal_bias },
			{ "depth_bias", c.depth_bias },
			{ "pcf_radius", c.pcf_radius },
			{ "strength", c.strength },
		};
	}

	void from_json(const json& j, ShadowConfig& c)
	{
		c = ShadowConfig{};
		ReadOpt(j, "enabled", c.enabled);
		ReadOpt(j, "algorithm", c.algorithm);
		ReadOpt(j, "resolution", c.resolution);
		ReadOpt(j, "cascade_count", c.cascade_count);
		ReadOpt(j, "normal_bias", c.normal_bias);
		ReadOpt(j, "depth_bias", c.depth_bias);
		ReadOpt(j, "pcf_radius", c.pcf_radius);
		ReadOpt(j, "strength", c.strength);
	}

	void to_json(json& j, const SsaoConfig& c)
	{
		j = json{
			{ "enabled", c.enabled },
			{ "radius", c.radius },
			{ "bias", c.bias },
			{ "power", c.power },
			{ "sample_count", c.sample_count },
			{ "enable_blur", c.enable_blur },
		};
	}

	void from_json(const json& j, SsaoConfig& c)
	{
		c = SsaoConfig{};
		ReadOpt(j, "enabled", c.enabled);
		ReadOpt(j, "radius", c.radius);
		ReadOpt(j, "bias", c.bias);
		ReadOpt(j, "power", c.power);
		ReadOpt(j, "sample_count", c.sample_count);
		ReadOpt(j, "enable_blur", c.enable_blur);
	}

	void to_json(json& j, const TaaConfig& c)
	{
		j = json{
			{ "enabled", c.enabled },
			{ "blend_factor", c.blend_factor },
			{ "variance_gamma", c.variance_gamma },
			{ "jitter_scale", c.jitter_scale },
		};
	}

	void from_json(const json& j, TaaConfig& c)
	{
		c = TaaConfig{};
		ReadOpt(j, "enabled", c.enabled);
		ReadOpt(j, "blend_factor", c.blend_factor);
		ReadOpt(j, "variance_gamma", c.variance_gamma);
		ReadOpt(j, "jitter_scale", c.jitter_scale);
	}

	void to_json(json& j, const DxrConfig& c)
	{
		j = json{
			{ "enable_shadow", c.enable_shadow },
			{ "enable_reflection", c.enable_reflection },
			{ "shadow_normal_bias", c.shadow_normal_bias },
			{ "shadow_max_distance", c.shadow_max_distance },
			{ "reflection_max_distance", c.reflection_max_distance },
			{ "reflection_intensity", c.reflection_intensity },
			{ "enable_material_textures", c.enable_material_textures },
			{ "enable_ggx", c.enable_ggx },
		};
	}

	void from_json(const json& j, DxrConfig& c)
	{
		c = DxrConfig{};
		ReadOpt(j, "enable_shadow", c.enable_shadow);
		ReadOpt(j, "enable_reflection", c.enable_reflection);
		ReadOpt(j, "shadow_normal_bias", c.shadow_normal_bias);
		ReadOpt(j, "shadow_max_distance", c.shadow_max_distance);
		ReadOpt(j, "reflection_max_distance", c.reflection_max_distance);
		ReadOpt(j, "reflection_intensity", c.reflection_intensity);
		ReadOpt(j, "enable_material_textures", c.enable_material_textures);
		ReadOpt(j, "enable_ggx", c.enable_ggx);
	}

	void to_json(json& j, const IndirectLightConfig& c)
	{
		j = json{
			{ "enabled", c.enabled },
			{ "estimator", c.estimator },
			{ "max_bounce_depth", c.max_bounce_depth },
			{ "samples_per_pixel", c.samples_per_pixel },
			{ "model_manifest_path", c.model_manifest_path },
			{ "tile_dispatch_size", c.tile_dispatch_size },
		};
	}

	void from_json(const json& j, IndirectLightConfig& c)
	{
		c = IndirectLightConfig{};
		ReadOpt(j, "enabled", c.enabled);
		ReadOpt(j, "estimator", c.estimator);
		ReadOpt(j, "max_bounce_depth", c.max_bounce_depth);
		ReadOpt(j, "samples_per_pixel", c.samples_per_pixel);
		ReadOpt(j, "model_manifest_path", c.model_manifest_path);
		ReadOpt(j, "tile_dispatch_size", c.tile_dispatch_size);
	}

	void to_json(json& j, const ExperimentConfig& c)
	{
		j = json{
			{ "camera_trajectory_path", c.camera_trajectory_path },
			{ "random_seed", c.random_seed },
			{ "warmup_frames", c.warmup_frames },
			{ "measure_frames", c.measure_frames },
			{ "interleave_rounds", c.interleave_rounds },
			{ "reference_hdr_path", c.reference_hdr_path },
		};
	}

	void from_json(const json& j, ExperimentConfig& c)
	{
		c = ExperimentConfig{};
		ReadOpt(j, "camera_trajectory_path", c.camera_trajectory_path);
		ReadOpt(j, "random_seed", c.random_seed);
		ReadOpt(j, "warmup_frames", c.warmup_frames);
		ReadOpt(j, "measure_frames", c.measure_frames);
		ReadOpt(j, "interleave_rounds", c.interleave_rounds);
		ReadOpt(j, "reference_hdr_path", c.reference_hdr_path);
	}

	void to_json(json& j, const RenderConfig& c)
	{
		j = json{
			{ "config_version", c.config_version },
			{ "pipeline", c.pipeline },
			{ "scene", c.scene },
			{ "tonemap", c.tonemap },
			{ "shadow", c.shadow },
			{ "ssao", c.ssao },
			{ "taa", c.taa },
			{ "dxr", c.dxr },
			{ "indirect_light", c.indirect_light },
			{ "experiment", c.experiment },
			{ "deferred_debug", c.deferred_debug },
			{ "rt_debug", c.rt_debug },
		};
	}

	void from_json(const json& j, RenderConfig& c)
	{
		c = RenderConfig{};
		ReadOpt(j, "config_version", c.config_version);
		ReadOpt(j, "pipeline", c.pipeline);
		ReadOpt(j, "scene", c.scene);
		ReadOpt(j, "tonemap", c.tonemap);
		ReadOpt(j, "shadow", c.shadow);
		ReadOpt(j, "ssao", c.ssao);
		ReadOpt(j, "taa", c.taa);
		ReadOpt(j, "dxr", c.dxr);
		ReadOpt(j, "indirect_light", c.indirect_light);
		ReadOpt(j, "experiment", c.experiment);
		ReadOpt(j, "deferred_debug", c.deferred_debug);
		ReadOpt(j, "rt_debug", c.rt_debug);
	}

	RenderParams RenderConfig::ToRenderParams() const
	{
		RenderParams p;
		p.SunDirection[0]       = scene.sun_direction[0];
		p.SunDirection[1]       = scene.sun_direction[1];
		p.SunDirection[2]       = scene.sun_direction[2];
		p.SunIntensity          = scene.sun_intensity;
		p.SunColor[0]           = scene.sun_color[0];
		p.SunColor[1]           = scene.sun_color[1];
		p.SunColor[2]           = scene.sun_color[2];
		p.EnableSunLight        = scene.enable_sun;

		p.PointLightPosition[0] = scene.point_position[0];
		p.PointLightPosition[1] = scene.point_position[1];
		p.PointLightPosition[2] = scene.point_position[2];
		p.PointLightRange       = scene.point_range;
		p.PointLightIntensity   = scene.point_intensity;
		p.PointLightColor[0]    = scene.point_color[0];
		p.PointLightColor[1]    = scene.point_color[1];
		p.PointLightColor[2]    = scene.point_color[2];
		p.EnablePointLight      = scene.enable_point;

		p.EnableIBL             = scene.enable_ibl;
		p.IBLIntensity          = scene.ibl_intensity;

		p.Exposure              = tonemap.exposure;
		p.EnableTonemap         = tonemap.enable_tonemap;
		p.EnableSRGBEncode      = tonemap.enable_srgb;

		p.EnableShadow          = shadow.enabled;
		p.ShadowAlgorithm       = (shadow.algorithm == "CSM") ? 1u : 0u;
		p.ShadowResolution      = shadow.resolution;
		p.ShadowCascadeCount    = shadow.cascade_count;
		p.ShadowNormalBias      = shadow.normal_bias;
		p.ShadowDepthBias       = shadow.depth_bias;
		p.ShadowPCFRadius       = shadow.pcf_radius;
		p.ShadowStrength        = shadow.strength;

		p.EnableSSAO            = ssao.enabled;
		p.SSAORadius            = ssao.radius;
		p.SSAOBias              = ssao.bias;
		p.SSAOPower             = ssao.power;
		p.SSAOSampleCount       = ssao.sample_count;
		p.SSAOEnableBlur        = ssao.enable_blur;

		p.EnableTAA             = taa.enabled;
		p.TAABlendFactor        = taa.blend_factor;
		p.TAAVarianceGamma      = taa.variance_gamma;
		p.TAAJitterScale        = taa.jitter_scale;

		p.EnableRTShadow          = dxr.enable_shadow;
		p.EnableRTReflection      = dxr.enable_reflection;
		p.RTShadowNormalBias      = dxr.shadow_normal_bias;
		p.RTShadowMaxDistance     = dxr.shadow_max_distance;
		p.RTReflectionMaxDistance = dxr.reflection_max_distance;
		p.RTReflectionIntensity   = dxr.reflection_intensity;
		p.EnableMaterialTextures  = dxr.enable_material_textures;
		p.EnableGGX               = dxr.enable_ggx;

		const uint32_t maxDeferred = static_cast<uint32_t>(DebugView::Count);
		p.Debug = (deferred_debug < maxDeferred)
			? static_cast<DebugView>(deferred_debug)
			: DebugView::Final;

		const uint32_t maxRt = static_cast<uint32_t>(RayTracingDebugView::Count);
		p.RTDebugView = (rt_debug < maxRt)
			? static_cast<RayTracingDebugView>(rt_debug)
			: RayTracingDebugView::Shaded;

		return p;
	}

	void RenderConfig::FromRenderParams(const RenderParams& p)
	{
		scene.sun_direction   = { p.SunDirection[0], p.SunDirection[1], p.SunDirection[2] };
		scene.sun_intensity   = p.SunIntensity;
		scene.sun_color       = { p.SunColor[0], p.SunColor[1], p.SunColor[2] };
		scene.enable_sun      = p.EnableSunLight;

		scene.point_position  = { p.PointLightPosition[0], p.PointLightPosition[1], p.PointLightPosition[2] };
		scene.point_range     = p.PointLightRange;
		scene.point_intensity = p.PointLightIntensity;
		scene.point_color     = { p.PointLightColor[0], p.PointLightColor[1], p.PointLightColor[2] };
		scene.enable_point    = p.EnablePointLight;

		scene.enable_ibl      = p.EnableIBL;
		scene.ibl_intensity   = p.IBLIntensity;

		tonemap.exposure       = p.Exposure;
		tonemap.enable_tonemap = p.EnableTonemap;
		tonemap.enable_srgb    = p.EnableSRGBEncode;

		shadow.enabled       = p.EnableShadow;
		shadow.algorithm     = (p.ShadowAlgorithm == 1u) ? "CSM" : "SimpleShadowMap";
		shadow.resolution    = p.ShadowResolution;
		shadow.cascade_count = p.ShadowCascadeCount;
		shadow.normal_bias   = p.ShadowNormalBias;
		shadow.depth_bias    = p.ShadowDepthBias;
		shadow.pcf_radius    = p.ShadowPCFRadius;
		shadow.strength      = p.ShadowStrength;

		ssao.enabled      = p.EnableSSAO;
		ssao.radius       = p.SSAORadius;
		ssao.bias         = p.SSAOBias;
		ssao.power        = p.SSAOPower;
		ssao.sample_count = p.SSAOSampleCount;
		ssao.enable_blur  = p.SSAOEnableBlur;

		taa.enabled        = p.EnableTAA;
		taa.blend_factor   = p.TAABlendFactor;
		taa.variance_gamma = p.TAAVarianceGamma;
		taa.jitter_scale   = p.TAAJitterScale;

		dxr.enable_shadow            = p.EnableRTShadow;
		dxr.enable_reflection        = p.EnableRTReflection;
		dxr.shadow_normal_bias       = p.RTShadowNormalBias;
		dxr.shadow_max_distance      = p.RTShadowMaxDistance;
		dxr.reflection_max_distance  = p.RTReflectionMaxDistance;
		dxr.reflection_intensity     = p.RTReflectionIntensity;
		dxr.enable_material_textures = p.EnableMaterialTextures;
		dxr.enable_ggx               = p.EnableGGX;

		deferred_debug = static_cast<uint32_t>(p.Debug);
		rt_debug       = static_cast<uint32_t>(p.RTDebugView);
	}

	std::wstring RenderConfig::GetPipelineRendererName() const
	{
		if (pipeline == "Forward" || pipeline == "ForwardRenderer")
			return L"ForwardRenderer";
		if (pipeline == "RayTracing" || pipeline == "DXR" || pipeline == "RayTracingRenderer")
			return L"RayTracingRenderer";
		if (pipeline == "GFL2" || pipeline == "GFL2CharacterPipeline")
			return L"GFL2CharacterPipeline";
		return L"DeferredRenderer";
	}

	void RenderConfig::SetPipelineFromRendererName(const std::wstring& rendererName)
	{
		if (rendererName == L"ForwardRenderer")
			pipeline = "Forward";
		else if (rendererName == L"RayTracingRenderer")
			pipeline = "RayTracing";
		else if (rendererName == L"GFL2CharacterPipeline")
			pipeline = "GFL2";
		else
			pipeline = "Deferred";
	}

	bool RenderConfig::ParseFromJsonString(const std::string& jsonText, std::string* outErr)
	{
		try
		{
			const json root = json::parse(jsonText);
			if (!root.is_object())
			{
				if (outErr) *outErr = "RenderConfig JSON root must be an object";
				return false;
			}
			RenderConfig parsed;
			from_json(root, parsed);
			if (parsed.config_version == 0 || parsed.config_version > 2)
			{
				if (outErr) *outErr = "Unsupported RenderConfig config_version";
				return false;
			}
			*this = std::move(parsed);
			return true;
		}
		catch (const std::exception& e)
		{
			if (outErr) *outErr = std::string("RenderConfig JSON parse error: ") + e.what();
			return false;
		}
	}

	std::string RenderConfig::DumpToJsonString() const
	{
		json root;
		to_json(root, *this);
		return root.dump(2);
	}

	bool RenderConfig::LoadFromFile(const std::wstring& filePath, std::string* outErr)
	{
		std::ifstream ifs(filePath, std::ios::in | std::ios::binary);
		if (!ifs.is_open())
		{
			if (outErr) *outErr = "Failed to open RenderConfig JSON file";
			return false;
		}
		std::ostringstream ss;
		ss << ifs.rdbuf();
		return ParseFromJsonString(ss.str(), outErr);
	}

	bool RenderConfig::SaveToFile(const std::wstring& filePath, std::string* outErr) const
	{
		return WriteFileAtomically(filePath, DumpToJsonString() + "\n", outErr);
	}

	bool WriteStatsReportJson(const std::wstring& filePath, const FrameStatsReport& report, std::string* outErr)
	{
		try
		{
			json root = {
				{ "pipeline", report.Pipeline },
				{ "width", report.Width },
				{ "height", report.Height },
				{ "frame_index", report.FrameIndex },
				{ "cpu_frame_ms", report.CpuFrameMs },
				{ "gpu_frame_ms", report.GpuFrameMs },
				{ "vram_usage_mb", report.VramUsageMB },
				{ "vram_budget_mb", report.VramBudgetMB },
				{ "bindless_sm66", report.BindlessSM66 },
				{ "set_descriptor_heaps_calls", report.SetHeapCalls },
			};
			json passes = json::array();
			for (const auto& pt : report.Passes)
			{
				passes.push_back({ { "name", pt.Name }, { "gpu_ms", pt.GpuMs } });
			}
			root["passes"] = std::move(passes);
			return WriteFileAtomically(filePath, root.dump(2) + "\n", outErr);
		}
		catch (const std::exception& e)
		{
			if (outErr) *outErr = std::string("StatsOut JSON serialize error: ") + e.what();
			return false;
		}
	}
}
