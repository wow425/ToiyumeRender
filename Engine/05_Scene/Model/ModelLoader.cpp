


#include "00_Core/PCH.h"
#include "05_Scene/Model/ModelLoader.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "05_Scene/Model/Model.h"
#include "03_AssetSystem/Assets/Importers/glTF.h"

#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureConvert.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"

#include <fstream>
#include <unordered_map>

using namespace Renderer;
using namespace Graphics;

// R3锛氶噰鏍峰櫒缁勫悎缂撳瓨鐨勪竴鏉¤褰曘€?
// 涓轰粈涔堜笉鑳藉彧瀛?offset锛歴_SamplerHeap 鏄函 bump 鍒嗛厤鍣紝涓や釜 Renderer 鐨?Shutdown 閮戒細
// 璋?s_SamplerHeap.Destroy()锛屼笅涓€杞?Initialize 鐨?Create() 鍙堟妸娓告爣褰掗浂銆備簬鏄笂涓€杞?
// 瀛樹笅鏉ョ殑鍋忕Щ鍦ㄦ柊鍫嗛噷銆岀宸с€嶄粛鐒惰惤鍦ㄥ悎娉曡寖鍥村唴锛屽嵈鎸囧悜浜嗗畬鍏ㄤ笉鍚岀殑閲囨牱鍣?鈥斺€?
// 涓嶄細宕╂簝銆佷笉浼氭姤閿欙紝鍙細璁╄创鍥鹃噰鏍锋ā寮忛潤榛橀敊鎺夛紙閲嶅/杈圭紭鎷変几/杩囨护涓嶅锛夛紝
// 鎺掓煡鎴愭湰鏋侀珮銆傛墍浠ョ紦瀛樺€奸噷甯︿笂鍫嗙殑浠ｉ檯鍙凤紝浠ｉ檯涓嶇涓€寰嬪綋鏈懡涓€侀噸鏂板垎閰嶃€?
struct SamplerCacheEntry
{
	uint32_t generation; // 鍒嗛厤鏃?Renderer::s_SamplerHeap 鐨勪唬闄咃紙DescriptorHeap::GetGeneration锛?
	uint32_t offset;     // 閲囨牱鍣ㄦ弿杩扮琛ㄥ湪鍫嗗唴鐨勫亸绉伙紙GetOffsetOfHandle 鐨勭粨鏋滐級
};

// 缂撳瓨宸插垱寤虹殑閲囨牱鍣ㄧ粍鍚堛€侹ey: 瀵诲潃妯″紡缁勫悎, Value: 鎻忚堪绗﹀爢鍋忕Щ + 浠ｉ檯鍙?
std::unordered_map<uint32_t, SamplerCacheEntry> g_SamplerPermutations;

// 鏍规嵁浣嶆帺鐮侊紙AddressModes锛夊姩鎬佽幏鍙栨垨鍒涘缓 D3D12 閲囨牱鍣ㄦ弿杩扮
D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(uint32_t addressModes)
{
	SamplerDesc samplerDesc;
	// 鍙栦綆 2 浣嶄綔涓?U 鏂瑰悜瀵诲潃妯″紡 (0: Wrap, 1: Mirror, 2: Clamp 绛?
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(addressModes & 0x3);
	// 鍙崇Щ 2 浣嶅彇鎺ヤ笅鏉ョ殑 2 浣嶄綔涓?V 鏂瑰悜瀵诲潃妯″紡
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(addressModes >> 2);
	// 鍦ㄩ潤鎬侀噰鏍峰櫒鍫嗘垨涓存椂鍫嗕腑鍒涘缓鎻忚堪绗﹀苟杩斿洖鍙ユ焺
	return samplerDesc.CreateDescriptor();
}

static uint32_t ConvertToRenderMaterialFlags(const Scene::Material::MaterialConstantData& material)
{
	uint32_t flags = Scene::Material::kMaterial_None;
	if ((material.flags & (1u << 7)) != 0)
		flags |= Scene::Material::kMaterial_AlphaBlend;
	if ((material.flags & (1u << 6)) != 0)
		flags |= Scene::Material::kMaterial_AlphaTest;
	if ((material.flags & (1u << 5)) != 0)
		flags |= Scene::Material::kMaterial_DoubleSided;
	return flags;
}

