
// 该类没怎么啃，就用ai带着看了遍


#include "ModelLoader.h"
#include "Renderer.h"
#include "Model.h"
#include "glTF.h"

#include "../Resource/ResourceManager/TextureManager.h"
#include "TextureConvert.h"
#include "../RHI/PipelineState/GraphicsCommon.h"

#include <fstream>
#include <unordered_map>

using namespace Renderer;
using namespace Graphics;

// 缓存已创建的采样器组合。Key: 寻址模式组合, Value: 描述符堆中的偏移
// 理由：避免为相同参数的采样器重复在堆中分配空间，节省描述符容量。
std::unordered_map<uint32_t, uint32_t> g_SamplerPermutations;

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

// 为模型加载材质并设置描述符表
void LoadMaterials(Model& model,
    const std::vector<MaterialTextureData>& materialTextures,
    const std::vector<std::wstring>& textureNames,
    const std::vector<uint8_t>& textureOptions,
    const std::wstring& basePath)
{
    // 底层溯源：DX12 要求 CBV 的起始地址必须 256 字节对齐 (Alignment / アライメント)
    // 这是为了简化硬件地址转换逻辑 (Address Translation Logic)
    static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

    // 第一步：处理纹理资源
    const uint32_t numTextures = (uint32_t)textureNames.size();
    model.textures.resize(numTextures);
    for (size_t ti = 0; ti < numTextures; ++ti)
    {
        std::wstring originalFile = basePath + textureNames[ti];
        // 动态编译：若无 DDS 或原始文件更新，则调用纹理转换工具生成优化过的 DDS
        CompileTextureOnDemand(originalFile, textureOptions[ti]);

        std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";
        // 从磁盘载入 GPU 资源
        model.textures[ti] = TextureManager::LoadDDSFromFile(ddsFile);
    }

    // 第二步：构建描述符表 (Descriptor Table)
    const uint32_t numMaterials = (uint32_t)materialTextures.size();
    std::vector<uint32_t> tableOffsets(numMaterials);

    for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
    {
        const MaterialTextureData& srcMat = materialTextures[matIdx];

        // 在 GPU 可见的全局着色器资源堆 (SRV Heap) 中申请连续空间
        // kNumTextures 通常为 5 (Diffuse, Normal, Specular 等)，此处省略了kBlackTransparent2D
        DescriptorHandle TextureHandles = Renderer::s_TextureHeap.Alloc(kNumTextures);
        uint32_t SRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(TextureHandles);

        uint32_t DestCount = kNumTextures;
        uint32_t SourceCounts[kNumTextures] = { 1, 1, 1, 1, 1 };

        // 默认备选纹理，防止材质缺失贴图导致采样非法地址导致 GPU 挂起 (TDR)
        D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures] =
        {
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kDefaultNormalMap)
        };

        D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[kNumTextures];
        for (uint32_t j = 0; j < kNumTextures; ++j)
        {
            // 0xffff 表示该槽位无贴图，使用引擎默认贴图
            if (srcMat.stringIdx[j] == 0xffff)
                SourceTextures[j] = DefaultTextures[j];
            else
                SourceTextures[j] = model.textures[srcMat.stringIdx[j]].GetSRV();
        }

        // 关键 API：将 CPU 端的描述符拷贝到 GPU 可见的描述符堆中
        // 理由：CPU 写入描述符堆是昂贵的，批量拷贝 (Batch Copy) 比逐个设置效率更高
        g_Device->CopyDescriptors(1, &TextureHandles, &DestCount,
            DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // 第三步：处理采样器描述符 (Sampler Descriptor)
        uint32_t addressModes = srcMat.addressModes;
        auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

        if (samplerMapLookup == g_SamplerPermutations.end())
        {
            // 若该采样器组合未缓存，则在采样器堆中分配并拷贝
            DescriptorHandle SamplerHandles = Renderer::s_SamplerHeap.Alloc(kNumTextures);
            uint32_t SamplerDescriptorTable = Renderer::s_SamplerHeap.GetOffsetOfHandle(SamplerHandles);
            g_SamplerPermutations[addressModes] = SamplerDescriptorTable;

            // 编码 Offset：低 16 位存 SRV 偏移，高 16 位存 Sampler 偏移
            tableOffsets[matIdx] = SRVDescriptorTable | SamplerDescriptorTable << 16;

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
            tableOffsets[matIdx] = SRVDescriptorTable | samplerMapLookup->second << 16;
        }
    }

    // 第四步：将计算好的 Table Offset 回填到每个 Mesh 对象中
    uint8_t* meshPtr = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        Mesh& mesh = *(Mesh*)meshPtr;
        uint32_t offsetPair = tableOffsets[mesh.materialCBV];
        mesh.srvTable = offsetPair & 0xFFFF;
        mesh.samplerTable = offsetPair >> 16;
        // 根据 Mesh 的 PSO 标志位 (如是否带骨骼、是否透明) 关联对应的渲染管线状态
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        // 指针偏移，跳过变长的 Draw Call 数据区
        meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

