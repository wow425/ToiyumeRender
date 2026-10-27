


#include "00_Core/PCH.h"
#include "05_Scene/Model/ModelLoader.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "05_Scene/Model/Model.h"
#include "03_AssetSystem/Importers/glTF.h"

#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "03_AssetSystem/Importers/Texture/TextureConvert.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"

#include <fstream>
#include <unordered_map>

using namespace Renderer;
using namespace Graphics;

// R3：采样器组合缓存的一条记录。
// 为什么不能只存 offset：s_SamplerHeap 是纯 bump 分配器，两个 Renderer 的 Shutdown 都会
// 调 s_SamplerHeap.Destroy()，下一轮 Initialize 的 Create() 又把游标归零。于是上一轮
// 存下来的偏移在新堆里「碰巧」仍然落在合法范围内，却指向了完全不同的采样器 ——
// 不会崩溃、不会报错，只会让贴图采样模式静默错掉（重复/边缘拉伸/过滤不对），
// 排查成本极高。所以缓存值里带上堆的代际号，代际不符一律当未命中、重新分配。
struct SamplerCacheEntry
{
	uint32_t generation; // 分配时 Renderer::s_SamplerHeap 的代际（DescriptorHeap::GetGeneration）
	uint32_t offset;     // 采样器描述符表在堆内的偏移（GetOffsetOfHandle 的结果）
};

// 缓存已创建的采样器组合。Key: 寻址模式组合, Value: 描述符堆偏移 + 代际号
std::unordered_map<uint32_t, SamplerCacheEntry> g_SamplerPermutations;