void LoadMaterials(Scene::Model::Model& model,
	const std::vector<Scene::Model::MaterialTextureData>& materialTextures,   // 鏉愯川绾圭悊鏁版嵁
	const std::vector<Scene::Material::MaterialConstantData>& materialConstants, // 鏉愯川鍙傛暟鏁版嵁
	const std::vector<std::wstring>& textureNames,              // 鏂囦欢鍚?
	const std::vector<uint8_t>& textureOptions,
	const std::wstring& basePath)                               // 璺緞
{
	static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

	// 1. Load textures鍔犺浇绾圭悊
	{
     const uint32_t numTextures = (uint32_t)textureNames.size();
		model.textures.resize(numTextures);
		for (size_t ti = 0; ti < numTextures; ++ti)
		{
            std::wstring originalFile = basePath + textureNames[ti];
			// 闃插尽鎬ф鏌ワ細纭繚 textureOptions 闀垮害瓒冲
			uint8_t opt = 0xFF;
			if (ti < textureOptions.size())
				opt = textureOptions[ti];
			else
				Utility::Printf("Warning: textureOptions missing entry for texture %zu, using default\n", ti);

			// 鍔ㄦ€佺紪璇戯細鑻ユ棤 DDS 鎴栧師濮嬫枃浠舵洿鏂帮紝鍒欒皟鐢ㄧ汗鐞嗚浆鎹㈠伐鍏风敓鎴愪紭鍖栬繃鐨?DDS
			CompileTextureOnDemand(originalFile, opt);

			std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";
			// 浠庣鐩樿浇鍏?GPU 璧勬簮
			model.textures[ti] = TextureManager::LoadDDSFromFile(ddsFile);
		}
	}

	// 鐢熸垚鎻忚堪绗﹁〃鍜岃褰曟潗璐ㄥ亸绉婚噺
	// [WP-1B / TD-01b] 浠庡疄渚嬬骇 RenderServices 鐨?4 鍒嗗尯鍗曞彲瑙佸爢锛圸one A + SAMPLER锛夊垎閰嶆潗璐ㄦ弿杩扮锛?
	// 褰诲簳瑙ｈ€?ModelLoader 涓庡叿浣?Renderer 鐨勭鏈夊爢鐢熷懡鍛ㄦ湡锛屽疄鐜板垏 Renderer 涓嶉噸杞芥ā鍨嬨€?
	Renderer::RenderServices* services = Renderer::RenderServices::GetActiveServices();
	ASSERT(services != nullptr && services->IsInitialized(),
		"ModelLoader::LoadMaterials 需要活跃的 RenderServices（UnifiedDescriptorHeap）");
	if (services == nullptr || !services->IsInitialized())
	{
		LOG_ERROR(L"[ModelLoader] LoadMaterials 澶辫触锛歊enderServices 鏈垵濮嬪寲\n");
		return;
	}
	Renderer::UnifiedDescriptorHeap& unifiedHeap = services->GetDescriptorHeap();
	const uint32_t samplerHeapGeneration = unifiedHeap.GetGeneration();
	std::vector<uint32_t> allocatedMaterialSlots;

	const uint32_t numMaterials = (uint32_t)materialTextures.size();
	model.m_Materials.resize(numMaterials);
	allocatedMaterialSlots.reserve(numMaterials);

	// 閬嶅巻涓烘瘡涓綉鏍艰祴浜堢汗鐞嗭紙鎸夌収PBR鏍囧噯鍒掑垎5涓汗鐞嗘Ы浣嶏紝鏃犲垯浣跨敤default濉厖)
	for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
	{
		const Scene::Model::MaterialTextureData& srcMat = materialTextures[matIdx];

		// 鍒嗛厤绾圭悊鎻忚堪绗﹁〃銆傛Ы浣嶉『搴忎弗鏍奸伒寰?ConstantBuffers.h 鐨勬潗璐ㄧ汗鐞嗘Ы浣?
		// 锛堝嵆 HLSL GBufferPS 鐨?t0..t4锛夛細
		//   t0 BaseColor 路 t1 MetallicRoughness 路 t2 Occlusion 路 t3 Emissive 路 t4 Normal
		const BindlessAllocation textureAlloc = unifiedHeap.AllocPersistentMaterialBlock(kNumTextures);
		if (!textureAlloc.IsValid())
		{
			LOG_ERROR(L"[ModelLoader] Zone A 5 妲芥潗璐ㄥ潡鍒嗛厤澶辫触锛坢atIdx=%u锛塡n", matIdx);
			return;
		}
		const uint32_t SRVDescriptorTable = textureAlloc.HeapIndex;
		allocatedMaterialSlots.push_back(SRVDescriptorTable);
		const D3D12_CPU_DESCRIPTOR_HANDLE destSrvHandle = textureAlloc.Cpu;

		uint32_t DestCount = kNumTextures;
		uint32_t SourceCounts[kNumTextures] = { 1, 1, 1, 1, 1 };

		// ============================================================
		// 榛樿锛堝崰浣嶏級绾圭悊 鈥斺€?R1.2
		// ------------------------------------------------------------
		// 杩欎簺绾圭悊鍐冲畾銆屾潗璐ㄦ病鏈夎璐村浘鏃讹紝Shader 閲囨牱鍒颁粈涔堛€嶃€傞€夐敊浼氱洿鎺ユ壄鏇?PBR锛?
		//   BaseColor         缂虹渷搴斾负鐧借壊 (1,1,1,1)锛氫箻娉曞崟浣嶅厓锛屼繚鐣?baseColorFactor 鍘熻壊
		//   MetallicRoughness 缂虹渷搴斾负 (1, 1, 1, 1)锛欸=roughness=1, B=metallic=1锛?
		//                     鍐嶄箻鏉愯川鐨?metallicFactor/roughnessFactor 鎵嶇瓑浜庢潗璐ㄥ€?
		//                     锛坓lTF 瑙勫畾锛氭棤 MR 璐村浘鏃?metallic/roughness 鍏ㄧ敱 factor 鍐冲畾锛?
		//   Occlusion         缂虹渷搴斾负鐧借壊 (1,1,1,1)锛欰O=1 琛ㄧず瀹屽叏涓嶅彈閬斀
		//   Emissive          缂虹渷搴斾负榛戣壊 (0,0,0,1)锛氭棤鑷彂鍏?
		//   Normal            缂虹渷搴斾负骞抽潰娉曠嚎 (0.5,0.5,1,1)锛氳В鐮佸悗鏄垏绾跨┖闂?+Z
		//
		// 鏀归€犲墠杩欓噷鎶?5 涓Ы鎸夊彟涓€濂楅『搴忓～锛屽鑷达細
		//   Normal 妲芥嬁鍒扮函鐧?(1,1,1) 鈫?瑙ｇ爜鎴?(1,1,1) 鐨勯潪娉曟硶绾匡紝鍏ㄦā鍨嬪厜鐓ч敊涔?
		//   MetallicRoughness 妲芥嬁鍒板钩闈㈡硶绾胯创鍥?鈫?metallic鈮?锛岄潪閲戝睘鍏ㄥ彉閲戝睘
		//   Occlusion 妲芥嬁鍒伴€忔槑榛?鈫?AO=0锛岀幆澧冨厜琚畬鍏ㄦ姽鎺?
		// 瀹炴祴璇ユā鍨?20 涓潗璐ㄩ噷 19 涓彧鏈?baseColor锛屽叾浣欐Ы鍏ㄩ儴璧伴粯璁よ〃锛?
		// 鎵€浠ヨ繖寮犺〃鐩存帴鍐冲畾浜嗙敾闈㈡槸鍚︽纭€?
		// ============================================================
		D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures] =
		{
			GetDefaultTexture(kWhiteOpaque2D),     // t0 BaseColor:          涔?1 淇濇寔鍘熻壊
			GetDefaultTexture(kWhiteOpaque2D),     // t1 MetallicRoughness:  G=1,B=1 浜ょ粰 factor 鍐冲畾
			GetDefaultTexture(kWhiteOpaque2D),     // t2 Occlusion:          AO = 1
			GetDefaultTexture(kBlackOpaque2D),     // t3 Emissive:           鏃犺嚜鍙戝厜
			GetDefaultTexture(kDefaultNormalMap)   // t4 Normal:             骞抽潰娉曠嚎 (0.5,0.5,1)
		};
		for (uint32_t i = 0; i < kNumTextures; ++i) // 楠岃瘉鎵€鏈夐粯璁ょ汗鐞嗘槸鍚︽湁鏁?
		{
			if (DefaultTextures[i].ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN)
			{
				Utility::Printf("Error: Default texture %d not initialized\n", i);
				return;
			}
		}

       D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[kNumTextures];
		for (uint32_t j = 0; j < kNumTextures; ++j)
		{
			//// 0xffff 琛ㄧず璇ユЫ浣嶆棤璐村浘锛屼娇鐢ㄥ紩鎿庨粯璁よ创鍥?
			uint16_t idx = srcMat.stringIdx[j];
			if (idx == 0xffff)
			{
				SourceTextures[j] = DefaultTextures[j];
			}
			else if (idx >= model.textures.size())
			{
				// 闃插尽鎬у洖閫€锛氱储寮曡秺鐣屾椂浣跨敤榛樿绾圭悊
				Utility::Printf("Warning: texture index %u out of range (num textures=%zu). Using default.\n", idx, model.textures.size());
				SourceTextures[j] = DefaultTextures[j];
			}
			else
			{
				SourceTextures[j] = model.textures[idx].GetSRV();
			}
		}

		g_Device->CopyDescriptors(1, &destSrvHandle, &DestCount, DestCount,
			SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		///////////////////////////////////////////////////////////////////////////
		//
		//
		// 绗笁姝ワ細澶勭悊閲囨牱鍣ㄦ弿杩扮 (Sampler Descriptor)
		uint32_t addressModes = srcMat.addressModes;
		auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

		// R3/WP-1B锛氱紦瀛樻寜 UnifiedDescriptorHeap 鐨勪唬闄呭彿鏍￠獙锛屾敮鎸佸杞?Init/Shutdown 闃?ABA銆?
		const bool samplerCacheHit =
			samplerMapLookup != g_SamplerPermutations.end() &&
			samplerMapLookup->second.generation == samplerHeapGeneration;

		// 鑻ヨ閲囨牱鍣ㄧ粍鍚堟湭缂撳瓨锛堟垨缂撳瓨鏉ヨ嚜涓婁竴浠ｅ凡琚攢姣佺殑鍫嗭級锛屽垯鍦ㄩ噰鏍峰櫒鍫嗕腑閲嶆柊鍒嗛厤骞舵嫹璐?
		if (!samplerCacheHit)
		{
			const BindlessAllocation samplerAlloc = unifiedHeap.AllocSamplers(kNumTextures);
			if (!samplerAlloc.IsValid())
			{
				LOG_ERROR(L"[ModelLoader] SAMPLER 鍫嗗垎閰嶅け璐ワ紙matIdx=%u锛塡n", matIdx);
				return;
			}
			const uint32_t SamplerDescriptorTable = samplerAlloc.HeapIndex;
			const D3D12_CPU_DESCRIPTOR_HANDLE destSamplerHandle = samplerAlloc.Cpu;
			// 璁板綍鏈鍒嗛厤鎵€灞炵殑浠ｉ檯锛涗笅涓€杞爢閲嶅缓鍚庤鏉＄洰浼氳嚜鍔ㄥけ鏁堛€?
			// 娉ㄦ剰蹇呴』鍐欏湪涓嬮潰閭ｄ釜寰幆涔嬪墠 鈥斺€?寰幆浼氭妸 addressModes 閫愭Ы鍙崇Щ娑堣€楁帀锛?
			// 涔嬪悗鐨?addressModes 宸茬粡涓嶆槸缂撳瓨閿簡銆?
			g_SamplerPermutations[addressModes] = { samplerHeapGeneration, SamplerDescriptorTable };

			model.m_Materials[matIdx].SRVTable = SRVDescriptorTable;
			model.m_Materials[matIdx].SamplerTable = SamplerDescriptorTable;

			D3D12_CPU_DESCRIPTOR_HANDLE SourceSamplers[kNumTextures];
			for (uint32_t j = 0; j < kNumTextures; ++j)
			{
				SourceSamplers[j] = GetSampler(addressModes & 0xF);
				addressModes >>= 4; // 閬嶅巻姣忎釜绾圭悊妲戒綅鐨?4-bit 瀵诲潃璁剧疆
			}
			g_Device->CopyDescriptors(1, &destSamplerHandle, &DestCount,
				DestCount, SourceSamplers, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		}
		else
		{
			model.m_Materials[matIdx].SRVTable = SRVDescriptorTable;
			model.m_Materials[matIdx].SamplerTable = samplerMapLookup->second.offset;
		}

      // Safely read material constants if available
		if (matIdx < materialConstants.size())
			model.m_Materials[matIdx].Flags = ConvertToRenderMaterialFlags(materialConstants[matIdx]);
		else
		{
			model.m_Materials[matIdx].Flags = Scene::Material::kMaterial_None;
			Utility::Printf("Warning: materialConstants missing for material %u, using default flags\n", matIdx);
		}

		// Compute Material CBV address only if material constant buffer exists
		if (model.m_MaterialConstants.GetGpuVirtualAddress() != D3D12_GPU_VIRTUAL_ADDRESS_NULL && matIdx < materialConstants.size())
			model.m_Materials[matIdx].MaterialCBV = model.m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * matIdx;
		else
			model.m_Materials[matIdx].MaterialCBV = 0;
	}

	// [WP-1B / TD-01b] 缁戝畾寤惰繜褰掕繕鍥炶皟锛歁odel 閿€姣佹椂灏嗘墍鏈夋潗璐ㄥ崰鐢ㄧ殑 Zone A 5 妲藉潡
	// 缁?DeferredReleaseQueue 褰掕繕缁?UnifiedDescriptorHeap锛堣嫢鍫嗗凡鍏堥攢姣佸垯鐢?weak_ptr 鑷姩璺宠繃锛夈€?
	if (!allocatedMaterialSlots.empty())
	{
		Renderer::UnifiedDescriptorHeap* heapPtr = &unifiedHeap;
		std::weak_ptr<uint32_t> token = unifiedHeap.GetLifetimeToken();
		Graphics::DeferredReleaseQueue* releaseQueue = &services->GetReleaseQueue();
		model.m_ReleaseMaterialDescriptors = [heapPtr, token, releaseQueue, slots = std::move(allocatedMaterialSlots)]() {
			if (token.expired())
				return;
			for (uint32_t slot : slots)
				heapPtr->FreePersistentMaterialBlock(slot, kNumTextures, releaseQueue);
		};
	}
}



// 澶栭儴璋冪敤鎺ュ彛锛氬姞杞芥ā鍨嬫枃浠跺苟澶勭悊缂撳瓨閫昏緫锛堟病鍟冿紝鏆傜暀锛屽悗缁晝锛?
std::shared_ptr<Scene::Model::Model> Scene::Model::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
	// 1. 璺緞澶勭悊闃舵
	const std::wstring miniFileName = Utility::RemoveExtension(filePath) + L".ty";
	const std::wstring fileName = Utility::RemoveBasePath(filePath);

	struct _stat64 sourceFileStat;
	struct _stat64 miniFileStat;
	std::ifstream inFile;
	FileHeader header;

	// 2. 缂撳瓨鐘舵€佹鏌?(Cache Validation)
	// _wstat64 浼氳Е鍙戜竴娆＄郴缁熻皟鐢?(System Call / 銈枫偣銉嗐儬銈炽兗銉?锛岄櫡鍏ュ唴鏍告€佹煡璇㈡枃浠剁郴缁熷厓鏁版嵁銆?
	// 濡傛灉杩斿洖 -1锛岃鏄庢枃浠朵笉瀛樺湪銆?
	bool sourceFileMissing = _wstat64(filePath.c_str(), &sourceFileStat) == -1;
	bool miniFileMissing = _wstat64(miniFileName.c_str(), &miniFileStat) == -1;

	// 濡傛灉鍘熷缇庢湳璧勪骇 (濡?glTF) 閮戒笉瀛樺湪锛屽垯寮哄埗闄嶇骇涓轰笉閲嶅缓锛屽瘎甯屾湜浜庣紦瀛樻枃浠跺瓨鍦ㄣ€?
	if (sourceFileMissing)
		forceRebuild = false;

	// 鍘熷鏂囦欢鍜岀紦瀛樻枃浠跺弻鍙屼涪澶憋紝鐩存帴鎶ラ敊閫€鍑恒€傝繖涔熸槸鏈€甯歌鐨勮繍琛屾椂璧勬簮缂哄け宕╂簝鍘熷洜銆?
	// R0锛氶敊璇俊鎭繀椤昏兘瀹氫綅銆屽摢涓枃浠躲€佸湪鍝€佽鎬庝箞淇€嶃€?
	// 鍘熸潵鍙墦鍗颁竴涓８鏂囦欢鍚嶏紝鐪嬩笉鍒版悳绱㈣矾寰勶紝涔熺湅涓嶅埌鍚庢灉锛屾帓鏌ユ椂鍑犱箮鏃犵敤銆?
	if (sourceFileMissing && miniFileMissing)
	{
		Utility::Printf(L"[Asset] 閿欒锛氭ā鍨嬪姞杞藉け璐?鈥斺€?婧愭枃浠朵笌缂撳瓨鏂囦欢閮戒笉瀛樺湪\n");
		Utility::Printf(L"[Asset]   婧愭枃浠?: %ws\n", filePath.c_str());
		Utility::Printf(L"[Asset]   缂撳瓨   : %ws\n", miniFileName.c_str());
		Utility::Printf(L"[Asset]   鍚庢灉   : 璇ユā鍨嬩笉浼氭覆鏌擄紙閫氬父琛ㄧ幇涓虹敾闈腑缂哄皯瀵瑰簲鐗╀綋锛塡n");
		Utility::Printf(L"[Asset]   鎺掓煡   : 纭涓婃柟 [Asset] AssetsRoot 鏄惁鎸囧悜鏈」鐩?Assets 鐩綍锛沑n");
		Utility::Printf(L"[Asset]            鑻ユā鍨嬪皻鏈笅杞斤紝璇峰弬鑰?docs/AI/ 涓殑璧勪骇璇存槑琛ラ綈銆俓n");
		return nullptr;
	}

	bool needBuild = forceRebuild;

	// 瑙﹀彂閲嶅缓鐨勬潯浠讹細1. 缂撳瓨鏂囦欢缂哄け銆?2. 鍘熷鏂囦欢瀛樺湪锛屼笖鍏舵渶鍚庝慨鏀规椂闂?(st_mtime) 鏅氫簬缂撳瓨鏂囦欢銆?
	if (miniFileMissing || !sourceFileMissing && sourceFileStat.st_mtime > miniFileStat.st_mtime)
		needBuild = true;

	// 3. 鐗堟湰鍙锋牎楠?(Version Verification)
	// 鍗充娇缂撳瓨瀛樺湪锛屼篃鏈夊彲鑳芥槸鏃х増寮曟搸鐢熸垚鐨勩€傝繖閲岄€氳繃璇诲彇鏂囦欢澶撮儴杩涜榄旀暟鍜岀増鏈彿姣斿銆?
	if (!needBuild)
	{
		// 浠ヤ簩杩涘埗鍙妯″紡鎵撳紑鏂囦欢銆傛敞鎰忥細std::ifstream 鏄悓姝?I/O锛屼細闃诲涓荤嚎绋嬨€?
		inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
		inFile.read((char*)&header, sizeof(FileHeader));

		// strncmp 瀵规瘮鍓?涓瓧鑺傜殑鏍囪瘑绗?(Magic Number) "TY"銆?
		if (strncmp(header.id, "TY", 2) != 0 || header.version != CURRENT_MINI_FILE_VERSION)
		{
			// 濡傛灉鐗堟湰涓嶅尮閰嶏紙搴熷純 / 銉囥儣銉偙銉笺儓锛夛紝鍒欏己鍒惰Е鍙戦噸寤猴紝骞跺叧闂綋鍓嶉敊璇祦銆?
			Utility::Printf("Model version deprecated.  Rebuilding %ws...\n", fileName.c_str());
			needBuild = true;
			inFile.close();
		}
	}

	// 4. 璧勬簮鐑樼剻闃舵 (Asset Baking / 銈偦銉冦儓銉欍偆銈?
	if (needBuild)
	{
		if (sourceFileMissing)
		{
			// 闃插尽鎬х紪绋嬶細闇€瑕侀噸寤轰絾婧愭枃浠舵病浜嗐€?
			Utility::Printf(L"[Asset] 閿欒锛氶渶瑕侀噸鏂扮儤鐒欙紝浣嗘簮鏂囦欢缂哄け -> %ws\n", filePath.c_str());
			Utility::Printf(L"[Asset]   鎻愮ず锛氱敤 -RebuildModels 鍓嶈鍏堢‘璁ゆ簮 .gltf/.glb 瀛樺湪銆俓n");
			return nullptr;
		}

		ModelData modelData;
		// 鑾峰彇缁熶竴鐨勫皬鍐欐枃浠舵墿灞曞悕锛屽噯澶囪矾鐢辩粰涓嶅悓鐨勮В鏋愬櫒 (Parser / 銉戙兗銈点兗)銆?
		const std::wstring fileExt = Utility::ToLower(Utility::GetFileExtension(filePath));

		// 宸ヤ笟鐣岀棝鐐癸細glTF 绛夊熀浜?JSON 鐨勬牸寮忚櫧鐒堕€氱敤锛屼絾鍦?CPU 绔弽搴忓垪鍖栨瀬鍏剁紦鎱紝涓斾骇鐢熷ぇ閲忓唴瀛樼鐗囥€?
		if (fileExt == L"gltf" || fileExt == L"glb")
		{
			glTF::Asset asset(filePath);
			// BuildModel 浼氬皢鏍戠姸鐨?JSON 鏁版嵁灞曞钩涓鸿繛缁殑鏁扮粍锛圫oA 鎴?AoS 甯冨眬锛夛紝璁＄畻鍒囩嚎绌洪棿锛屽苟鐢熸垚浼樺寲鐨勭储寮曘€?
			if (!BuildModel(modelData, asset))
				return nullptr;
		}
		else
		{
			Utility::Printf(L"Unsupported model file extension: %ws\n", fileExt.c_str());
			return nullptr;
		}

		// 灏嗗睍骞冲悗鐨?ModelData 涓€娆℃€ф寜鎴戜滑涓婇潰 ASCII 鍥剧殑绱у噾甯冨眬 Dump 鍒扮鐩樸€?
		if (!SaveModel(miniFileName, modelData))
			return nullptr;

		// 閲嶆柊鎵撳紑鍒氬垰鐑樼剻濂界殑浜岃繘鍒舵枃浠舵祦锛屽噯澶囨寮忓姞杞藉埌鍐呭瓨銆?
		inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
		inFile.read((char*)&header, sizeof(FileHeader));
	}

	// 瀹归敊澶勭悊锛氱‘淇濇枃浠舵祦鍙敤銆?
	if (!inFile)
		return nullptr;

	// [WP-1D / TD-07c] 杩愯鏈熸枃浠跺ご涓庣鐩橀暱搴︽牎楠岋細鎴柇鎴栨崯鍧忕殑 .ty 蹇呴』浼橀泤杩斿洖 nullptr锛?
	// 涓嶅緱鎸夌暩褰?header 鍙戣捣瓒呭ぇ鍒嗛厤鎴栬鍙栨湭瀹氫箟鍨冨溇鍐呭瓨銆?
	if (strncmp(header.id, "TY", 2) != 0 || header.version != CURRENT_MINI_FILE_VERSION)
	{
		LOG_ERROR(L"[Asset] .ty 缂撳瓨鏂囦欢澶存棤鏁堬細%ws\n", miniFileName.c_str());
		return nullptr;
	}

	struct _stat64 actualMiniStat = {};
	if (_wstat64(miniFileName.c_str(), &actualMiniStat) != 0 || actualMiniStat.st_size < (long long)sizeof(FileHeader))
	{
		LOG_ERROR(L"[Asset] .ty 缂撳瓨鏂囦欢闀垮害寮傚父锛?ws\n", miniFileName.c_str());
		return nullptr;
	}
	const uint64_t fileBytes = static_cast<uint64_t>(actualMiniStat.st_size);
	const uint64_t minPayloadBytes =
		static_cast<uint64_t>(sizeof(FileHeader)) +
		static_cast<uint64_t>(header.geometrySize) +
		static_cast<uint64_t>(header.numNodes) * sizeof(GraphNode) +
		static_cast<uint64_t>(header.meshDataSize) +
		static_cast<uint64_t>(header.numMaterials) * (sizeof(Scene::Material::MaterialConstantData) + sizeof(MaterialTextureData)) +
		static_cast<uint64_t>(header.numTextures) * sizeof(uint8_t);
	if (minPayloadBytes > fileBytes)
	{
		LOG_ERROR(L"[Asset] .ty 缂撳瓨宸叉埅鏂紙闇€瑕佽嚦灏?%llu B锛屽疄闄?%llu B锛夛細%ws\n",
			(unsigned long long)minPayloadBytes, (unsigned long long)fileBytes, miniFileName.c_str());
		return nullptr;
	}

	auto readExact = [&](void* dst, size_t bytes, const wchar_t* section) -> bool {
		if (bytes == 0)
			return true;
		inFile.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(bytes));
		if (!inFile || static_cast<size_t>(inFile.gcount()) != bytes)
		{
			LOG_ERROR(L"[Asset] .ty 璇诲彇 %ws 澶辫触锛堟祦鎻愬墠缁撴潫锛夛細%ws\n", section, miniFileName.c_str());
			return false;
		}
		return true;
	};

	std::wstring basePath = Utility::GetBasePath(filePath);

	// 5. 鍐呭瓨鍒嗛厤涓庢暟鎹亴鍏?(Memory Allocation & Loading)
	std::shared_ptr<Scene::Model::Model> model(new Scene::Model::Model);

	// 鍔犺浇鍦烘櫙鍥惧眰绾ф爲 (Scene Graph) - 涓昏鐢ㄤ簬楠ㄩ鍔ㄧ敾鍜岀墿浠舵寕杞姐€?
	model->m_NumNodes = header.numNodes;
	if (header.numNodes > 0)
		model->m_SceneGraph.reset(new Scene::Model::GraphNode[header.numNodes]);

	// CPU 渚т繚鐣欎竴浠?Mesh 鍏冩暟鎹紙濡?Submesh 鐨勮竟鐣屻€佹潗璐ㄧ储寮曪級銆?
	model->m_NumMeshes = header.numMeshes;
	if (header.meshDataSize > 0)
		model->m_MeshData.reset(new uint8_t[header.meshDataSize]);

	// 銆怐X12 鏍稿績锛氬嚑浣曚綋鏁版嵁涓婁紶銆?Geometry Upload / 銈搞偑銉°儓銉偄銉冦儣銉兗銉?
	if (header.geometrySize > 0)
	{
		UploadBuffer modelData;
		modelData.Create(L"Model Data Upload", header.geometrySize);
		bool ok = readExact(modelData.Map(), header.geometrySize, L"GeometryData");
		modelData.Unmap();
		if (!ok)
			return nullptr;
		model->m_DataBuffer.Create(L"Model Data", header.geometrySize, 1, modelData);
	}

	// 灏嗗垰鎵嶅垎閰嶇殑 CPU 渚у眰绾ф爲鍜?Mesh 鍏冩暟鎹～婊°€?
	if (!readExact(model->m_SceneGraph.get(), static_cast<size_t>(header.numNodes) * sizeof(GraphNode), L"SceneGraph"))
		return nullptr;
	if (!readExact(model->m_MeshData.get(), header.meshDataSize, L"MeshData"))
		return nullptr;

	std::vector<Scene::Material::MaterialConstantData> materialConstantData(header.numMaterials);
	// 銆怐X12 鏍稿績锛氭潗璐ㄥ父閲忎笂浼犮€?Material Constant Buffer / 銉炪儐銉偄銉畾鏁般儛銉冦儠銈?
	if (header.numMaterials > 0)
	{
		UploadBuffer materialConstants;
		materialConstants.Create(L"Material Constant Upload", static_cast<size_t>(header.numMaterials) * sizeof(MaterialConstants));
		MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();

		bool ok = true;
		for (uint32_t i = 0; i < header.numMaterials; ++i)
		{
			if (!readExact(&materialConstantData[i], sizeof(Scene::Material::MaterialConstantData), L"MaterialConstants"))
			{
				ok = false;
				break;
			}
			std::memcpy(materialCBV, &materialConstantData[i], sizeof(Scene::Material::MaterialConstantData));
			materialCBV++; // 鎸囬拡鍋忕Щ锛屽啓鍏ヤ笅涓€涓潗璐ㄥ弬鏁?
		}
		materialConstants.Unmap();
		if (!ok)
			return nullptr;

		model->m_MaterialConstants.Create(L"Material Constants", header.numMaterials, sizeof(MaterialConstants), materialConstants);
	}

	// 6. 绾圭悊鍏冩暟鎹姞杞?(Texture Metadata Loading)
	std::vector<MaterialTextureData> materialTextures(header.numMaterials);
	if (!readExact(materialTextures.data(), static_cast<size_t>(header.numMaterials) * sizeof(MaterialTextureData), L"MaterialTextures"))
		return nullptr;

	std::vector<std::wstring> textureNames(header.numTextures);
	for (uint32_t i = 0; i < header.numTextures; ++i)
	{
		std::string utf8TextureName;
		// 浠?'\0' (Null Terminator) 浣滀负鍒嗛殧绗︼紝浠庝簩杩涘埗娴佷腑鎻愬彇瀛楃涓层€?
		if (!std::getline(inFile, utf8TextureName, '\0'))
		{
			LOG_ERROR(L"[Asset] .ty 绾圭悊瀛楃涓茶〃鎹熷潖锛堢储寮?%u/%u锛夛細%ws\n", i, header.numTextures, miniFileName.c_str());
			return nullptr;
		}
		textureNames[i] = Utility::UTF8ToWideString(utf8TextureName);
	}

	std::vector<uint8_t> textureOptions(header.numTextures);
	if (!readExact(textureOptions.data(), static_cast<size_t>(header.numTextures) * sizeof(uint8_t), L"TextureOptions"))
		return nullptr;

	// 璋冪敤鍐呴儴鍑芥暟锛岀湡姝ｅ幓瑙﹀彂鎴栫粦瀹氳繖浜涚汗鐞嗚祫婧?(SRV 鍒涘缓绛?銆?
	LoadMaterials(*model, materialTextures, materialConstantData, textureNames, textureOptions, basePath);

	// R7锛氱暀涓€浠芥潗璐ㄦ爣閲忕殑 CPU 鍓湰锛屼緵鍏夎拷璺緞鍦ㄥ缓 BLAS 鏃剁儤杩涙瘡瀹炰緥鏁版嵁銆?
	// 涓庝笂琛屽埌 GPU 鐨?m_MaterialConstants 鐢ㄧ殑鏄?*鍚屼竴浠?* materialConstantData锛?
	// 鍥犳涓嶄細鍑虹幇銆屽厜鏍呬竴濂椼€佸厜杩藉彟涓€濂椼€嶇殑涓嶄竴鑷淬€?
	model->m_MaterialConstantData = materialConstantData;

	// 鍖呭洿鐩?
	model->m_BoundingSphere = Math::BoundingSphere(*(XMFLOAT4*)header.boundingSphere);
	model->m_BoundingBox = Math::AxisAlignedBox(Math::Vector3(*(XMFLOAT3*)header.minPos), Math::Vector3(*(XMFLOAT3*)header.maxPos));

	return model;
}