// 外部调用接口：加载模型文件并处理缓存逻辑（没啃，暂留，后续啃）
std::shared_ptr<Model> Renderer::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
    // 1. 路径处理阶段
    // 推导引擎专用的二进制缓存文件后缀。通常这种文件不参与版本控制，由引擎的 DDC (Derived Data Cache) 局部生成。
    const std::wstring miniFileName = Utility::RemoveExtension(filePath) + L".ty";
    const std::wstring fileName = Utility::RemoveBasePath(filePath);

    // 声明文件状态结构体，用于获取文件的时间戳和大小。
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
    if (sourceFileMissing && miniFileMissing)
    {
        Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
        return nullptr;
    }

    bool needBuild = forceRebuild;

    // 【核心缓存逻辑】
    // 触发重建的条件：1. 缓存文件缺失。 2. 原始文件存在，且其最后修改时间 (st_mtime) 晚于缓存文件。
    // 这保证了美术人员在 DCC 软件 (如 Maya/Blender) 中修改并导出模型后，引擎能自动热重载 (Hot Reload / ホットリロード)。
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
            Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
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
    ASSERT(strncmp(header.id, "TY", 4) == 0 && header.version == CURRENT_MINI_FILE_VERSION);

    std::wstring basePath = Utility::GetBasePath(filePath);

    // 5. 内存分配与数据灌入 (Memory Allocation & Loading)
    // 实例化引擎层的 Model 对象。
    std::shared_ptr<Model> model(new Model);

    // 加载场景图层级树 (Scene Graph) - 主要用于骨骼动画和物件挂载。
    model->m_NumNodes = header.numNodes;
    model->m_SceneGraph.reset(new GraphNode[header.numNodes]);

    // CPU 侧保留一份 Mesh 元数据（如 Submesh 的边界、材质索引）。
    model->m_NumMeshes = header.numMeshes;
    model->m_MeshData.reset(new uint8_t[header.meshDataSize]);

    // 【DX12 核心：几何体数据上传】(Geometry Upload / ジオメトリアップロード)
    if (header.geometrySize > 0)
    {
        UploadBuffer modelData;
        // 硬件级行为：在 Upload Heap (UMA 架构或 PCIe 可见的系统内存) 中分配空间。属性为 Write-Combine，CPU 写入极快，但读取极慢。
        modelData.Create(L"Model Data Upload", header.geometrySize);

        // 直接将磁盘数据 Read 进这段映射好的物理内存中。
        inFile.read((char*)modelData.Map(), header.geometrySize);
        modelData.Unmap();

        // 硬件级行为：m_DataBuffer.Create 底层会申请一块 Default Heap (纯 GPU VRAM) 内存，
        // 录制一条 CopyBufferRegion 的 Command，将数据从 Upload Heap 经 PCIe 总线搬运到 Default Heap。
        // 避坑：别忘了底层在此之后必须插入 Resource Barrier 转换为 VERTEX_AND_CONSTANT_BUFFER 状态。
        model->m_DataBuffer.Create(L"Model Data", header.geometrySize, 1, modelData);
    }

    // 将刚才分配的 CPU 侧层级树和 Mesh 元数据填满。
    inFile.read((char*)model->m_SceneGraph.get(), header.numNodes * sizeof(GraphNode));
    inFile.read((char*)model->m_MeshData.get(), header.meshDataSize);

    // 【DX12 核心：材质常量上传】(Material Constant Buffer / マテリアル定数バッファ)
    if (header.numMaterials > 0)
    {
        UploadBuffer materialConstants;
        // 为所有材质参数（如 Albedo, Roughness, Metallic 的数值）分配 Upload Buffer。
        materialConstants.Create(L"Material Constant Upload", header.numMaterials * sizeof(MaterialConstants));
        MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();

        // 循环读取。在工业界，如果是严密的对齐数据，这里通常会优化为一个整块的 block read 而非循环单次 read，以减少函数调用开销。
        for (uint32_t i = 0; i < header.numMaterials; ++i)
        {
            inFile.read((char*)materialCBV, sizeof(MaterialConstantData));
            materialCBV++; // 指针偏移，写入下一个材质参数
        }
        materialConstants.Unmap();

        // 同样，拷贝到 GPU 专属的 VRAM 中。
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
    LoadMaterials(*model, materialTextures, textureNames, textureOptions, basePath);

    // 7. 包围体数据提取 (Bounding Volume Data)
    // 用于视锥体剔除 (Frustum Culling / フラスタムカリング)。如果包围盒不在相机的投影矩阵范围内，整个模型将被剔除，直接省去 Draw Call 提交。


    // 8. 动画与骨骼系统 (Animation & Skeleton System)


    // 顺利通关，返回模型指针。
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