// 根据位掩码（AddressModes）动态获取或创建 D3D12 采样器描述符
D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(uint32_t addressModes)
{
	SamplerDesc samplerDesc;
	// 取低 2 位作为 U 方向寻址模式 (0: Wrap, 1: Mirror, 2: Clamp 等)
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(addressModes & 0x3);
	// 右移 2 位取接下来的 2 位作为 V 方向寻址模式
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(addressModes >> 2);
	// 在静态采样器堆或临时堆中创建描述符并返回句柄
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
	const std::vector<Scene::Model::MaterialTextureData>& materialTextures,   // 材质纹理数据
	const std::vector<Scene::Material::MaterialConstantData>& materialConstants, // 材质参数数据
	const std::vector<std::wstring>& textureNames,              // 文件名
	const std::vector<uint8_t>& textureOptions,
	const std::wstring& basePath)                               // 路径
{
	static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

	// 1. Load textures加载纹理
	{
     const uint32_t numTextures = (uint32_t)textureNames.size();
		model.textures.resize(numTextures);
		for (size_t ti = 0; ti < numTextures; ++ti)
		{
            std::wstring originalFile = basePath + textureNames[ti];
			// 防御性检查：确保 textureOptions 长度足够
			uint8_t opt = 0xFF;
			if (ti < textureOptions.size())
				opt = textureOptions[ti];
			else
				Utility::Printf("Warning: textureOptions missing entry for texture %zu, using default\n", ti);

			// 动态编译：若无 DDS 或原始文件更新，则调用纹理转换工具生成优化过的 DDS
			CompileTextureOnDemand(originalFile, opt);

			std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";
			// 从磁盘载入 GPU 资源
			model.textures[ti] = TextureManager::LoadDDSFromFile(ddsFile);
		}
	}

	// 生成描述符表和记录材质偏移量
	const uint32_t numMaterials = (uint32_t)materialTextures.size();
	model.m_Materials.resize(numMaterials);

	// 遍历为每个网格赋予纹理（按照PBR标准划分5个纹理槽位，无则使用default填充)
	for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
	{
		const Scene::Model::MaterialTextureData& srcMat = materialTextures[matIdx];

		// 分配纹理描述符表。槽位顺序严格遵循 ConstantBuffers.h 的材质纹理槽位
		// （即 HLSL GBufferPS 的 t0..t4）：
		//   t0 BaseColor · t1 MetallicRoughness · t2 Occlusion · t3 Emissive · t4 Normal
		DescriptorHandle TextureHandles = Renderer::s_TextureHeap.Alloc(kNumTextures);
		uint32_t SRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(TextureHandles);

		uint32_t DestCount = kNumTextures;
		uint32_t SourceCounts[kNumTextures] = { 1, 1, 1, 1, 1 };

		// ============================================================
		// 默认（占位）纹理 —— R1.2
		// ------------------------------------------------------------
		// 这些纹理决定「材质没有该贴图时，Shader 采样到什么」。选错会直接扭曲 PBR：
		//   BaseColor         缺省应为白色 (1,1,1,1)：乘法单位元，保留 baseColorFactor 原色
		//   MetallicRoughness 缺省应为 (1, 1, 1, 1)：G=roughness=1, B=metallic=1，
		//                     再乘材质的 metallicFactor/roughnessFactor 才等于材质值
		//                     （glTF 规定：无 MR 贴图时 metallic/roughness 全由 factor 决定）
		//   Occlusion         缺省应为白色 (1,1,1,1)：AO=1 表示完全不受遮蔽
		//   Emissive          缺省应为黑色 (0,0,0,1)：无自发光
		//   Normal            缺省应为平面法线 (0.5,0.5,1,1)：解码后是切线空间 +Z
		//
		// 改造前这里把 5 个槽按另一套顺序填，导致：
		//   Normal 槽拿到纯白 (1,1,1) → 解码成 (1,1,1) 的非法法线，全模型光照错乱
		//   MetallicRoughness 槽拿到平面法线贴图 → metallic≈1，非金属全变金属
		//   Occlusion 槽拿到透明黑 → AO=0，环境光被完全抹掉
		// 实测该模型 20 个材质里 19 个只有 baseColor，其余槽全部走默认表，
		// 所以这张表直接决定了画面是否正确。
		// ============================================================
		D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures] =
		{
			GetDefaultTexture(kWhiteOpaque2D),     // t0 BaseColor:          乘 1 保持原色
			GetDefaultTexture(kWhiteOpaque2D),     // t1 MetallicRoughness:  G=1,B=1 交给 factor 决定
			GetDefaultTexture(kWhiteOpaque2D),     // t2 Occlusion:          AO = 1
			GetDefaultTexture(kBlackOpaque2D),     // t3 Emissive:           无自发光
			GetDefaultTexture(kDefaultNormalMap)   // t4 Normal:             平面法线 (0.5,0.5,1)
		};
		for (uint32_t i = 0; i < kNumTextures; ++i) // 验证所有默认纹理是否有效
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
			//// 0xffff 表示该槽位无贴图，使用引擎默认贴图
			uint16_t idx = srcMat.stringIdx[j];
			if (idx == 0xffff)
			{
				SourceTextures[j] = DefaultTextures[j];
			}
			else if (idx >= model.textures.size())
			{
				// 防御性回退：索引越界时使用默认纹理
				Utility::Printf("Warning: texture index %u out of range (num textures=%zu). Using default.\n", idx, model.textures.size());
				SourceTextures[j] = DefaultTextures[j];
			}
			else
			{
				SourceTextures[j] = model.textures[idx].GetSRV();
			}
		}

		g_Device->CopyDescriptors(1, &TextureHandles, &DestCount, DestCount,
			SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		///////////////////////////////////////////////////////////////////////////
		//
		//
		// 第三步：处理采样器描述符 (Sampler Descriptor)
		uint32_t addressModes = srcMat.addressModes;
		auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

		// R3：缓存必须按「代际 + 偏移」校验。s_SamplerHeap 一旦 Destroy + Create（切 renderer
		// 必然发生），旧偏移就指向别的采样器了，只有代际号相同才算真命中。
		const uint32_t samplerHeapGeneration = Renderer::s_SamplerHeap.GetGeneration();
		const bool samplerCacheHit =
			samplerMapLookup != g_SamplerPermutations.end() &&
			samplerMapLookup->second.generation == samplerHeapGeneration;

		// 若该采样器组合未缓存（或缓存来自上一代已被销毁的堆），则在采样器堆中重新分配并拷贝
		if (!samplerCacheHit)
		{
			DescriptorHandle SamplerHandles = Renderer::s_SamplerHeap.Alloc(kNumTextures);
			uint32_t SamplerDescriptorTable = Renderer::s_SamplerHeap.GetOffsetOfHandle(SamplerHandles);
			// 记录本次分配所属的代际；下一轮堆重建后该条目会自动失效。
			// 注意必须写在下面那个循环之前 —— 循环会把 addressModes 逐槽右移消耗掉，
			// 之后的 addressModes 已经不是缓存键了。
			g_SamplerPermutations[addressModes] = { samplerHeapGeneration, SamplerDescriptorTable };

			model.m_Materials[matIdx].SRVTable = SRVDescriptorTable;
			model.m_Materials[matIdx].SamplerTable = SamplerDescriptorTable;

			D3D12_CPU_DESCRIPTOR_HANDLE SourceSamplers[kNumTextures];
			for (uint32_t j = 0; j < kNumTextures; ++j)
			{
				SourceSamplers[j] = GetSampler(addressModes & 0xF);
				addressModes >>= 4; // 遍历每个纹理槽位的 4-bit 寻址设置
			}
			g_Device->CopyDescriptors(1, &SamplerHandles, &DestCount,
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
}



// 外部调用接口：加载模型文件并处理缓存逻辑（没啃，暂留，后续啃）
std::shared_ptr<Scene::Model::Model> Scene::Model::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
	// 1. 路径处理阶段
	const std::wstring miniFileName = Utility::RemoveExtension(filePath) + L".ty";
	const std::wstring fileName = Utility::RemoveBasePath(filePath);

	struct _stat64 sourceFileStat;
	struct _stat64 miniFileStat;
	std::ifstream inFile;
	FileHeader header;

	// 2. 缓存状态检查 (Cache Validation)
	// _wstat64 会触发一次系统调用 (System Call / システムコール)，陷入内核态查询文件系统元数据。
	// 如果返回 -1，说明文件不存在。
	bool sourceFileMissing = _wstat64(filePath.c_str(), &sourceFileStat) == -1;
	bool miniFileMissing = _wstat64(miniFileName.c_str(), &miniFileStat) == -1;

	// 如果原始美术资产 (如 glTF) 都不存在，则强制降级为不重建，寄希望于缓存文件存在。
	if (sourceFileMissing)
		forceRebuild = false;

	// 原始文件和缓存文件双双丢失，直接报错退出。这也是最常见的运行时资源缺失崩溃原因。
	// R0：错误信息必须能定位「哪个文件、在哪、该怎么修」。
	// 原来只打印一个裸文件名，看不到搜索路径，也看不到后果，排查时几乎无用。
	if (sourceFileMissing && miniFileMissing)
	{
		Utility::Printf(L"[Asset] 错误：模型加载失败 —— 源文件与缓存文件都不存在\n");
		Utility::Printf(L"[Asset]   源文件 : %ws\n", filePath.c_str());
		Utility::Printf(L"[Asset]   缓存   : %ws\n", miniFileName.c_str());
		Utility::Printf(L"[Asset]   后果   : 该模型不会渲染（通常表现为画面中缺少对应物体）\n");
		Utility::Printf(L"[Asset]   排查   : 确认上方 [Asset] AssetsRoot 是否指向本项目 Assets 目录；\n");
		Utility::Printf(L"[Asset]            若模型尚未下载，请参考 docs/AI/ 中的资产说明补齐。\n");
		return nullptr;
	}

	bool needBuild = forceRebuild;

	// 触发重建的条件：1. 缓存文件缺失。 2. 原始文件存在，且其最后修改时间 (st_mtime) 晚于缓存文件。
	if (miniFileMissing || !sourceFileMissing && sourceFileStat.st_mtime > miniFileStat.st_mtime)
		needBuild = true;

	// 3. 版本号校验 (Version Verification)
	// 即使缓存存在，也有可能是旧版引擎生成的。这里通过读取文件头部进行魔数和版本号比对。
	if (!needBuild)
	{
		// 以二进制只读模式打开文件。注意：std::ifstream 是同步 I/O，会阻塞主线程。
		inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
		inFile.read((char*)&header, sizeof(FileHeader));

		// strncmp 对比前2个字节的标识符 (Magic Number) "TY"。
		if (strncmp(header.id, "TY", 2) != 0 || header.version != CURRENT_MINI_FILE_VERSION)
		{
			// 如果版本不匹配（废弃 / デプリケート），则强制触发重建，并关闭当前错误流。
			Utility::Printf("Model version deprecated.  Rebuilding %ws...\n", fileName.c_str());
			needBuild = true;
			inFile.close();
		}
	}

	// 4. 资源烘焙阶段 (Asset Baking / アセットベイク)
	if (needBuild)
	{
		if (sourceFileMissing)
		{
			// 防御性编程：需要重建但源文件没了。
			Utility::Printf(L"[Asset] 错误：需要重新烘焙，但源文件缺失 -> %ws\n", filePath.c_str());
			Utility::Printf(L"[Asset]   提示：用 -RebuildModels 前请先确认源 .gltf/.glb 存在。\n");
			return nullptr;
		}

		ModelData modelData;
		// 获取统一的小写文件扩展名，准备路由给不同的解析器 (Parser / パーサー)。
		const std::wstring fileExt = Utility::ToLower(Utility::GetFileExtension(filePath));

		// 工业界痛点：glTF 等基于 JSON 的格式虽然通用，但在 CPU 端反序列化极其缓慢，且产生大量内存碎片。
		if (fileExt == L"gltf" || fileExt == L"glb")
		{
			glTF::Asset asset(filePath);
			// BuildModel 会将树状的 JSON 数据展平为连续的数组（SoA 或 AoS 布局），计算切线空间，并生成优化的索引。
			if (!BuildModel(modelData, asset))
				return nullptr;
		}
		else
		{
			Utility::Printf(L"Unsupported model file extension: %ws\n", fileExt.c_str());
			return nullptr;
		}

		// 将展平后的 ModelData 一次性按我们上面 ASCII 图的紧凑布局 Dump 到磁盘。
		if (!SaveModel(miniFileName, modelData))
			return nullptr;

		// 重新打开刚刚烘焙好的二进制文件流，准备正式加载到内存。
		inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
		inFile.read((char*)&header, sizeof(FileHeader));
	}

	// 容错处理：确保文件流可用。
	if (!inFile)
		return nullptr;

	// 再次断言，确保进入加载阶段的数据绝对合法。
	ASSERT(strncmp(header.id, "TY", 2) == 0 && header.version == CURRENT_MINI_FILE_VERSION);

	std::wstring basePath = Utility::GetBasePath(filePath);

	// 5. 内存分配与数据灌入 (Memory Allocation & Loading)
	std::shared_ptr<Scene::Model::Model> model(new Scene::Model::Model);

	// 加载场景图层级树 (Scene Graph) - 主要用于骨骼动画和物件挂载。
	model->m_NumNodes = header.numNodes;
	model->m_SceneGraph.reset(new Scene::Model::GraphNode[header.numNodes]);

	// CPU 侧保留一份 Mesh 元数据（如 Submesh 的边界、材质索引）。
	model->m_NumMeshes = header.numMeshes;
	model->m_MeshData.reset(new uint8_t[header.meshDataSize]);

	// 【DX12 核心：几何体数据上传】(Geometry Upload / ジオメトリアップロード)
	if (header.geometrySize > 0)
	{
		UploadBuffer modelData;
		modelData.Create(L"Model Data Upload", header.geometrySize);
		inFile.read((char*)modelData.Map(), header.geometrySize);
		modelData.Unmap();
		model->m_DataBuffer.Create(L"Model Data", header.geometrySize, 1, modelData);
	}

	// 将刚才分配的 CPU 侧层级树和 Mesh 元数据填满。
	inFile.read((char*)model->m_SceneGraph.get(), header.numNodes * sizeof(GraphNode));
	inFile.read((char*)model->m_MeshData.get(), header.meshDataSize);

	std::vector<Scene::Material::MaterialConstantData> materialConstantData(header.numMaterials);
	// 【DX12 核心：材质常量上传】(Material Constant Buffer / マテリアル定数バッファ)
	if (header.numMaterials > 0)
	{
		UploadBuffer materialConstants;
		materialConstants.Create(L"Material Constant Upload", header.numMaterials * sizeof(MaterialConstants));
		MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();

		for (uint32_t i = 0; i < header.numMaterials; ++i)
		{
			inFile.read((char*)&materialConstantData[i], sizeof(Scene::Material::MaterialConstantData));
			std::memcpy(materialCBV, &materialConstantData[i], sizeof(Scene::Material::MaterialConstantData));
			materialCBV++; // 指针偏移，写入下一个材质参数
		}
		materialConstants.Unmap();

		model->m_MaterialConstants.Create(L"Material Constants", header.numMaterials, sizeof(MaterialConstants), materialConstants);
	}

	// 6. 纹理元数据加载 (Texture Metadata Loading)
	// 注意：这里只加载纹理的“名字”和“采样设置”，不直接在这里读取巨大的贴图文件。
	// 贴图通常通过独立的异步 I/O 线程池去加载，避免阻塞主线程。
	std::vector<MaterialTextureData> materialTextures(header.numMaterials);
	inFile.read((char*)materialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));

	std::vector<std::wstring> textureNames(header.numTextures);
	for (uint32_t i = 0; i < header.numTextures; ++i)
	{
		std::string utf8TextureName;
		// 以 '\0' (Null Terminator) 作为分隔符，从二进制流中提取字符串。
		std::getline(inFile, utf8TextureName, '\0');
		textureNames[i] = Utility::UTF8ToWideString(utf8TextureName);
	}

	std::vector<uint8_t> textureOptions(header.numTextures);
	inFile.read((char*)textureOptions.data(), header.numTextures * sizeof(uint8_t));

	// 调用内部函数，真正去触发或绑定这些纹理资源 (SRV 创建等)。
	LoadMaterials(*model, materialTextures, materialConstantData, textureNames, textureOptions, basePath);

	// R7：留一份材质标量的 CPU 副本，供光追路径在建 BLAS 时烘进每实例数据。
	// 与上行到 GPU 的 m_MaterialConstants 用的是**同一份** materialConstantData，
	// 因此不会出现「光栅一套、光追另一套」的不一致。
	model->m_MaterialConstantData = materialConstantData;

	// 包围盒
	model->m_BoundingSphere = Math::BoundingSphere(*(XMFLOAT4*)header.boundingSphere);
	model->m_BoundingBox = Math::AxisAlignedBox(Math::Vector3(*(XMFLOAT3*)header.minPos), Math::Vector3(*(XMFLOAT3*)header.maxPos));

	return model;
}

//+ ---------------------------------------------------------------------- - +
//| .mini File / Memory Layout(紧凑型二进制连续内存布局) |
//+= ======================================================================+
//| FileHeader(版本号, 各数据块的 Size 和 Count, 包围盒等定长数据) |
//+---------------------------------------------------------------------- - +
//| SceneGraph Array(GraphNode[numNodes] - 场景节点层级树) |
//+---------------------------------------------------------------------- - +
//| Mesh Data Array(uint8_t[meshDataSize] - 顶点 / 索引混合的交错缓冲) |
//+---------------------------------------------------------------------- - +
//| Geometry Data(CPU->GPU 上传用的大块显存数据块) |
//+---------------------------------------------------------------------- - +
//| Material Constants(MaterialConstantData[numMaterials] - 材质参数) |
//+---------------------------------------------------------------------- - +
//| Texture Metadata(名称字符串, 采样器配置选项) |
//+---------------------------------------------------------------------- - +
//| Animation KeyFrames & Curves(动画曲线数据, 紧凑排列) |
//+---------------------------------------------------------------------- - +
//| Skeleton Joints & IBMs(骨骼节点索引与逆绑定矩阵) |
//+---------------------------------------------------------------------- - +