//+ ---------------------------------------------------------------------- - +
//| .mini File / Memory Layout(绱у噾鍨嬩簩杩涘埗杩炵画鍐呭瓨甯冨眬) |
//+= ======================================================================+
//| FileHeader(鐗堟湰鍙? 鍚勬暟鎹潡鐨?Size 鍜?Count, 鍖呭洿鐩掔瓑瀹氶暱鏁版嵁) |
//+---------------------------------------------------------------------- - +
//| SceneGraph Array(GraphNode[numNodes] - 鍦烘櫙鑺傜偣灞傜骇鏍? |
//+---------------------------------------------------------------------- - +
//| Mesh Data Array(uint8_t[meshDataSize] - 椤剁偣 / 绱㈠紩娣峰悎鐨勪氦閿欑紦鍐? |
//+---------------------------------------------------------------------- - +
//| Geometry Data(CPU->GPU 涓婁紶鐢ㄧ殑澶у潡鏄惧瓨鏁版嵁鍧? |
//+---------------------------------------------------------------------- - +
//| Material Constants(MaterialConstantData[numMaterials] - 鏉愯川鍙傛暟) |
//+---------------------------------------------------------------------- - +
//| Texture Metadata(鍚嶇О瀛楃涓? 閲囨牱鍣ㄩ厤缃€夐」) |
//+---------------------------------------------------------------------- - +
//| Animation KeyFrames & Curves(鍔ㄧ敾鏇茬嚎鏁版嵁, 绱у噾鎺掑垪) |
//+---------------------------------------------------------------------- - +
//| Skeleton Joints & IBMs(楠ㄩ鑺傜偣绱㈠紩涓庨€嗙粦瀹氱煩闃? |
//+---------------------------------------------------------------------- - +
