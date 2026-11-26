#include "00_Core/PCH.h"
#include "03_AssetSystem/Loaders/GFL2/EnvSceneLoader.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "03_AssetSystem/Assets/Importers/glTF.h"
#include "03_AssetSystem/Assets/Importers/json.hpp"
#include "03_AssetSystem/Loaders/GFL2/GFL2MathConvert.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
		std::wstring Utf8ToWideEnv(const std::string& utf8)
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

		std::string SanitizeJsonNonFiniteEnv(const std::string& src)
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
				if (c == '"') { inString = true; out.push_back(c); ++i; continue; }
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

		bool ReadJsonFileEnv(const fs::path& filePath, json& outJson)
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

			if (raw.size() >= 3 &&
				static_cast<unsigned char>(raw[0]) == 0xEF &&
				static_cast<unsigned char>(raw[1]) == 0xBB &&
				static_cast<unsigned char>(raw[2]) == 0xBF)
			{
				raw.erase(0, 3);
			}

			const std::string sanitized = SanitizeJsonNonFiniteEnv(raw);
			outJson = json::parse(sanitized, nullptr, /*allow_exceptions=*/false);
			if (outJson.is_discarded())
			{
				LOG_ERROR(L"[GFL2][M6] JSON parse error in %ws\n", filePath.c_str());
				return false;
			}
			return true;
		}

		bool TryResolveAssetRootFromAncestor(const fs::path& start, int maxDepth, fs::path& outDir)
		{
			// docs/AI/灏戝墠2澶嶅埢/璧勪骇
			static const wchar_t kRelWide[] = L"docs\\AI\\\u5c11\u524d2\u590d\u523b\\\u8d44\u4ea7";
			std::error_code ec;
			fs::path cur = start;
			for (int i = 0; i < maxDepth && !cur.empty(); ++i)
			{
				const fs::path cand = cur / kRelWide;
				if (fs::is_regular_file(cand / L"\u73af\u5883\u573a\u666f" / L"env_manifest.json", ec))
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

		void ReadXMFloat4Safe(const json& j, DirectX::XMFLOAT4& dst)
		{
			if (!j.is_array())
				return;
			float v[4] = {};
			for (size_t i = 0; i < 4 && i < j.size(); ++i)
			{
				if (j[i].is_number())
				{
					const float f = j[i].get<float>();
					v[i] = std::isfinite(f) ? f : 0.0f;
				}
			}
			dst = DirectX::XMFLOAT4(v[0], v[1], v[2], v[3]);
		}
	}

	std::wstring EnvSceneLoader::ResolveAssetRoot()
	{
		std::wstring buf(MAX_PATH, L'\0');
		DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
		if (n > 0)
		{
			buf.resize(n);
			fs::path found;
			if (TryResolveAssetRootFromAncestor(fs::path(buf).parent_path(), 8, found))
				return found.wstring();
		}
		std::error_code ec;
		fs::path cwd = fs::current_path(ec);
		if (!cwd.empty())
		{
			fs::path found;
			if (TryResolveAssetRootFromAncestor(cwd, 8, found))
				return found.wstring();
		}
#ifdef TOOIYUME_SOURCE_ROOT_DIR
		{
			fs::path found;
			if (TryResolveAssetRootFromAncestor(fs::path(TOOIYUME_SOURCE_ROOT_DIR), 2, found))
				return found.wstring();
		}
#endif
		return {};
	}

	uint32_t EnvSceneLoader::FindTextureBindlessIdx(uint32_t resourceId) const
	{
		auto it = m_TexIndexByResourceId.find(resourceId);
		if (it == m_TexIndexByResourceId.end())
			return kInvalidBindlessIdx;
		return m_Textures[it->second].BindlessIdx;
	}

	bool EnvSceneLoader::LoadEnv(Renderer::RenderServices& services, uint32_t reflectionProbe5682SrvIdx)
	{
		if (m_Stats.Loaded)
			return true;

		if (Graphics::g_Device == nullptr || !services.IsInitialized())
			return false;

		const std::wstring assetRootStr = ResolveAssetRoot();
		if (assetRootStr.empty())
		{
			LOG_ERROR(L"[GFL2][M6] Failed to locate docs/AI/灏戝墠2澶嶅埢/璧勪骇/鐜鍦烘櫙/env_manifest.json\n");
			return false;
		}

		const fs::path assetRoot(assetRootStr);
		const fs::path envRoot = assetRoot / L"\u73af\u5883\u573a\u666f";
		const fs::path ppRoot  = assetRoot / L"\u540e\u5904\u7406";

		json manifest;
		if (!ReadJsonFileEnv(envRoot / L"env_manifest.json", manifest))
		{
			LOG_ERROR(L"[GFL2][M6] Failed to parse env_manifest.json\n");
			return false;
		}

		Renderer::UnifiedDescriptorHeap& heap = services.GetDescriptorHeap();
		if (!heap.IsInitialized())
			return false;

		try
		{
			m_Stats = {};
			m_Stats.AssetDir = envRoot.wstring();

			auto LoadAndRegisterTex = [&](uint32_t resId, const fs::path& fullPath, bool isSRGB, const std::string& role) -> uint32_t
			{
				if (uint32_t existing = FindTextureBindlessIdx(resId); existing != kInvalidBindlessIdx)
					return existing;
				TextureRef texRef = TextureManager::LoadDDSFromFile(fullPath.wstring(), Graphics::kMagenta2D, isSRGB);
				if (!texRef.IsValid())
					return kInvalidBindlessIdx;

				const Renderer::BindlessAllocation slot = heap.AllocPersistentSingle();
				if (!slot.IsValid())
					return kInvalidBindlessIdx;
				Graphics::g_Device->CopyDescriptorsSimple(
					1, slot.Cpu, texRef.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				m_AllocatedZoneASlots.push_back(slot.HeapIndex);

				LoadedTextureSlot entry;
				entry.ResourceId  = resId;
				entry.BindlessIdx = slot.HeapIndex;
				entry.IsSRGB      = isSRGB;
				entry.Role        = role;
				entry.FileName    = fullPath.filename().wstring();
				entry.Texture     = texRef;

				m_TexIndexByResourceId[resId] = m_Textures.size();
				m_Textures.push_back(std::move(entry));
				++m_Stats.LoadedTextureCount;
				return slot.HeapIndex;
			};

			// 1. 杞藉叆鐜璐村浘 (sceneTextures)
			if (manifest.contains("sceneTextures") && manifest["sceneTextures"].is_array())
			{
				for (const auto& texObj : manifest["sceneTextures"])
				{
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
					LoadAndRegisterTex(resId, envRoot / Utf8ToWideEnv(ddsRel), isSRGB, texObj.value("name", ""));
				}
			}

			// 2. 杞藉叆 7281 棰勭儤鐒?3D LUT (.dds, Linear)
			const fs::path lutPath = ppRoot / L"textures" / L"7281_Baked3DLUT_1024x32_Linear.dds";
			m_Stats.Lut3DSrvIdx      = LoadAndRegisterTex(7281u, lutPath, false, "Baked3DLUT");
			m_Stats.SkyboxTexSrvIdx  = FindTextureBindlessIdx(5687u);
			m_Stats.LightmapSrvIdx   = FindTextureBindlessIdx(5685u);
			m_Stats.ShadowMaskSrvIdx = FindTextureBindlessIdx(5686u);

			// 3. meshId ("M01") -> meshIndex 鏄犲皠
			std::unordered_map<std::string, uint32_t> meshIdToIdx;
			if (manifest.contains("uniqueMeshes") && manifest["uniqueMeshes"].is_array())
			{
				m_Stats.UniqueSubmeshes = static_cast<uint32_t>(manifest["uniqueMeshes"].size());
				for (const auto& um : manifest["uniqueMeshes"])
				{
					const std::string mid = um.value("meshId", "");
					const uint32_t idx = um.value("meshIndex", 0u);
					if (!mid.empty())
						meshIdToIdx[mid] = idx;
				}
			}

			// 4. 瑙ｆ瀽 draws (SceneOpaque_4xMSAA + Skybox_4xMSAA + FloorPlanarReflection)
			if (!manifest.contains("draws") || !manifest["draws"].is_array())
			{
				Shutdown(&services);
				return false;
			}

			const auto& drawsJson = manifest["draws"];
			m_CpuDraws.reserve(drawsJson.size());
			m_CpuMaterials.reserve(drawsJson.size());
			m_DrawMeta.reserve(drawsJson.size());

			for (const auto& dc : drawsJson)
			{
				const std::string passName = dc.value("pass", "");
				EnvDrawCategory cat = EnvDrawCategory::OtherFX;
				if (passName == "SceneOpaque_4xMSAA")          cat = EnvDrawCategory::Opaque;
				else if (passName == "Skybox_4xMSAA")          cat = EnvDrawCategory::Skybox;
				else if (passName == "FloorPlanarReflection")  cat = EnvDrawCategory::FloorMirror;
				else continue;

				const uint32_t eid  = dc.value("eventId", 0u);
				// EID 2574..2698 carry AK-12 rig bone transforms near origin with 5m/8m pillar/catwalk geometry
				// that are occluded/excluded in reference EID4739 and otherwise encase AK-12 and block all cameras.
				if (eid >= 2574u && eid <= 2698u)
					continue;
				const uint32_t fsId = dc.value("fsId", 0u);
				const std::string matRel = dc.value("materialFile", "");
				if (matRel.empty())
					continue;

				json matJson;
				if (!ReadJsonFileEnv(envRoot / Utf8ToWideEnv(matRel), matJson))
					continue;

				EnvDrawMeta meta;
				meta.EventId      = eid;
				meta.FragShaderId = fsId;
				meta.Category     = cat;
				meta.IsDecal      = (fsId == 8175u);
				if (auto it = meshIdToIdx.find(dc.value("meshId", "")); it != meshIdToIdx.end())
					meta.SubmeshIndex = it->second;

				const std::string cullStr = matJson.contains("renderState")
					? matJson["renderState"].value("cullMode", "CullMode.Back")
					: "CullMode.Back";
				if (cullStr.find("Front") != std::string::npos) meta.CullMode = CullModeId::Front;
				else if (cullStr.find("NoCull") != std::string::npos || cullStr.find("None") != std::string::npos) meta.CullMode = CullModeId::None;
				else meta.CullMode = CullModeId::Back;

				GFL2MaterialData matData{};
				matData.BaseColorIdx       = Renderer::UnifiedDescriptorHeap::kSentinelWhite2D;
				matData.NormalIdx          = Renderer::UnifiedDescriptorHeap::kSentinelFlatNormal2D;
				matData.RmoOrSdfIdx        = Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
				matData.RampMapIdx         = (m_Stats.LightmapSrvIdx != kInvalidBindlessIdx) ? m_Stats.LightmapSrvIdx : Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
				matData.HairSpecAoIdx      = (m_Stats.ShadowMaskSrvIdx != kInvalidBindlessIdx) ? m_Stats.ShadowMaskSrvIdx : Renderer::UnifiedDescriptorHeap::kSentinelWhite2D;
				matData.EyeBlendIdx        = Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
				matData.ReflectionProbeIdx = (reflectionProbe5682SrvIdx != kInvalidBindlessIdx) ? reflectionProbe5682SrvIdx : Renderer::UnifiedDescriptorHeap::kSentinelBlack2D;
				matData.EventId            = eid;
				matData.ShadingModelId     = static_cast<uint32_t>(ShadingModel::Env);
				matData.CullMode           = static_cast<uint32_t>(meta.CullMode);

				// 分类匹配场景贴图（BaseColor *_d/_da, Normal *_n, SMOE *_smo/_smoe）
				if (matJson.contains("textures") && matJson["textures"].is_array())
				{
					for (const auto& t : matJson["textures"])
					{
						const uint32_t tid = t.value("resourceId", 0u);
						if (tid == 5682u || tid == 5685u || tid == 5686u || tid == 9068u || tid == 7838u ||
							tid == 57u || tid == 59u || tid == 67u || tid == 79u)
							continue;
						const uint32_t bIdx = FindTextureBindlessIdx(tid);
						if (bIdx == kInvalidBindlessIdx)
							continue;
						std::string nameLower = t.value("name", "");
						for (char& ch : nameLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

						if ((nameLower.size() >= 2 && nameLower.rfind("_n") == nameLower.size() - 2) || nameLower.find("_n_") != std::string::npos)
							matData.NormalIdx = bIdx;
						else if (nameLower.find("_smoe") != std::string::npos || nameLower.find("_smo") != std::string::npos)
							matData.RmoOrSdfIdx = bIdx;
						else
							matData.BaseColorIdx = bIdx;
					}
				}

				uint32_t envFlags = kEnvFlag_None;
				if (meta.IsDecal)   envFlags |= kEnvFlag_AlphaCutout;
				if (fsId == 7295u)  envFlags |= kEnvFlag_IsEmissive;
				if (fsId == 8173u)  envFlags |= kEnvFlag_IsPlatform;

				// 榛樿鍙傛暟
				matData.RawCb2[0] = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f); // lmST
				matData.RawCb2[1] = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f); // baseMapST
				matData.RawCb2[2] = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); // baseColorTint
				matData.RawCb2[3] = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f); // emissiveColor
				matData.RawCb2[6] = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); // normal/metal/smooth/ao scales
				matData.RawCb2[7] = DirectX::XMFLOAT4(0.0f, 0.5f, 0.0f, 0.0f); // emissiveMult, cutoff

				if (matJson.contains("cb2_perMaterial_fragment") && matJson["cb2_perMaterial_fragment"].is_array())
				{
					const auto& cb2 = matJson["cb2_perMaterial_fragment"];
					if (cb2.size() > 1) ReadXMFloat4Safe(cb2[1], matData.RawCb2[1]);
					if (cb2.size() > 2) ReadXMFloat4Safe(cb2[2], matData.RawCb2[2]);
					if (cb2.size() > 3) ReadXMFloat4Safe(cb2[3], matData.RawCb2[3]);
					if (cb2.size() > 6) ReadXMFloat4Safe(cb2[6], matData.RawCb2[6]);
					if (cb2.size() > 7) ReadXMFloat4Safe(cb2[7], matData.RawCb2[7]);
				}
				else if (matJson.contains("cb2_perMaterial_vertex") && matJson["cb2_perMaterial_vertex"].is_array())
				{
					const auto& cb2 = matJson["cb2_perMaterial_vertex"];
					if (cb2.size() > 1) ReadXMFloat4Safe(cb2[1], matData.RawCb2[1]);
					if (cb2.size() > 2) ReadXMFloat4Safe(cb2[2], matData.RawCb2[2]);
				}

				GFL2DrawData drawData{};
				drawData.WorldMatrix   = Math::Matrix4(Math::kIdentity);
				drawData.WorldIT       = Math::Matrix4(Math::kIdentity);
				drawData.MaterialIndex = static_cast<uint32_t>(m_CpuMaterials.size());
				drawData.SubmeshIndex  = meta.SubmeshIndex;
				drawData.EventId       = eid;

				if (matJson.contains("worldMatrixColMajor") && matJson["worldMatrixColMajor"].is_array() &&
					matJson["worldMatrixColMajor"].size() == 16)
				{
					float cols[16] = {};
					for (int i = 0; i < 16; ++i)
						cols[i] = matJson["worldMatrixColMajor"][i].get<float>();
					drawData.WorldMatrix = MathConv::UnityColsToUE5RowMatrix(cols);
					drawData.WorldIT     = MathConv::ComputeRowNormalMatrix(drawData.WorldMatrix);
				}

				if (matJson.contains("cb1_perDraw_vertex") && matJson["cb1_perDraw_vertex"].is_object())
				{
					const auto& cb1 = matJson["cb1_perDraw_vertex"];
					if (cb1.contains("unity_LightmapST_cb1_15") && cb1["unity_LightmapST_cb1_15"].is_array())
					{
						DirectX::XMFLOAT4 lmST{};
						ReadXMFloat4Safe(cb1["unity_LightmapST_cb1_15"], lmST);
						if (std::fabs(lmST.x) > 1e-6f && std::fabs(lmST.y) > 1e-6f && m_Stats.LightmapSrvIdx != kInvalidBindlessIdx)
						{
							matData.RawCb2[0] = lmST;
							envFlags |= kEnvFlag_HasLightmap;
						}
					}
					if (cb1.contains("sh_L2_cb1_17_23") && cb1["sh_L2_cb1_17_23"].is_array() && cb1["sh_L2_cb1_17_23"].size() == kSHVec4Count)
					{
						DirectX::XMFLOAT4 inSH[kSHVec4Count] = {};
						for (uint32_t s = 0; s < kSHVec4Count; ++s)
							ReadXMFloat4Safe(cb1["sh_L2_cb1_17_23"][s], inSH[s]);
						MathConv::UnityToUE5SH_L2(inSH, drawData.SH_L2);
						drawData.Flags |= 1u;
					}
				}

				matData.MaterialFlags = envFlags;

				m_CpuDraws.push_back(drawData);
				m_CpuMaterials.push_back(matData);
				m_DrawMeta.push_back(meta);
				if (cat == EnvDrawCategory::Opaque)
					++m_Stats.OpaqueDrawCount;
			}

			if (m_CpuDraws.empty())
			{
				Shutdown(&services);
				return false;
			}

			// 5. 涓婁紶 DrawData / MaterialData SSBO -> Zone A
			m_DrawDataBuffer.Create(
				L"GFL2_Env_DrawData",
				static_cast<uint32_t>(m_CpuDraws.size()),
				sizeof(GFL2DrawData),
				m_CpuDraws.data());
			m_MaterialDataBuffer.Create(
				L"GFL2_Env_MaterialData",
				static_cast<uint32_t>(m_CpuMaterials.size()),
				sizeof(GFL2MaterialData),
				m_CpuMaterials.data());

			const Renderer::BindlessAllocation drawAlloc = heap.AllocPersistentSingle();
			const Renderer::BindlessAllocation matAlloc  = heap.AllocPersistentSingle();
			if (!drawAlloc.IsValid() || !matAlloc.IsValid())
			{
				if (drawAlloc.IsValid()) heap.FreePersistentSingle(drawAlloc.HeapIndex, &services.GetReleaseQueue());
				if (matAlloc.IsValid())  heap.FreePersistentSingle(matAlloc.HeapIndex, &services.GetReleaseQueue());
				Shutdown(&services);
				return false;
			}

			Graphics::g_Device->CopyDescriptorsSimple(
				1, drawAlloc.Cpu, m_DrawDataBuffer.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			Graphics::g_Device->CopyDescriptorsSimple(
				1, matAlloc.Cpu, m_MaterialDataBuffer.GetSRV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			m_Stats.DrawDataSrvIdx     = drawAlloc.HeapIndex;
			m_Stats.MaterialDataSrvIdx = matAlloc.HeapIndex;
			m_AllocatedZoneASlots.push_back(m_Stats.DrawDataSrvIdx);
			m_AllocatedZoneASlots.push_back(m_Stats.MaterialDataSrvIdx);

			if (!LoadEnvGeometry(envRoot.wstring()))
			{
				Shutdown(&services);
				return false;
			}

			LoadAdditionalLights(assetRoot.wstring());
			m_Stats.Loaded = true;

			LOG_INFO(L"[GFL2][M6] EnvScene loaded: opaqueDraws=%u, totalDraws=%zu, submeshes=%u, verts=%u, tex=%u, LUT3D=%u\n",
				m_Stats.OpaqueDrawCount, m_CpuDraws.size(), m_Stats.UniqueSubmeshes, m_Stats.VertexCount,
				m_Stats.LoadedTextureCount, m_Stats.Lut3DSrvIdx);
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR(L"[GFL2][M6] Exception in EnvSceneLoader::LoadEnv: %hs\n", e.what());
			Shutdown(&services);
			return false;
		}
	}

	bool EnvSceneLoader::LoadEnvGeometry(const std::wstring& envRoot)
	{
		const fs::path gltfPath = fs::path(envRoot) / L"meshes" / L"Scene_full_assembled_LH_Unity.gltf";
		if (!fs::exists(gltfPath))
			return false;

		glTF::Asset asset(gltfPath.wstring());
		if (asset.m_meshes.empty())
			return false;

		auto ReadFloatAttrib = [](const glTF::Accessor* acc, uint32_t idx, float out[4], uint32_t maxComps)
		{
			for (uint32_t k = 0; k < maxComps; ++k) out[k] = 0.0f;
			if (acc == nullptr || acc->dataPtr == nullptr || idx >= acc->count) return;
			uint32_t compCount = 1u;
			if (acc->type == glTF::Accessor::kVec2) compCount = 2u;
			else if (acc->type == glTF::Accessor::kVec3) compCount = 3u;
			else if (acc->type >= glTF::Accessor::kVec4) compCount = 4u;

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
					uint16_t v = 0; std::memcpy(&v, ptr + c * sizeof(uint16_t), sizeof(uint16_t));
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

		for (size_t si = 0; si < asset.m_meshes.size(); ++si)
		{
			const glTF::Mesh& mesh = asset.m_meshes[si];
			if (mesh.primitives.empty())
				return false;

			const glTF::Primitive& prim = mesh.primitives[0];
			const glTF::Accessor* posAcc = prim.attributes[glTF::Primitive::kPosition];
			const glTF::Accessor* nrmAcc = prim.attributes[glTF::Primitive::kNormal];
			const glTF::Accessor* tanAcc = prim.attributes[glTF::Primitive::kTangent];
			const glTF::Accessor* uv0Acc = prim.attributes[glTF::Primitive::kTexcoord0];
			const glTF::Accessor* uv1Acc = prim.attributes[glTF::Primitive::kTexcoord1];
			const glTF::Accessor* colAcc = prim.attributes[glTF::Primitive::kColor0];
			const glTF::Accessor* idxAcc = prim.indices;
			if (posAcc == nullptr || posAcc->dataPtr == nullptr || idxAcc == nullptr || idxAcc->dataPtr == nullptr)
				return false;

			const uint32_t baseVertex  = static_cast<uint32_t>(allVertices.size());
			const uint32_t startIndex  = static_cast<uint32_t>(allIndices.size());
			const uint32_t vertexCount = posAcc->count;
			const uint32_t indexCount  = idxAcc->count;

			allVertices.resize(static_cast<size_t>(baseVertex) + vertexCount);
			for (uint32_t v = 0; v < vertexCount; ++v)
			{
				float p[4] = {}, n[4] = {}, t[4] = { 1, 0, 0, 1 }, uv0[4] = {}, uv1[4] = {}, c0[4] = { 1, 1, 1, 1 };
				ReadFloatAttrib(posAcc, v, p, 3);
				if (nrmAcc != nullptr) ReadFloatAttrib(nrmAcc, v, n, 3); else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
				if (tanAcc != nullptr) ReadFloatAttrib(tanAcc, v, t, 4);
				if (uv0Acc != nullptr) ReadFloatAttrib(uv0Acc, v, uv0, 2);
				if (uv1Acc != nullptr) ReadFloatAttrib(uv1Acc, v, uv1, 2);
				if (colAcc != nullptr) ReadFloatAttrib(colAcc, v, c0, 4);

				GFL2CharacterVertex& dst = allVertices[static_cast<size_t>(baseVertex) + v];
				dst.Position = MathConv::UnityToUE5Vec3(p[0], p[1], p[2]);
				DirectX::XMFLOAT3 nUE = MathConv::UnityToUE5Vec3(n[0], n[1], n[2]);
				const float nLen = std::sqrt(nUE.x * nUE.x + nUE.y * nUE.y + nUE.z * nUE.z);
				if (nLen > 1e-6f) { nUE.x /= nLen; nUE.y /= nLen; nUE.z /= nLen; }
				else nUE = { 0.0f, 0.0f, 1.0f };
				dst.Normal = nUE;
				const DirectX::XMFLOAT3 tUE = MathConv::UnityToUE5Vec3(t[0], t[1], t[2]);
				dst.Tangent = DirectX::XMFLOAT4(tUE.x, tUE.y, tUE.z, (t[3] >= 0.0f) ? 1.0f : -1.0f);
				dst.UV0     = DirectX::XMFLOAT2(uv0[0], uv0[1]);
				dst.UV1     = DirectX::XMFLOAT2(uv1[0], uv1[1]);
				dst.Color0  = DirectX::XMFLOAT4(c0[0], c0[1], c0[2], c0[3]);
			}

			const uint32_t validIdxCount = (indexCount / 3u) * 3u;
			allIndices.resize(static_cast<size_t>(startIndex) + validIdxCount);
			const uint8_t* idxRaw = reinterpret_cast<const uint8_t*>(idxAcc->dataPtr);
			const uint32_t idxElemSize = (idxAcc->componentType == glTF::Accessor::kUnsignedInt) ? 4u : 2u;
			const uint32_t idxStride = (idxAcc->stride != 0u) ? idxAcc->stride : idxElemSize;

			auto ReadIdx = [&](uint32_t k) -> uint32_t
			{
				const uint8_t* ptr = idxRaw + static_cast<size_t>(k) * idxStride;
				if (idxAcc->componentType == glTF::Accessor::kUnsignedInt)
				{
					uint32_t val = 0; std::memcpy(&val, ptr, sizeof(uint32_t));
					return std::min(val, vertexCount - 1u);
				}
				uint16_t val = 0; std::memcpy(&val, ptr, sizeof(uint16_t));
				return std::min<uint32_t>(val, vertexCount - 1u);
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

		m_VertexBuffer.Create(L"GFL2_Env_UnifiedVB", static_cast<uint32_t>(allVertices.size()), sizeof(GFL2CharacterVertex), allVertices.data());
		m_IndexBuffer.Create(L"GFL2_Env_UnifiedIB", static_cast<uint32_t>(allIndices.size()), sizeof(uint32_t), allIndices.data());

		m_Stats.VertexCount = static_cast<uint32_t>(allVertices.size());
		m_Stats.IndexCount  = static_cast<uint32_t>(allIndices.size());
		m_Stats.MeshLoaded  = true;
		return true;
	}

	bool EnvSceneLoader::LoadAdditionalLights(const std::wstring& assetRoot)
	{
		m_AddLightsCB = {};
		m_Light1ShadowViewProj = Math::Matrix4(Math::kIdentity);

		const fs::path addlPath = fs::path(assetRoot) / L"\u5149\u7167" / L"additional_lights.json";
		json addl;
		if (!ReadJsonFileEnv(addlPath, addl))
			return false;

		if (addl.contains("shadowCastParams_EID1633VS") &&
			addl["shadowCastParams_EID1633VS"].contains("cb0_1345_1348_lightViewProj"))
		{
			const auto& rowsJson = addl["shadowCastParams_EID1633VS"]["cb0_1345_1348_lightViewProj"];
			if (rowsJson.is_array() && rowsJson.size() == 4)
			{
				float rows[16] = {};
				for (int r = 0; r < 4; ++r)
					for (int c = 0; c < 4 && c < static_cast<int>(rowsJson[r].size()); ++c)
						rows[r * 4 + c] = rowsJson[r][c].get<float>();
				m_Light1ShadowViewProj = MathConv::UnityWorldToClipRowsToUE5(rows);
			}
		}

		if (addl.contains("lights") && addl["lights"].is_array())
		{
			for (size_t i = 0; i < 2 && i < addl["lights"].size(); ++i)
			{
				const auto& lj = addl["lights"][i];
				auto& dst = m_AddLightsCB.Lights[i];

				DirectX::XMFLOAT4 p{}, col{}, att{}, dir{}, sh{};
				ReadXMFloat4Safe(lj["_AdditionalLightsPosition_cb0_10_plus_i"], p);
				ReadXMFloat4Safe(lj["_AdditionalLightsColor_cb0_266_plus_i"], col);
				ReadXMFloat4Safe(lj["_AdditionalLightsAttenuation_cb0_522_plus_i"], att);
				ReadXMFloat4Safe(lj["_AdditionalLightsSpotDir_cb0_778_plus_i"], dir);
				ReadXMFloat4Safe(lj["_AdditionalLightsShadowParams_cb0_2457_plus_i"], sh);

				const DirectX::XMFLOAT3 pUE = MathConv::UnityToUE5Vec3(p.x, p.y, p.z);
				const DirectX::XMFLOAT3 dUE = MathConv::UnityToUE5Vec3(dir.x, dir.y, dir.z);
				dst.PositionAndType = { pUE.x, pUE.y, pUE.z, p.w };
				dst.ColorAndRange   = col;
				dst.AttenParams     = att;
				dst.SpotDirUE5      = { dUE.x, dUE.y, dUE.z, dir.w };
				dst.ShadowParams    = sh;

				if (i == 1 && lj.contains("_AdditionalLightsWorldToShadow_slice_cb0_1433_plus_4i"))
				{
					const auto& sRowsJson = lj["_AdditionalLightsWorldToShadow_slice_cb0_1433_plus_4i"];
					if (sRowsJson.is_array() && sRowsJson.size() == 4)
					{
						float rows[16] = {};
						for (int r = 0; r < 4; ++r)
							for (int c = 0; c < 4 && c < static_cast<int>(sRowsJson[r].size()); ++c)
								rows[r * 4 + c] = sRowsJson[r][c].get<float>();
						m_AddLightsCB.Light1WorldToShadow = MathConv::UnityWorldToClipRowsToUE5(rows);
					}
				}
			}
			m_AddLightsCB.ControlParams = { 1.0f, 1.0f, 0.0f, 2.0f };
		}
		return true;
	}

	void EnvSceneLoader::Shutdown(Renderer::RenderServices* services)
	{
		if (services != nullptr)
		{
			auto& heap = services->GetDescriptorHeap();
			if (heap.IsInitialized())
			{
				for (const uint32_t slot : m_AllocatedZoneASlots)
					heap.FreePersistentSingle(slot, &services->GetReleaseQueue());
			}
		}
		m_AllocatedZoneASlots.clear();
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
