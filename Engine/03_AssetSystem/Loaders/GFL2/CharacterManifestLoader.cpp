#include "00_Core/PCH.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "03_AssetSystem/Assets/Importers/glTF.h"
#include "03_AssetSystem/Assets/Importers/json.hpp"
#include "03_AssetSystem/Loaders/GFL2/GFL2MathConvert.h"

#include <cfloat>
#include <cmath>
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace GFL2
{
	namespace
	{
		std::wstring Utf8ToWide(const std::string& utf8)
		{
			if (utf8.empty())
				return {};
			const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
			if (len <= 0)
				return {};
			std::wstring out(static_cast<size_t>(len), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
			return out;
		}

		// 灏?RFC-8259 闈炴硶瑁稿瓧闈㈤噺 NaN / -Infinity / Infinity锛堜粎寮曞彿澶栵級瑙勮寖鍖栦负 0.0
		std::string SanitizeJsonNonFiniteTokens(const std::string& src)
		{
			std::string out;
			out.reserve(src.size());
			bool inString = false;
			bool escaped  = false;
			for (size_t i = 0; i < src.size();)
			{
				const char c = src[i];
				if (inString)
				{
					out.push_back(c);
					if (escaped) escaped = false;
					else if (c == '\\') escaped = true;
					else if (c == '"') inString = false;
					++i;
					continue;
				}
				if (c == '"')
				{
					inString = true;
					out.push_back(c);
					++i;
					continue;
				}
				auto matchBare = [&](const char* tok, size_t tokLen) -> bool
				{
					if (i + tokLen > src.size()) return false;
					if (src.compare(i, tokLen, tok) != 0) return false;
					const bool prevOk = (i == 0) || (!std::isalnum(static_cast<unsigned char>(src[i - 1])) && src[i - 1] != '_');
					const bool nextOk = (i + tokLen == src.size()) || (!std::isalnum(static_cast<unsigned char>(src[i + tokLen])) && src[i + tokLen] != '_');
					return prevOk && nextOk;
				};
				if (matchBare("NaN", 3))       { out.append("0.0"); i += 3; continue; }
				if (matchBare("-Infinity", 9)) { out.append("0.0"); i += 9; continue; }
				if (matchBare("+Infinity", 9)) { out.append("0.0"); i += 9; continue; }
				if (matchBare("Infinity", 8))  { out.append("0.0"); i += 8; continue; }
				out.push_back(c);
				++i;
			}
			return out;
		}

		bool ReadJsonFile(const fs::path& filePath, json& outJson)
		{
			std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
			if (!ifs.is_open())
				return false;
			const std::streamsize sz = ifs.tellg();
			if (sz <= 0)
				return false;
			std::string raw(static_cast<size_t>(sz), '\0');
			ifs.seekg(0, std::ios::beg);
			if (!ifs.read(raw.data(), sz))
				return false;

			// 剥离可选 UTF-8 BOM 并在解析前清洗 NaN/Infinity（避免 MSVC 调试器捕获首chance parse_error 异常）
			if (raw.size() >= 3 &&
				static_cast<unsigned char>(raw[0]) == 0xEF &&
				static_cast<unsigned char>(raw[1]) == 0xBB &&
				static_cast<unsigned char>(raw[2]) == 0xBF)
			{
				raw.erase(0, 3);
			}

			const std::string sanitized = SanitizeJsonNonFiniteTokens(raw);
			outJson = json::parse(sanitized, nullptr, /*allow_exceptions=*/false);
			if (outJson.is_discarded())
			{
				LOG_ERROR(L"[GFL2][M0] JSON parse error in %ws\n", filePath.c_str());
				return false;
			}
			return true;
		}

		bool GetExeDirectory(fs::path& outDir)
		{
			std::wstring buf(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
				if (n == 0)
					return false;
				if (n < buf.size())
				{
					buf.resize(n);
					break;
				}
				buf.resize(buf.size() * 2u);
			}
			outDir = fs::path(buf).parent_path();
			return !outDir.empty();
		}

		bool TryResolveAK12FromAncestor(const fs::path& start, int maxDepth, fs::path& outDir)
		{
			// docs/AI/灏戝墠2澶嶅埢/璧勪骇/AK12
			static const wchar_t kRelWide[] = L"docs\\AI\\\u5c11\u524d2\u590d\u523b\\\u8d44\u4ea7\\AK12";
			std::error_code ec;
			fs::path cur = start;
			for (int i = 0; i < maxDepth && !cur.empty(); ++i)
			{
				const fs::path cand = cur / kRelWide;
				if (fs::is_regular_file(cand / L"ak12_manifest.json", ec))
				{
					outDir = cand;
					return true;
				}
				if (!cur.has_parent_path() || cur.parent_path() == cur)
					break;
				cur = cur.parent_path();
			}
			return false;
		}

		ShadingModel ClassifyShadingModel(uint32_t eventId, const std::string& slug, uint32_t fragShaderId)
		{
			if (fragShaderId == 7344u) return ShadingModel::Outline;
			if (fragShaderId == 8202u || fragShaderId == 8204u) return ShadingModel::Transparent;
			if (fragShaderId == 8189u) return ShadingModel::EyeBase;
			if (fragShaderId == 8191u) return ShadingModel::EyeAdd;
			if (fragShaderId == 7335u) return ShadingModel::EyeMul;
			if (fragShaderId == 8185u || fragShaderId == 7337u) return ShadingModel::Face;
			if (fragShaderId == 8187u || fragShaderId == 8193u || fragShaderId == 7328u) return ShadingModel::Hair;
			if (fragShaderId == 8177u || eventId == 2788u) return ShadingModel::Nails;
			if (slug.find("wpn_") == 0) return ShadingModel::Weapon;
			if (slug.find("cloth_") == 0 || slug.find("coat_") == 0 || slug.find("glasses") != std::string::npos)
				return ShadingModel::Cloth;
			return ShadingModel::Body;
		}

		CullModeId ParseCullMode(const std::string& s)
		{
			if (s.find("Front") != std::string::npos) return CullModeId::Front;
			if (s.find("NoCull") != std::string::npos || s.find("None") != std::string::npos) return CullModeId::None;
			return CullModeId::Back;
		}

		BlendModeId ParseBlendMode(const json& renderState)
		{
			if (!renderState.value("blendEnabled", false))
				return BlendModeId::Opaque;
			const auto& cb = renderState["colorBlend"];
			const std::string src = cb.value("sourceFactor", "");
			const std::string dst = cb.value("destinationFactor", "");
			if (src.find("One") != std::string::npos && dst.find("One") != std::string::npos)
				return BlendModeId::Additive;
			if (src.find("DstCol") != std::string::npos)
				return BlendModeId::Multiply;
			return BlendModeId::AlphaBlend;
		}

		StencilPresetId ClassifyStencilPreset(uint32_t eventId, const json& renderState, uint8_t& outRef)
		{
			outRef = 0;
			if (!renderState.value("stencilTestEnabled", false))
				return StencilPresetId::Disabled;

			const auto& sf = renderState["stencilFront"];
			const auto& sb = renderState["stencilBack"];
			const uint32_t frontRef = sf.value("reference", 0u);
			outRef = static_cast<uint8_t>(frontRef & 0xFFu);

			if (eventId == 3198u) return StencilPresetId::BangsShadowInc200To201;
			if (eventId == 3295u) return StencilPresetId::FaceRedrawDec201To200;
			if (eventId == 3178u) return StencilPresetId::HairBangsNotEqual204;
			if (eventId == 3335u) return StencilPresetId::BangsOverEyeEqual204;
			if (eventId == 3577u)
			{
				outRef = static_cast<uint8_t>(sb.value("reference", 204u) & 0xFFu);
				return StencilPresetId::OutlineNotEqual204;
			}
			if (frontRef == 203u) return StencilPresetId::Write203Always;
			if (frontRef == 204u) return StencilPresetId::Write204Always;
			if (frontRef == 200u) return StencilPresetId::Write200Always;
			return StencilPresetId::Disabled;
		}

		float JsonFloat(const json& obj, const char* key, float defVal = 0.0f)
		{
			if (!obj.is_object() || !obj.contains(key))
				return defVal;
			const auto& v = obj[key];
			return v.is_number() ? v.get<float>() : defVal;
		}

		uint32_t JsonUInt(const json& obj, const char* key, uint32_t defVal = 0u)
		{
			if (!obj.is_object() || !obj.contains(key))
				return defVal;
			const auto& v = obj[key];
			return v.is_number() ? v.get<uint32_t>() : defVal;
		}

		std::string JsonStr(const json& obj, const char* key, const std::string& defVal = "")
		{
			if (!obj.is_object() || !obj.contains(key))
				return defVal;
			const auto& v = obj[key];
			return v.is_string() ? v.get<std::string>() : defVal;
		}

		void ReadFloat4Array(const json& j, float out[4], float def0 = 0.0f, float def1 = 0.0f, float def2 = 0.0f, float def3 = 0.0f)
		{
			out[0] = def0; out[1] = def1; out[2] = def2; out[3] = def3;
			if (!j.is_array())
				return;
			for (size_t i = 0; i < 4 && i < j.size(); ++i)
				if (j[i].is_number())
					out[i] = j[i].get<float>();
		}

		void AssignXMFloat4(DirectX::XMFLOAT4& dst, const json& j)
		{
			if (j.is_null())
				return;
			float tmp[4] = {};
			ReadFloat4Array(j, tmp);
			dst = DirectX::XMFLOAT4(tmp[0], tmp[1], tmp[2], tmp[3]);
		}

		void PopulateCb2FallbackFromSemantics(const json& sem, DirectX::XMFLOAT4 rawCb2[kCb2Vec4Count])
		{
			if (!sem.is_object())
				return;
			if (sem.contains("alphaAndAddShadowFactor")) AssignXMFloat4(rawCb2[0], sem["alphaAndAddShadowFactor"]);
			if (sem.contains("baseMapST"))               AssignXMFloat4(rawCb2[1], sem["baseMapST"]);
			if (sem.contains("rmoMapST"))                AssignXMFloat4(rawCb2[2], sem["rmoMapST"]);
			if (sem.contains("normalMapST"))             AssignXMFloat4(rawCb2[3], sem["normalMapST"]);
			if (sem.contains("fresnelColorA"))           AssignXMFloat4(rawCb2[5], sem["fresnelColorA"]);
			if (sem.contains("fresnelColorB"))           AssignXMFloat4(rawCb2[6], sem["fresnelColorB"]);
			if (sem.contains("sparkleAndFresnelParams")) AssignXMFloat4(rawCb2[7], sem["sparkleAndFresnelParams"]);
			if (sem.contains("baseColorTintRGBA"))       AssignXMFloat4(rawCb2[11], sem["baseColorTintRGBA"]);
			if (sem.contains("postMultiplyTint"))        AssignXMFloat4(rawCb2[12], sem["postMultiplyTint"]);
			if (sem.contains("outlineLitColor"))         AssignXMFloat4(rawCb2[13], sem["outlineLitColor"]);
			if (sem.contains("outlineShadowColor"))      AssignXMFloat4(rawCb2[14], sem["outlineShadowColor"]);
			if (sem.contains("outlineParams") && sem["outlineParams"].is_object())
			{
				const auto& op = sem["outlineParams"];
				rawCb2[15] = DirectX::XMFLOAT4(
					JsonFloat(op, "widthScale", 0.0f),
					JsonFloat(op, "zBiasOffset", 0.0f),
					JsonFloat(op, "colorMult", 1.0f),
					0.5f);
			}
			if (sem.contains("emissiveAndHairSpec") && sem["emissiveAndHairSpec"].is_object())
			{
				const auto& eh = sem["emissiveAndHairSpec"];
				rawCb2[16] = DirectX::XMFLOAT4(
					JsonFloat(eh, "emissiveScale", 0.0f),
					0.0f,
					JsonFloat(eh, "hairOrFaceSpecIntensity", 1.0f),
					JsonFloat(eh, "viewYUvShift", 0.05f));
			}
			if (sem.contains("faceSdfSmoothAndShadowLightBias") && sem["faceSdfSmoothAndShadowLightBias"].is_object())
			{
				const auto& fs = sem["faceSdfSmoothAndShadowLightBias"];
				rawCb2[17] = DirectX::XMFLOAT4(
					JsonFloat(fs, "sdfSmoothWidth", 0.1f),
					JsonFloat(fs, "shadowLightDirOffset", 0.1f),
					0.0f,
					0.0f);
			}
			if (sem.contains("featureFlags_cb2_30") && sem["featureFlags_cb2_30"].is_object())
			{
				const auto& ff = sem["featureFlags_cb2_30"];
				rawCb2[30] = DirectX::XMFLOAT4(
					JsonFloat(ff, "shadowLightBiasEnable", 0.0f),
					JsonFloat(ff, "rampMapEnable", 1.0f),
					JsonFloat(ff, "useSecondUV", 0.0f),
					JsonFloat(ff, "shLuminanceNormalizeEnable", 1.0f));
			}
			if (sem.contains("anisotropy_cb2_31")) AssignXMFloat4(rawCb2[31], sem["anisotropy_cb2_31"]);
		}
	}

	std::wstring CharacterManifestLoader::ResolveAK12Root(const std::wstring& customAssetDir)
	{
		std::error_code ec;
		if (!customAssetDir.empty())
		{
			fs::path p(customAssetDir);
			if (fs::is_regular_file(p / L"ak12_manifest.json", ec))
				return p.wstring();
		}

		fs::path exeDir;
		if (GetExeDirectory(exeDir))
		{
			fs::path found;
			if (TryResolveAK12FromAncestor(exeDir, 8, found))
				return found.wstring();
		}

		fs::path cwd = fs::current_path(ec);
		if (!cwd.empty())
		{
			fs::path found;
			if (TryResolveAK12FromAncestor(cwd, 8, found))
				return found.wstring();
		}

#ifdef TOOIYUME_SOURCE_ROOT_DIR
		{
			fs::path found;
			if (TryResolveAK12FromAncestor(fs::path(TOOIYUME_SOURCE_ROOT_DIR), 2, found))
				return found.wstring();
		}
#endif
		return {};
	}

	uint32_t CharacterManifestLoader::FindTextureBindlessIdx(uint32_t resourceId) const
	{
		auto it = m_TexIndexByResourceId.find(resourceId);
		if (it == m_TexIndexByResourceId.end())
			return kInvalidBindlessIdx;
		return m_Textures[it->second].BindlessIdx;
	}

	bool CharacterManifestLoader::LoadAK12(Renderer::RenderServices& services, const std::wstring& customAssetDir)
	{
		if (m_Stats.Loaded)
			return true;

		if (Graphics::g_Device == nullptr || !services.IsInitialized())
		{
			LOG_ERROR(L"[GFL2][M0] CharacterManifestLoader::LoadAK12 failed: RenderServices/Device not ready\n");
			return false;
		}

		const std::wstring rootStr = ResolveAK12Root(customAssetDir);
		if (rootStr.empty())
		{
			LOG_ERROR(L"[GFL2][M0] Failed to locate docs/AI/灏戝墠2澶嶅埢/璧勪骇/AK12/ak12_manifest.json\n");
			return false;
		}

		const fs::path ak12Root(rootStr);
		json manifest;
		if (!ReadJsonFile(ak12Root / L"ak12_manifest.json", manifest))
		{
			LOG_ERROR(L"[GFL2][M0] Failed to parse ak12_manifest.json at %ws\n", ak12Root.c_str());
			return false;
		}

		Renderer::UnifiedDescriptorHeap& heap = services.GetDescriptorHeap();
		if (!heap.IsInitialized())
		{
			LOG_ERROR(L"[GFL2][M0] UnifiedDescriptorHeap not initialized\n");
			return false;
		}

		try
		{

		m_Stats = {};
		m_Stats.AssetDir = ak12Root.wstring();

		// 1. 鏋勫缓 submesh rep_eid -> submeshIndex 鏄犲皠琛紙28 鍞竴瀛愮綉鏍硷級
		std::unordered_map<uint32_t, uint32_t> eidToSubmeshIdx;
		if (manifest.contains("submeshes") && manifest["submeshes"].is_array())
		{
			const auto& subs = manifest["submeshes"];
			m_Stats.UniqueSubmeshes = static_cast<uint32_t>(subs.size());
			for (uint32_t si = 0; si < static_cast<uint32_t>(subs.size()); ++si)
			{
				const auto& sm = subs[si];
				if (sm.contains("reusedByEids") && sm["reusedByEids"].is_array())
				{
					for (const auto& eidVal : sm["reusedByEids"])
						eidToSubmeshIdx[eidVal.get<uint32_t>()] = si;
				}
				if (sm.contains("rep_eid"))
					eidToSubmeshIdx[sm["rep_eid"].get<uint32_t>()] = si;
			}
		}

		// 2. 鎸夌簿纭?sRGB / Linear 鏍囧織鍔犺浇鍏ㄩ噺 52 寮犳牳蹇?AK-12 .dds 璐村浘骞剁櫥璁拌嚦 Zone A
		if (manifest.contains("textures") && manifest["textures"].is_object())
		{
			for (auto it = manifest["textures"].begin(); it != manifest["textures"].end(); ++it)
			{
				const auto& texObj = it.value();
				const uint32_t resId = texObj.value("resourceId", 0u);
				const std::string cs = texObj.value("colorSpace", "Linear");
				const bool isSRGB = (cs == "sRGB");

				std::string ddsRel;
				if (texObj.contains("files") && texObj["files"].is_array())
				{
					for (const auto& f : texObj["files"])
					{
						const std::string s = f.get<std::string>();
						if (s.size() >= 4 && _stricmp(s.c_str() + s.size() - 4, ".dds") == 0)
						{
							ddsRel = s;
							break;
						}
					}
				}
				if (resId == 0u || ddsRel.empty())
					continue;

				const std::wstring wideRel = Utf8ToWide(ddsRel);
				const fs::path fullPath = ak12Root / L"textures" / wideRel;
				TextureRef texRef = TextureManager::LoadDDSFromFile(fullPath.wstring(), Graphics::kMagenta2D, isSRGB);
				if (!texRef.IsValid())
				{
					LOG_ERROR(L"[GFL2][M0] Texture load failed (resId=%u, sRGB=%d): %ws\n",
						resId, isSRGB ? 1 : 0, fullPath.c_str());
					Shutdown(&services);
					return false;
				}

				const Renderer::BindlessAllocation slot = heap.AllocPersistentSingle();
				if (!slot.IsValid())
				{
					LOG_ERROR(L"[GFL2][M0] Zone A exhausted while allocating texture resId=%u\n", resId);
					Shutdown(&services);
					return false;
				}
				Graphics::g_Device->CopyDescriptorsSimple(
					1, slot.Cpu, texRef.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				m_AllocatedZoneASlots.push_back(slot.HeapIndex);

				LoadedTextureSlot entry;
				entry.ResourceId  = resId;
				entry.BindlessIdx = slot.HeapIndex;
				entry.IsSRGB      = isSRGB;
				entry.Role        = texObj.value("role", "");
				entry.FileName    = wideRel;
				entry.Texture     = texRef;

				m_TexIndexByResourceId[resId] = m_Textures.size();
				m_Textures.push_back(std::move(entry));

				++m_Stats.LoadedTextureCount;
				if (isSRGB) ++m_Stats.SrgbTextureCount;
				else        ++m_Stats.LinearTextureCount;
			}
		}

		const uint32_t probe5682Idx = FindTextureBindlessIdx(5682u);

		// 3. 瑙ｆ瀽 55 浠?materials/EID*.json -> GFL2DrawData + GFL2MaterialData
		if (!manifest.contains("drawCalls") || !manifest["drawCalls"].is_array())
		{
			LOG_ERROR(L"[GFL2][M0] ak12_manifest.json missing drawCalls array\n");
			Shutdown(&services);
			return false;
		}

		const auto& draws = manifest["drawCalls"];
		m_CpuDraws.reserve(draws.size());
		m_CpuMaterials.reserve(draws.size());
		m_DrawMeta.reserve(draws.size());

		for (size_t di = 0; di < draws.size(); ++di)
		{
			const auto& dc = draws[di];
			const uint32_t eventId = dc.value("eventId", 0u);
			const std::string matRel = dc.value("materialJson", "");
			if (matRel.empty())
				continue;

			json matJson;
			const fs::path matPath = ak12Root / Utf8ToWide(matRel);
			if (!ReadJsonFile(matPath, matJson))
			{
				LOG_ERROR(L"[GFL2][M0] Failed to read material json: %ws\n", matPath.c_str());
				Shutdown(&services);
				return false;
			}

			DrawCallMeta meta;
			meta.EventId       = eventId;
			meta.Slug          = JsonStr(dc, "slug", JsonStr(matJson, "slug", ""));
			meta.PartNameCN    = JsonStr(dc, "partNameCN", JsonStr(matJson, "partNameCN", ""));
			meta.PassName      = JsonStr(dc, "pass", JsonStr(matJson, "pass", ""));
			meta.IndexCount    = JsonUInt(dc, "indexCount", 0u);
			meta.TriangleCount = JsonUInt(dc, "triangleCount", 0u);
			if (matJson.contains("shaders"))
			{
				const auto& sh = matJson["shaders"];
				if (sh.contains("vertex") && sh["vertex"].contains("shaderId") && sh["vertex"]["shaderId"].is_number_unsigned())
					meta.VertexShaderId = sh["vertex"]["shaderId"].get<uint32_t>();
				if (sh.contains("fragment") && sh["fragment"].contains("shaderId") && sh["fragment"]["shaderId"].is_number_unsigned())
					meta.FragmentShaderId = sh["fragment"]["shaderId"].get<uint32_t>();
			}

			if (auto subIt = eidToSubmeshIdx.find(eventId); subIt != eidToSubmeshIdx.end())
				meta.SubmeshIndex = subIt->second;

			meta.ShadingModelId = ClassifyShadingModel(eventId, meta.Slug, meta.FragmentShaderId);
			const auto& rs = matJson["renderState"];
			meta.CullMode      = ParseCullMode(rs.value("cullMode", "CullMode.Back"));
			meta.BlendMode     = ParseBlendMode(rs);
			meta.StencilPreset = ClassifyStencilPreset(eventId, rs, meta.StencilRef);

			GFL2MaterialData matData{};
			matData.BaseColorIdx       = Renderer::UnifiedDescriptorHeap::kSentinelWhite2D;
			matData.NormalIdx          = Renderer::UnifiedDescriptorHeap::kSentinelFlatNormal2D;
			matData.RmoOrSdfIdx        = Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
			matData.RampMapIdx         = Renderer::UnifiedDescriptorHeap::kSentinelWhite2D;
			matData.HairSpecAoIdx      = Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
			matData.EyeBlendIdx        = Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
			matData.ReflectionProbeIdx = (probe5682Idx != kInvalidBindlessIdx) ? probe5682Idx : Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
			matData.EventId            = eventId;
			matData.ShadingModelId     = static_cast<uint32_t>(meta.ShadingModelId);
			matData.CullMode           = static_cast<uint32_t>(meta.CullMode);
			matData.StencilRefAndPreset =
				static_cast<uint32_t>(meta.StencilRef) |
				(static_cast<uint32_t>(meta.StencilPreset) << 8u) |
				(static_cast<uint32_t>(meta.BlendMode) << 16u);

			uint32_t matFlags = kMatFlag_None;
			if (matJson.contains("textures") && matJson["textures"].is_array())
			{
				for (const auto& t : matJson["textures"])
				{
					const uint32_t tid = JsonUInt(t, "resourceId", 0u);
					const uint32_t bIdx = FindTextureBindlessIdx(tid);
					if (bIdx == kInvalidBindlessIdx)
						continue;
					const std::string role = JsonStr(t, "role", "");
					if (role == "BaseColor")            matData.BaseColorIdx = bIdx;
					else if (role == "BaseColorAlpha") { matData.BaseColorIdx = bIdx; matFlags |= kMatFlag_HasBaseColorAlpha; }
					else if (role == "Normal")          matData.NormalIdx = bIdx;
					else if (role == "RMO")             matData.RmoOrSdfIdx = bIdx;
					else if (role == "SDFMask")        { matData.RmoOrSdfIdx = bIdx; matFlags |= kMatFlag_HasSDFMask; }
					else if (role == "RampMap")         matData.RampMapIdx = bIdx;
					else if (role == "SpecAO")         { matData.HairSpecAoIdx = bIdx; matFlags |= kMatFlag_HasHairSpecAO; }
					else if (role == "EyeBlend")       { matData.EyeBlendIdx = bIdx; matFlags |= kMatFlag_HasEyeBlend; }
					else if (role == "ReflectionProbe6") matData.ReflectionProbeIdx = bIdx;
				}
			}

			// 濉厖 cb2[0..31]
			const auto& cb2Block = matJson["cb2_perMaterial"];
			if (cb2Block.contains("rawVec4") && cb2Block["rawVec4"].is_object() && !cb2Block["rawVec4"].empty())
			{
				const auto& raw = cb2Block["rawVec4"];
				for (uint32_t idx = 0; idx < kCb2Vec4Count; ++idx)
				{
					const std::string key = "cb2[" + std::to_string(idx) + "]";
					if (raw.contains(key))
						AssignXMFloat4(matData.RawCb2[idx], raw[key]);
				}
			}
			else if (cb2Block.contains("decodedSemantics"))
			{
				PopulateCb2FallbackFromSemantics(cb2Block["decodedSemantics"], matData.RawCb2);
			}

			if (matData.RawCb2[30].x > 0.5f) matFlags |= kMatFlag_ShadowLightBias;
			if (matData.RawCb2[30].y > 0.5f) matFlags |= kMatFlag_RampEnable;
			if (matData.RawCb2[30].z > 0.5f) matFlags |= kMatFlag_UseSecondUV;
			if (matData.RawCb2[30].w > 0.5f) matFlags |= kMatFlag_SkinSHDCNormalize;
			if (meta.CullMode == CullModeId::Front && meta.ShadingModelId == ShadingModel::Transparent)
				matFlags |= kMatFlag_IsTransparentBack;

			// Per-Draw UE5 WorldMatrix + SH L2
			GFL2DrawData drawData{};
			drawData.WorldMatrix   = Math::Matrix4(Math::kIdentity);
			drawData.WorldIT       = Math::Matrix4(Math::kIdentity);
			drawData.MaterialIndex = static_cast<uint32_t>(m_CpuMaterials.size());
			drawData.SubmeshIndex  = meta.SubmeshIndex;
			drawData.EventId       = eventId;

			const auto& cb1 = matJson["cb1_perDraw"];
			if (cb1.contains("unity_ObjectToWorld_cols") && cb1["unity_ObjectToWorld_cols"].is_array())
			{
				float cols[16] = {};
				const auto& matArr = cb1["unity_ObjectToWorld_cols"];
				for (int col = 0; col < 4 && col < static_cast<int>(matArr.size()); ++col)
				{
					for (int row = 0; row < 4 && row < static_cast<int>(matArr[col].size()); ++row)
						cols[col * 4 + row] = matArr[col][row].get<float>();
				}
				drawData.WorldMatrix = MathConv::UnityColsToUE5RowMatrix(cols);
				drawData.WorldIT     = MathConv::ComputeRowNormalMatrix(drawData.WorldMatrix);
			}

			if (cb1.contains("sh_L2_cb1_17_23") && cb1["sh_L2_cb1_17_23"].is_array() && cb1["sh_L2_cb1_17_23"].size() == kSHVec4Count)
			{
				DirectX::XMFLOAT4 inSH[kSHVec4Count] = {};
				for (uint32_t s = 0; s < kSHVec4Count; ++s)
					AssignXMFloat4(inSH[s], cb1["sh_L2_cb1_17_23"][s]);
				MathConv::UnityToUE5SH_L2(inSH, drawData.SH_L2);
				drawData.Flags |= 1u;
				matFlags |= kMatFlag_HasValidSH;
			}

			matData.MaterialFlags = matFlags;

			m_CpuDraws.push_back(drawData);
			m_CpuMaterials.push_back(matData);
			m_DrawMeta.push_back(std::move(meta));
			m_Stats.TotalTriangles += dc.value("triangleCount", 0u);
		}

		if (m_CpuDraws.empty() || m_CpuMaterials.empty())
		{
			LOG_ERROR(L"[GFL2][M0] Loaded 0 DrawCalls from AK-12 manifest\n");
			Shutdown(&services);
			return false;
		}

		// 4. 鍒涘缓骞朵笂浼?StructuredBuffer -> Zone A SRV
		m_DrawDataBuffer.Create(
			L"GFL2_AK12_DrawData",
			static_cast<uint32_t>(m_CpuDraws.size()),
			sizeof(GFL2DrawData),
			m_CpuDraws.data());

		m_MaterialDataBuffer.Create(
			L"GFL2_AK12_MaterialData",
			static_cast<uint32_t>(m_CpuMaterials.size()),
			sizeof(GFL2MaterialData),
			m_CpuMaterials.data());

		const Renderer::BindlessAllocation drawSrvAlloc = heap.AllocPersistentSingle();
		const Renderer::BindlessAllocation matSrvAlloc  = heap.AllocPersistentSingle();
		if (!drawSrvAlloc.IsValid() || !matSrvAlloc.IsValid())
		{
			if (drawSrvAlloc.IsValid()) heap.FreePersistentSingle(drawSrvAlloc.HeapIndex, &services.GetReleaseQueue());
			if (matSrvAlloc.IsValid())  heap.FreePersistentSingle(matSrvAlloc.HeapIndex, &services.GetReleaseQueue());
			LOG_ERROR(L"[GFL2][M0] Zone A exhausted allocating SSBO descriptors\n");
			Shutdown(&services);
			return false;
		}

		Graphics::g_Device->CopyDescriptorsSimple(
			1, drawSrvAlloc.Cpu, m_DrawDataBuffer.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		Graphics::g_Device->CopyDescriptorsSimple(
			1, matSrvAlloc.Cpu, m_MaterialDataBuffer.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		m_DrawDataSrvIdx     = drawSrvAlloc.HeapIndex;
		m_MaterialDataSrvIdx = matSrvAlloc.HeapIndex;
		m_AllocatedZoneASlots.push_back(m_DrawDataSrvIdx);
		m_AllocatedZoneASlots.push_back(m_MaterialDataSrvIdx);

		m_Stats.Loaded               = true;
		m_Stats.DrawCallCount        = static_cast<uint32_t>(m_CpuDraws.size());
		m_Stats.DrawDataSrvIdx       = m_DrawDataSrvIdx;
		m_Stats.MaterialDataSrvIdx   = m_MaterialDataSrvIdx;

		if (!LoadAK12Geometry(ak12Root.wstring()))
		{
			LOG_ERROR(L"[GFL2][M1] AK-12 glTF geometry upload failed\n");
			Shutdown(&services);
			return false;
		}

		m_Stats.Loaded = true;

		LOG_INFO(L"[GFL2][M1] AK-12 loaded: draws=%u, submeshes=%u, verts=%u, indices=%u, tris=%u, textures=%u (sRGB=%u, Linear=%u), ZoneA=[DrawSSBO=%u, MatSSBO=%u]\n",
			m_Stats.DrawCallCount, m_Stats.UniqueSubmeshes, m_Stats.VertexCount, m_Stats.IndexCount, m_Stats.TotalTriangles,
			m_Stats.LoadedTextureCount, m_Stats.SrgbTextureCount, m_Stats.LinearTextureCount,
			m_DrawDataSrvIdx, m_MaterialDataSrvIdx);

		return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR(L"[GFL2][M0] Exception in CharacterManifestLoader::LoadAK12: %hs\n", e.what());
			Shutdown(&services);
			return false;
		}
	}

	bool CharacterManifestLoader::LoadAK12Geometry(const std::wstring& ak12Root)
	{
		const fs::path gltfPath = fs::path(ak12Root) / L"meshes" / L"AK12_full_assembled_LH_Unity.gltf";
		if (!fs::exists(gltfPath))
		{
			LOG_ERROR(L"[GFL2][M1] Missing assembled glTF: %ws\n", gltfPath.c_str());
			return false;
		}

		glTF::Asset asset(gltfPath.wstring());
		if (asset.m_meshes.empty())
		{
			LOG_ERROR(L"[GFL2][M1] Failed to parse meshes in %ws\n", gltfPath.c_str());
			return false;
		}

		auto ReadFloatAttrib = [](const glTF::Accessor* acc, uint32_t idx, float out[4], uint32_t maxComps)
		{
			for (uint32_t k = 0; k < maxComps; ++k)
				out[k] = 0.0f;
			if (acc == nullptr || acc->dataPtr == nullptr || idx >= acc->count)
				return;

			uint32_t compCount = 1u;
			switch (acc->type)
			{
			case glTF::Accessor::kScalar: compCount = 1u; break;
			case glTF::Accessor::kVec2:   compCount = 2u; break;
			case glTF::Accessor::kVec3:   compCount = 3u; break;
			default:                      compCount = 4u; break;
			}

			uint32_t compBytes = 4u;
			if (acc->componentType == glTF::Accessor::kUnsignedShort) compBytes = 2u;
			else if (acc->componentType == glTF::Accessor::kUnsignedByte) compBytes = 1u;

			const uint32_t stride = (acc->stride != 0u) ? acc->stride : (compBytes * compCount);
			const uint8_t* ptr = reinterpret_cast<const uint8_t*>(acc->dataPtr) + static_cast<size_t>(idx) * stride;

			const uint32_t n = std::min(compCount, maxComps);
			if (acc->componentType == glTF::Accessor::kFloat)
			{
				for (uint32_t c = 0; c < n; ++c)
					std::memcpy(&out[c], ptr + c * sizeof(float), sizeof(float));
			}
			else if (acc->componentType == glTF::Accessor::kUnsignedShort)
			{
				for (uint32_t c = 0; c < n; ++c)
				{
					uint16_t v = 0;
					std::memcpy(&v, ptr + c * sizeof(uint16_t), sizeof(uint16_t));
					out[c] = static_cast<float>(v) / 65535.0f;
				}
			}
			else if (acc->componentType == glTF::Accessor::kUnsignedByte)
			{
				for (uint32_t c = 0; c < n; ++c)
					out[c] = static_cast<float>(ptr[c]) / 255.0f;
			}
		};

		m_Submeshes.clear();
		m_Submeshes.reserve(asset.m_meshes.size());

		std::vector<GFL2CharacterVertex> allVertices;
		std::vector<uint32_t> allIndices;
		std::vector<std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>> subLocalBounds(asset.m_meshes.size(),
			{ { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX } });

		for (size_t si = 0; si < asset.m_meshes.size(); ++si)
		{
			const glTF::Mesh& mesh = asset.m_meshes[si];
			if (mesh.primitives.empty())
			{
				LOG_ERROR(L"[GFL2][M1] Submesh %zu has 0 primitives\n", si);
				return false;
			}

			const glTF::Primitive& prim = mesh.primitives[0];
			const glTF::Accessor* posAcc = prim.attributes[glTF::Primitive::kPosition];
			const glTF::Accessor* nrmAcc = prim.attributes[glTF::Primitive::kNormal];
			const glTF::Accessor* tanAcc = prim.attributes[glTF::Primitive::kTangent];
			const glTF::Accessor* uv0Acc = prim.attributes[glTF::Primitive::kTexcoord0];
			const glTF::Accessor* uv1Acc = prim.attributes[glTF::Primitive::kTexcoord1];
			const glTF::Accessor* colAcc = prim.attributes[glTF::Primitive::kColor0];
			const glTF::Accessor* idxAcc = prim.indices;

			if (posAcc == nullptr || posAcc->dataPtr == nullptr || idxAcc == nullptr || idxAcc->dataPtr == nullptr)
			{
				LOG_ERROR(L"[GFL2][M1] Submesh %zu missing POSITION or indices\n", si);
				return false;
			}

			const uint32_t baseVertex  = static_cast<uint32_t>(allVertices.size());
			const uint32_t startIndex  = static_cast<uint32_t>(allIndices.size());
			const uint32_t vertexCount = posAcc->count;
			const uint32_t indexCount  = idxAcc->count;

			allVertices.resize(static_cast<size_t>(baseVertex) + vertexCount);
			auto& lbounds = subLocalBounds[si];

			for (uint32_t v = 0; v < vertexCount; ++v)
			{
				float p[4] = {}, n[4] = {}, t[4] = { 1, 0, 0, 1 }, uv0[4] = {}, uv1[4] = {}, c0[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
				ReadFloatAttrib(posAcc, v, p, 3);
				if (nrmAcc != nullptr) ReadFloatAttrib(nrmAcc, v, n, 3); else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
				if (tanAcc != nullptr) ReadFloatAttrib(tanAcc, v, t, 4);
				if (uv0Acc != nullptr) ReadFloatAttrib(uv0Acc, v, uv0, 2);
				if (uv1Acc != nullptr) ReadFloatAttrib(uv1Acc, v, uv1, 2);
				if (colAcc != nullptr)
				{
					ReadFloatAttrib(colAcc, v, c0, 4);
					if (colAcc->type == glTF::Accessor::kVec3) c0[3] = 1.0f;
				}

				GFL2CharacterVertex& dst = allVertices[static_cast<size_t>(baseVertex) + v];
				// UE5 LH Z-Up 循环置换: (x_ue, y_ue, z_ue) = (z_u, x_u, y_u)，保持 tangent.w 不翻号
				dst.Position = MathConv::UnityToUE5Vec3(p[0], p[1], p[2]);

				DirectX::XMFLOAT3 nUE = MathConv::UnityToUE5Vec3(n[0], n[1], n[2]);
				const float nLen = std::sqrt(nUE.x * nUE.x + nUE.y * nUE.y + nUE.z * nUE.z);
				if (nLen > 1e-6f)
				{
					nUE.x /= nLen; nUE.y /= nLen; nUE.z /= nLen;
				}
				else
				{
					nUE = { 0.0f, 0.0f, 1.0f };
				}
				dst.Normal = nUE;

				const DirectX::XMFLOAT3 tUE = MathConv::UnityToUE5Vec3(t[0], t[1], t[2]);
				dst.Tangent = DirectX::XMFLOAT4(tUE.x, tUE.y, tUE.z, (t[3] >= 0.0f) ? 1.0f : -1.0f);
				dst.UV0     = DirectX::XMFLOAT2(uv0[0], uv0[1]);
				dst.UV1     = DirectX::XMFLOAT2(uv1[0], uv1[1]);
				dst.Color0  = DirectX::XMFLOAT4(c0[0], c0[1], c0[2], c0[3]);

				lbounds.first.x  = std::min(lbounds.first.x,  dst.Position.x);
				lbounds.first.y  = std::min(lbounds.first.y,  dst.Position.y);
				lbounds.first.z  = std::min(lbounds.first.z,  dst.Position.z);
				lbounds.second.x = std::max(lbounds.second.x, dst.Position.x);
				lbounds.second.y = std::max(lbounds.second.y, dst.Position.y);
				lbounds.second.z = std::max(lbounds.second.z, dst.Position.z);
			}

			// 璇诲彇绱㈠紩骞舵寜 UE5 FrontCounterClockwise=TRUE 瀵硅皟涓夎褰㈢粫搴?(i0, i1, i2) -> (i0, i2, i1)
			const uint32_t validIdxCount = (indexCount / 3u) * 3u;
			allIndices.resize(static_cast<size_t>(startIndex) + validIdxCount);
			const uint8_t* idxRaw = reinterpret_cast<const uint8_t*>(idxAcc->dataPtr);
			const uint32_t idxElemSize = (idxAcc->componentType == glTF::Accessor::kUnsignedInt) ? 4u : 2u;
			const uint32_t idxStride = (idxAcc->stride != 0u) ? idxAcc->stride : idxElemSize;

			auto ReadIdx = [&](uint32_t k) -> uint32_t
			{
				const uint8_t* p = idxRaw + static_cast<size_t>(k) * idxStride;
				if (idxAcc->componentType == glTF::Accessor::kUnsignedInt)
				{
					uint32_t v = 0;
					std::memcpy(&v, p, sizeof(uint32_t));
					return std::min(v, vertexCount - 1u);
				}
				uint16_t v = 0;
				std::memcpy(&v, p, sizeof(uint16_t));
				return std::min<uint32_t>(v, vertexCount - 1u);
			};

			for (uint32_t t = 0; t + 2u < validIdxCount; t += 3u)
			{
				const uint32_t i0 = ReadIdx(t + 0u);
				const uint32_t i1 = ReadIdx(t + 1u);
				const uint32_t i2 = ReadIdx(t + 2u);
				allIndices[static_cast<size_t>(startIndex) + t + 0u] = i0;
				allIndices[static_cast<size_t>(startIndex) + t + 1u] = i2;
				allIndices[static_cast<size_t>(startIndex) + t + 2u] = i1;
			}

			SubmeshRange sr;
			sr.SubmeshIndex  = static_cast<uint32_t>(si);
			sr.BaseVertex    = baseVertex;
			sr.StartIndex    = startIndex;
			sr.IndexCount    = validIdxCount;
			sr.VertexCount   = vertexCount;
			sr.TriangleCount = validIdxCount / 3u;
			m_Submeshes.push_back(sr);
		}

		// 璁＄畻瑙掕壊鍦?UE5 涓栫晫绌洪棿涓嬬殑鎬诲寘鍥寸洅
		DirectX::XMFLOAT3 wmin = { FLT_MAX, FLT_MAX, FLT_MAX };
		DirectX::XMFLOAT3 wmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const auto& d : m_CpuDraws)
		{
			if (d.SubmeshIndex >= subLocalBounds.size())
				continue;
			const auto& lb = subLocalBounds[d.SubmeshIndex];
			const DirectX::XMMATRIX W = static_cast<DirectX::XMMATRIX>(d.WorldMatrix);
			for (int cx = 0; cx < 2; ++cx)
			for (int cy = 0; cy < 2; ++cy)
			for (int cz = 0; cz < 2; ++cz)
			{
				const DirectX::XMVECTOR pt = DirectX::XMVectorSet(
					cx ? lb.second.x : lb.first.x,
					cy ? lb.second.y : lb.first.y,
					cz ? lb.second.z : lb.first.z,
					1.0f);
				const DirectX::XMVECTOR ws = DirectX::XMVector4Transform(pt, W);
				DirectX::XMFLOAT3 f;
				DirectX::XMStoreFloat3(&f, ws);
				wmin.x = std::min(wmin.x, f.x); wmin.y = std::min(wmin.y, f.y); wmin.z = std::min(wmin.z, f.z);
				wmax.x = std::max(wmax.x, f.x); wmax.y = std::max(wmax.y, f.y); wmax.z = std::max(wmax.z, f.z);
			}
		}

		m_VertexBuffer.Create(
			L"GFL2_AK12_UnifiedVB",
			static_cast<uint32_t>(allVertices.size()),
			sizeof(GFL2CharacterVertex),
			allVertices.data());
		m_IndexBuffer.Create(
			L"GFL2_AK12_UnifiedIB",
			static_cast<uint32_t>(allIndices.size()),
			sizeof(uint32_t),
			allIndices.data());

		m_Stats.VertexCount    = static_cast<uint32_t>(allVertices.size());
		m_Stats.IndexCount     = static_cast<uint32_t>(allIndices.size());
		m_Stats.WorldBoundsMin = wmin;
		m_Stats.WorldBoundsMax = wmax;
		m_Stats.MeshLoaded     = true;
		return true;
	}

	void CharacterManifestLoader::ReleaseZoneADescriptors(
		Renderer::UnifiedDescriptorHeap* heap,
		Graphics::DeferredReleaseQueue* releaseQueue)
	{
		if (heap != nullptr && heap->IsInitialized())
		{
			for (const uint32_t slot : m_AllocatedZoneASlots)
				heap->FreePersistentSingle(slot, releaseQueue);
		}
		m_AllocatedZoneASlots.clear();
		m_DrawDataSrvIdx     = kInvalidBindlessIdx;
		m_MaterialDataSrvIdx = kInvalidBindlessIdx;
	}

	void CharacterManifestLoader::Shutdown(Renderer::RenderServices* services)
	{
		if (services != nullptr)
			ReleaseZoneADescriptors(&services->GetDescriptorHeap(), &services->GetReleaseQueue());
		else
			ReleaseZoneADescriptors(nullptr, nullptr);

		m_VertexBuffer.Destroy();
		m_IndexBuffer.Destroy();
		m_DrawDataBuffer.Destroy();
		m_MaterialDataBuffer.Destroy();
		m_Submeshes.clear();
		m_Textures.clear();
		m_TexIndexByResourceId.clear();
		m_CpuDraws.clear();
		m_CpuMaterials.clear();
		m_DrawMeta.clear();
		m_Stats = {};
	}
}
