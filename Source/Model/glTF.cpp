#include "glTF.h"

#include "../RHI/Command/CommandContext.h"
#include "../RHI/PipelineState/samplerManager.h"
#include "../Resource/Buffer/UploadBuffer.h"
#include "../RHI/GraphicsCore.h"
#include "../Utility/FileUtility.h"


#include <fstream>
#include <iostream>

using namespace glTF;
using namespace Graphics;
using namespace Utility;

void ReadFloats(json& list, float flt_array[])
{
    uint32_t i = 0;
    for (auto& flt : list)
        flt_array[i++] = flt;
}

void glTF::Asset::ProcessNodes(json& nodes)
{
    m_nodes.resize(nodes.size());
    uint32_t nodeIdx = 0;

    for (json::iterator it = nodes.begin(); it != nodes.end(); ++it)
    {
        glTF::Node& node = m_nodes[nodeIdx++];
        json& thisNode = it.value();

        node.flags = 0;
        node.mesh = nullptr;
        node.linearIdx = -1;


        if (thisNode.find("mesh") != thisNode.end())
        {
            node.mesh = &m_meshes[thisNode.at("mesh")];
        }

        if (thisNode.find("children") != thisNode.end())
        {
            json& children = thisNode["children"];
            node.children.reserve(children.size());
            for (auto& child : children)
                node.children.push_back(&m_nodes[child]);
        }
        // 有矩阵就读取，无则套用默认。scale, rotation, translation
        if (thisNode.find("matrix") != thisNode.end())
        {
            // TODO:  Should check for negative determinant to reverse triangle winding
            ReadFloats(thisNode["matrix"], node.matrix);
            node.hasMatrix = true;
        }
        else
        {
            // TODO:  Should check scale for 1 or 3 negative values to reverse triangle winding
            json::iterator scale = thisNode.find("scale");
            if (scale != thisNode.end())
            {
                ReadFloats(scale.value(), node.scale);
            }
            else
            {
                node.scale[0] = 1.0f;
                node.scale[1] = 1.0f;
                node.scale[2] = 1.0f;
            }

            json::iterator rotation = thisNode.find("rotation");
            if (rotation != thisNode.end())
            {
                ReadFloats(rotation.value(), node.rotation);
            }
            else
            {
                node.rotation[0] = 0.0f;
                node.rotation[1] = 0.0f;
                node.rotation[2] = 0.0f;
                node.rotation[3] = 1.0f;
            }

            json::iterator translation = thisNode.find("translation");
            if (translation != thisNode.end())
            {
                ReadFloats(translation.value(), node.translation);
            }
            else
            {
                node.translation[0] = 0.0f;
                node.translation[1] = 0.0f;
                node.translation[2] = 0.0f;
            }
        }
    }
}

void glTF::Asset::ProcessScenes(json& scenes)
{
    m_scenes.reserve(scenes.size());

    for (json::iterator it = scenes.begin(); it != scenes.end(); ++it)
    {
        glTF::Scene scene;
        json& thisScene = it.value();

        if (thisScene.find("nodes") != thisScene.end())
        {
            json& nodes = thisScene["nodes"];
            scene.nodes.reserve(nodes.size());
            for (auto& node : nodes)
                scene.nodes.push_back(&m_nodes[node]);
        }

        m_scenes.push_back(scene);
    }
}

// 类型转Accessor枚举类型
uint16_t TypeToEnum(const char type[])
{
    if (strncmp(type, "VEC", 3) == 0)
        return Accessor::kVec2 + type[3] - '2'; // ASCII码偏移得出枚举值
    else if (strncmp(type, "MAT", 3) == 0)
        return Accessor::kMat2 + type[3] - '2';
    else
        return Accessor::kScalar;               // 标量
}

void glTF::Asset::ProcessAccessors(json& accessors)
{
    m_accessors.reserve(accessors.size());

    for (json::iterator it = accessors.begin(); it != accessors.end(); ++it)
    {
        glTF::Accessor accessor;
        json& thisAccessor = it.value();

        glTF::BufferView& bufferView = m_bufferViews[thisAccessor.at("bufferView")];
        accessor.dataPtr = m_buffers[bufferView.buffer]->data() + bufferView.byteOffset;
        accessor.stride = bufferView.byteStride;
        if (thisAccessor.find("byteOffset") != thisAccessor.end())
            accessor.dataPtr += thisAccessor.at("byteOffset");
        accessor.count = thisAccessor.at("count");
        accessor.componentType = thisAccessor.at("componentType").get<uint16_t>() - 5120;

        char type[8];
        strcpy_s(type, thisAccessor.at("type").get<std::string>().c_str());

        accessor.type = TypeToEnum(type);

        m_accessors.push_back(accessor);
    }
}

void glTF::Asset::FindAttribute(Primitive& prim, json& attributes, Primitive::eAttribType type, const string& name)
{
    json::iterator attrib = attributes.find(name);
    if (attrib != attributes.end())
    {
        prim.attribMask |= 1 << type; // 位操作，存储type
        prim.attributes[type] = &m_accessors[attrib.value()];
    }
    else
    {
        prim.attributes[type] = nullptr;
    }
}

// 读取gltf的mesh数组节点，和accessors数组节点（用于查找具体数据视图）
void glTF::Asset::ProcessMeshes(json& meshes, json& accessors)
{

    m_meshes.resize(meshes.size()); // 本地mesh数组预分配内存，避免 std::vector 在解析过程中频繁动态扩容，导致内存碎片

    uint32_t curMesh = 0;
    // 遍历外层所有的 Mesh 对象
    for (json::iterator meshIt = meshes.begin(); meshIt != meshes.end(); ++meshIt, ++curMesh)
    {
        json& thisMesh = meshIt.value(); // 获取当前 Mesh 的 JSON 节点
        // 获取 "primitives" 数组。glTF 规范中，一个 Mesh 必须至少包含一个 Primitive
        json& primitives = thisMesh.at("primitives");

        m_meshes[curMesh].primitives.resize(primitives.size());  // 为当前 Mesh 预分配它所包含的子图元 (SubMesh) 的内存

        uint32_t curSubMesh = 0;
        // 遍历该 Mesh 的所有 Primitive（每个 Primitive 都会产生一个 DrawCall）
        for (json::iterator primIt = primitives.begin(); primIt != primitives.end(); ++primIt, ++curSubMesh)
        {
            // 获取对应 C++ 内存块的引用，准备写入数据
            glTF::Primitive& prim = m_meshes[curMesh].primitives[curSubMesh];
            json& thisPrim = primIt.value();

            // 这是一个极其关键的位掩码 (Bitmask)！
            // 底层渲染器通过检查这个 Mask（例如 (mask & (1 << kNormal))）
            // 就能在 O(1) 时间内知道这个模型有没有法线，从而去匹配正确的 根签名 (Root Signature) 和 顶点着色器 (Vertex Shader)
            prim.attribMask = 0;

            // "attributes" 节点是一个字典，记录了 POSITION, NORMAL 等属性对应的 Accessor 索引
            json& attributes = thisPrim.at("attributes");

            // 查找并绑定各个 顶点属性 (Vertex Attributes / 頂点属性)
            // 这些属性最终会组装成 DX12 的 D3D12_INPUT_ELEMENT_DESC 数组，交给 Input Assembler (IA) 阶段
            FindAttribute(prim, attributes, Primitive::kPosition, "POSITION");
            FindAttribute(prim, attributes, Primitive::kNormal, "NORMAL");
            FindAttribute(prim, attributes, Primitive::kTangent, "TANGENT");
            FindAttribute(prim, attributes, Primitive::kTexcoord0, "TEXCOORD_0");
            FindAttribute(prim, attributes, Primitive::kTexcoord1, "TEXCOORD_1");
            FindAttribute(prim, attributes, Primitive::kColor0, "COLOR_0");

            // 默认设置为 DX12 的 Triangle List 拓扑结构 (每 3 个顶点独立构成一个三角形)
            prim.mode = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            prim.indices = nullptr;   // 初始化索引访问器为空
            prim.material = nullptr;  // 初始化材质指针为空
            prim.minIndex = 0;        // 索引的最小边界
            prim.maxIndex = 0;        // 索引的最大边界
            prim.mode = 4;            // 4 在 glTF 规范中代表 TRIANGLES（如果不指定，默认就是 4）

            // 检查 glTF 是否显式指定了绘制模式（例如点云 0、线框 1、三角带 5 等）
            if (thisPrim.find("mode") != thisPrim.end())
                prim.mode = thisPrim.at("mode");

            // 检查是否使用了 索引缓冲区 (Index Buffer / インデックスバッファ)
            // 绝大多数情况都有索引，用于顶点复用，极大提升 GPU Vertex Cache 的命中率
            if (thisPrim.find("indices") != thisPrim.end())
            {
                // 获取指向 Accessor 的索引
                uint32_t accessorIndex = thisPrim.at("indices");
                json& indicesAccessor = accessors[accessorIndex];

                // 将指针指向引擎预先解析好的 Accessor 数组中的对应元素
                prim.indices = &m_accessors[accessorIndex];

                // 【底层逻辑】：读取索引的 Min 和 Max。
                // 这在 DX12 中非常重要！它可以用于计算顶点缓冲区的有效读取范围 (Vertex Fetch Range)。
                // 引擎可以根据这个范围，在 CPU 端进行包围盒剔除，或者限制 GPU 显存拷贝的尺寸，防止越界崩溃。
                if (indicesAccessor.find("max") != indicesAccessor.end())
                    prim.maxIndex = indicesAccessor.at("max")[0];
                if (indicesAccessor.find("min") != indicesAccessor.end())
                    prim.minIndex = indicesAccessor.at("min")[0];
            }

            // 绑定材质。不同的材质意味着在绘制这个 Primitive 前，必须切换一次 PSO 或 Descriptor Table
            if (thisPrim.find("material") != thisPrim.end())
                prim.material = &m_materials[thisPrim.at("material")];
        }
    }
}

// 转半精度float（没啃）
inline uint32_t floatToHalf(float f)
{
    const float kF32toF16 = (1.0 / (1ull << 56)) * (1.0 / (1ull << 56)); // 2^-112
    union { float f; uint32_t u; } x;
    x.f = Math::Clamp(f, 0.0f, 1.0f) * kF32toF16;
    return x.u >> 13;
}

uint32_t glTF::Asset::ReadTextureInfo(json& info_json, glTF::Texture*& info)
{
    info = nullptr;

    // 读取纹理
    if (info_json.find("index") != info_json.end())
        info = &m_textures[info_json.at("index")];
    // 读取纹理坐标
    if (info_json.find("texCoord") != info_json.end())
        return info_json.at("texCoord");
    else
        return 0;
}

void glTF::Asset::ProcessMaterials(json& materials)
{
    m_materials.reserve(materials.size()); // 预留内存 

    uint32_t materialIdx = 0;

    for (json::iterator it = materials.begin(); it != materials.end(); ++it)
    {
        glTF::Material material;
        json& thisMaterial = it.value();

        material.index = materialIdx++;
        material.flags = 0;
        material.alphaCutoff = floatToHalf(0.5f);
        material.normalTextureScale = 1.0f;
        // 模型材质存在alphaMode吗？
        if (thisMaterial.find("alphaMode") != thisMaterial.end())
        {
            string alphaMode = thisMaterial.at("alphaMode");
            if (alphaMode == "BLEND")
                material.alphaBlend = true;
            else if (alphaMode == "MASK")
                material.alphaTest = true;
        }
        // 模型材质存在alphaCutoff吗？
        if (thisMaterial.find("alphaCutoff") != thisMaterial.end())
        {
            material.alphaCutoff = floatToHalf(thisMaterial.at("alphaCutoff"));
            //material.alphaTest = true;  // Should we alpha test and alpha blend?
        }
        // 模型材质PBR读取
        if (thisMaterial.find("pbrMetallicRoughness") != thisMaterial.end())
        {
            json& metallicRoughness = thisMaterial.at("pbrMetallicRoughness");

            material.baseColorFactor[0] = 1.0f; // R
            material.baseColorFactor[1] = 1.0f; // G
            material.baseColorFactor[2] = 1.0f; // B
            material.baseColorFactor[3] = 1.0f; // A
            material.metallicFactor = 1.0f;     // 默认全金属
            material.roughnessFactor = 1.0f;    // 默认最粗糙
            for (uint32_t i = 0; i < Material::kNumTextures; ++i) // 清空本地纹理指针数组
                material.textures[i] = nullptr;

            if (metallicRoughness.find("baseColorFactor") != metallicRoughness.end())
                ReadFloats(metallicRoughness.at("baseColorFactor"), material.baseColorFactor);

            if (metallicRoughness.find("metallicFactor") != metallicRoughness.end())
                material.metallicFactor = metallicRoughness.at("metallicFactor");

            if (metallicRoughness.find("roughnessFactor") != metallicRoughness.end())
                material.roughnessFactor = metallicRoughness.at("roughnessFactor");

            if (metallicRoughness.find("baseColorTexture") != metallicRoughness.end())
                material.baseColorUV = ReadTextureInfo(metallicRoughness.at("baseColorTexture"),
                    material.textures[Material::kBaseColor]);

            if (metallicRoughness.find("metallicRoughnessTexture") != metallicRoughness.end())
                material.metallicRoughnessUV = ReadTextureInfo(metallicRoughness.at("metallicRoughnessTexture"),
                    material.textures[Material::kMetallicRoughness]);
        }

        // 双面渲染。如果开启，DX12 光栅化器状态 (Rasterizer State) 中的 CullMode 必须设为 D3D12_CULL_MODE_NONE
        if (thisMaterial.find("doubleSided") != thisMaterial.end())
            material.twoSided = thisMaterial.at("doubleSided");

        // 法线贴图
        if (thisMaterial.find("normalTextureScale") != thisMaterial.end())
            material.normalTextureScale = thisMaterial.at("normalTextureScale");

        // 自发光材质factor，影响 Bloom 后处理
        if (thisMaterial.find("emissiveFactor") != thisMaterial.end())
            ReadFloats(thisMaterial.at("emissiveFactor"), material.emissiveFactor);

        // AO贴图
        if (thisMaterial.find("occlusionTexture") != thisMaterial.end())
            material.occlusionUV = ReadTextureInfo(thisMaterial.at("occlusionTexture"),
                material.textures[Material::kOcclusion]);

        // 绑定自发光与法线纹理
        if (thisMaterial.find("emissiveTexture") != thisMaterial.end())
            material.emissiveUV = ReadTextureInfo(thisMaterial.at("emissiveTexture"),
                material.textures[Material::kEmissive]);

        if (thisMaterial.find("normalTexture") != thisMaterial.end())
            material.normalUV = ReadTextureInfo(thisMaterial.at("normalTexture"),
                material.textures[Material::kNormal]);

        m_materials.push_back(material);
    }
}

bool ReadFile(const wstring& fileName, void* Dest, size_t Size)
{
    // 现代操作系统（如 Windows 的 NTFS 文件系统）中，获取文件元数据（大小、修改时间）和真正打开文件流是两套不同的底层机制。
    // 直接读取文件分配表（MFT）获取大小，比建立完整的文件流句柄再把指针移到末尾要快得多，也更安全。
    struct _stat64 fileStat;

    // 2. 操作系统系统调用 (System Call / システムコール)
    // _wstat64 会触发一次上下文切换 (Context Switch)，陷入内核态。
    // OS 会去查询磁盘文件系统的元数据表 (如 NTFS 的 Master File Table)，而不实际加载文件数据。
    int fileExists = _wstat64(fileName.c_str(), &fileStat);

    if (fileExists == -1)
        return false;

    // 4. 创建 C++ 标准文件输入流。
    // 【核心机制】：ios::binary 极其重要！如果不加这个标志，C++ 会以文本模式打开文件。
    // 在文本模式下，Windows 会在底层偷偷把所有的回车换行符 (0x0D 0x0A) 转换成单个换行符 (0x0A)。
    // 这会导致你的顶点数据或纹理二进制数据被破坏，引发极其诡异的渲染错乱！
    ifstream file(fileName, ios::in | ios::binary);

    if (!file)
        return false;

    // 6. 内存越界安全断言 (Memory Safety Assertion)
    // 这是引擎开发中最重要的一环。调用者在调用此函数前，必须已经知道了文件有多大，并分配了 `Size` 大小的内存。
    // 如果实际磁盘文件的大小 `fileStat.st_size` 与预分配的大小不符，强行 read 会导致 缓冲区溢出 (Buffer Overflow / バッファオーバーフロー)。
    ASSERT(Size == (size_t)fileStat.st_size);

    // 7. 阻塞式磁盘拷贝 (Blocking Disk Copy)
    // 这是真正发生 I/O 耗时的操作。数据流向：
    // 硬盘 -> OS 页面缓存 (Page Cache) -> user 态的 Dest 内存块。
    file.read((char*)Dest, Size);

    // 8. 释放系统文件句柄。如果不关闭，会导致文件句柄泄漏 (Handle Leak)，其他程序将无法修改此文件。
    file.close();

    return true;
}

// 传入解析好的 "buffers" JSON 数组节点，以及提前读取好的 GLB Chunk 1 二进制块
void glTF::Asset::ProcessBuffers(json& buffers, ByteArray chunk1bin)
{
    // 1. 预分配内存 (Memory Pre-allocation)
    m_buffers.reserve(buffers.size());

    for (json::iterator it = buffers.begin(); it != buffers.end(); ++it)
    {
        json& thisBuffer = it.value();

        // 2. 检查 uri 字段：判断是 .gltf 还是 .glb
        // 如果存在 "uri" 字段，说明这是一个分离式的 .gltf 文件，数据在外部。
        if (thisBuffer.find("uri") != thisBuffer.end())
        {
            const string& uri = thisBuffer.at("uri");
            // 将 ASCII 的 URI 字符串转换为 UTF-16 的 wstring，并拼接基础路径。
            // 宽字符 (Wide Character) 是 Windows API (如 CreateFileW) 处理包含日文/中文文件路径的硬性要求。
            wstring filepath = m_basePath + wstring(uri.begin(), uri.end());

            // 发起同步磁盘 I/O (Disk I/O)，将整个 .bin 文件读入内存，返回一个智能指针 (std::shared_ptr<vector<byte>>)
            ByteArray ba = ReadFileSync(filepath);

            // 契约编程 (Design by Contract)：确保文件真的读到了数据，否则直接触发断言崩溃，阻止脏数据进入后续渲染管线。
            ASSERT(ba->size() > 0, "Missing bin file %ws", filepath.c_str());

            // 将指向这块堆内存的智能指针压入引擎的缓冲数组中
            m_buffers.push_back(ba);
        }
        else
        {
            // 3. 进入 .glb (GL Binary) 打包模式逻辑
            // glTF 2.0 规范严格要求：如果 Buffer 没有 uri，那它必须且只能是 buffers 数组中的【第一个】元素 (index 0)！
            // 因为 GLB 文件头后面紧跟的 Chunk 0 是 JSON，Chunk 1 就是那个唯一的、全局的二进制块。
            ASSERT(it == buffers.begin(), "Only the 1st buffer allowed to be internal");

            // 确保解析外层 GLB 结构时，成功读取了 Chunk 1
            ASSERT(chunk1bin->size() > 0, "GLB chunk1 missing data or not a GLB file");

            // 直接复用传入的二进制块，零拷贝 (Zero-copy) 思想的体现
            m_buffers.push_back(chunk1bin);
        }
    }
}

void glTF::Asset::ProcessBufferViews(json& bufferViews)
{
    // [底层逻辑] 预分配内存。
    m_bufferViews.reserve(bufferViews.size());

    for (json::iterator it = bufferViews.begin(); it != bufferViews.end(); ++it)
    {
        glTF::BufferView bufferView;
        json& thisBufferView = it.value();

        // 1. 必填字段提取
        // buffer: 指向底层的 Buffer 索引 (告诉引擎去哪块 ID3D12Resource 拿数据)
        // byteLength: 视图的总字节数 (对应 D3D12_VERTEX_BUFFER_VIEW::SizeInBytes)
        bufferView.buffer = thisBufferView.at("buffer");
        bufferView.byteLength = thisBufferView.at("byteLength");

        // 2. 赋默认值
        bufferView.byteOffset = 0;
        bufferView.byteStride = 0;
        bufferView.elementArrayBuffer = false; // 默认视为 顶点缓冲区 (Vertex Buffer / 頂点バッファ)

        // 3. 可选字段提取：byteOffset
        // [硬件映射] 该字段决定了 D3D12_VERTEX_BUFFER_VIEW::BufferLocation = Resource->GetGPUVirtualAddress() + byteOffset。
        if (thisBufferView.find("byteOffset") != thisBufferView.end())
            bufferView.byteOffset = thisBufferView.at("byteOffset");

        // 4. 可选字段提取：byteStride
        // [硬件映射] 对应 D3D12_VERTEX_BUFFER_VIEW::StrideInBytes。
        // 现代 GPU 架构中，Warp/Wavefront 内的线程在执行 Vertex Shader 时，如果 Pos/Normal/UV 是交错紧凑排列的，
        // 一个 128-byte 的 Cache Line 抓取可以同时命中多个属性，大幅提升 缓存命中率 (Cache Hit Rate / キャッシュヒット率)。
        if (thisBufferView.find("byteStride") != thisBufferView.end())
            bufferView.byteStride = thisBufferView.at("byteStride");

        // 5. Target 判定：34962 = ARRAY_BUFFER (顶点);  34963 = ELEMENT_ARRAY_BUFFER (索引)
        // [历史遗留] 这里的魔数 34963 实际上是 OpenGL 的 GL_ELEMENT_ARRAY_BUFFER 宏的值。
        // glTF 作为 WebGL/OpenGL 生态起步的标准，保留了这个魔数。
        // 在 DX12 中，这决定了该 View 最终是绑定到 IASetIndexBuffer 还是 IASetVertexBuffers。
        // 如果是 true，后续还需要结合 Accessor 解析出它是 DXGI_FORMAT_R16_UINT 还是 DXGI_FORMAT_R32_UINT 索引。
        if (thisBufferView.find("target") != thisBufferView.end() && thisBufferView.at("target") == 34963)
            bufferView.elementArrayBuffer = true;

        m_bufferViews.push_back(bufferView);
    }
}

void glTF::Asset::ProcessImages(json& images)
{
    m_images.resize(images.size());

    uint32_t imageIdx = 0;

    for (json::iterator it = images.begin(); it != images.end(); ++it)
    {
        json& thisImage = it.value();
        if (thisImage.find("uri") != thisImage.end()) // 外部资源uri：IO寻址
        {
            m_images[imageIdx++].path = thisImage.at("uri").get<string>();
        }
        else if (thisImage.find("bufferView") != thisImage.end())
        {
            Utility::Printf("GLB image at buffer view %d with mime type %s\n", thisImage.at("bufferView").get<uint32_t>(),
                thisImage.at("mimeType").get<string>().c_str());
        }
        else
        {
            ASSERT(0);
        }
    }
}

D3D12_TEXTURE_ADDRESS_MODE GLtoD3DTextureAddressMode(int32_t glWrapMode)
{
    switch (glWrapMode)
    {
    default: ERROR("Unexpected sampler wrap mode");
    case 33071: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 33648: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 10497: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

/*
D3D12_FILTER GLtoD3DTextureFilterMode( int32_t magFilter, int32_t minFilter )
{
    bool linearMag = magFilter == 9729;
    switch (minFilter)
    {
    case 9728: //nearest
    case 9984: return linearMag ? D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;//nearest_mipmap_nearest
    case 9729: //linear
    case 9987: return linearMag ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;//linear_mipmap_linear
    case 9985: return linearMag ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;//linear_mipmap_nearest
    case 9986: break;//nearest_mipmap_linear
    }
}
*/

void glTF::Asset::ProcessSamplers(json& samplers)
{
    m_samplers.resize(samplers.size());

    uint32_t samplerIdx = 0;

    for (json::iterator it = samplers.begin(); it != samplers.end(); ++it)
    {
        json& thisSampler = it.value();

        glTF::Sampler& sampler = m_samplers[samplerIdx++];

        // 3. 强制覆盖过滤模式：代码此处选择“硬编码”为各向异性过滤
        // 理由：glTF 原生支持的是基于 WebGL 的线性过滤，但在高性能渲染器中，
        // 为了视觉质量，开发者通常倾向于忽略资产建议，统一开启各向异性过滤。
        sampler.filter = D3D12_FILTER_ANISOTROPIC;

        // 4. 设置默认寻址模式：默认设为 Wrap（重复模式）
        sampler.wrapS = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.wrapT = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

        /*
        // Who cares what is provided?  It's about what you can afford, generally
        // speaking about materials.  If you want anisotropic filtering, why let
        // the asset dictate that.  And AF isn't represented in WebGL, so blech.
        int32_t magFilter = 9729;
        int32_t minFilter = 9987;
        if (thisSampler.find("magFilter") != thisSampler.end())
            magFilter = thisSampler.at("magFilter");
        if (thisSampler.find("minFilter") != thisSampler.end())
            minFilter = thisSampler.at("minFilter");
        sampler.filter = GLtoD3DTextureFilterMode(magFilter, minFilter);
        */

        // 5. 寻址模式覆盖：若资产中明确指定了 wrapS/T (U/V 轴)，则通过转换函数映射到 DX12 枚举
        // 注意：glTF 的 wrapS 对应 DX12 的 AddressU，wrapT 对应 AddressV
        if (thisSampler.find("wrapS") != thisSampler.end())
            sampler.wrapS = GLtoD3DTextureAddressMode(thisSampler.at("wrapS"));
        if (thisSampler.find("wrapT") != thisSampler.end())
            sampler.wrapT = GLtoD3DTextureAddressMode(thisSampler.at("wrapT"));
    }
}

void glTF::Asset::ProcessTextures(json& textures)
{
    m_textures.resize(textures.size());

    uint32_t texIdx = 0;

    for (json::iterator it = textures.begin(); it != textures.end(); ++it)
    {
        glTF::Texture& texture = m_textures[texIdx++];
        json& thisTexture = it.value();

        texture.source = nullptr;
        texture.sampler = nullptr;

        // 4. 建立 Image 引用
        // 逻辑推导：glTF 使用索引 (Index) 引用。在调用此函数前，m_images 必须已解析完成。
        if (thisTexture.find("source") != thisTexture.end())
            texture.source = &m_images[thisTexture.at("source")];

        // 5. 建立 Sampler 引用
        // 如果 glTF 中未定义 sampler，则在渲染时通常需要回退到引擎默认的采样设置。
        if (thisTexture.find("sampler") != thisTexture.end())
            texture.sampler = &m_samplers[thisTexture.at("sampler")];
    }
}



void glTF::Asset::Parse(const std::wstring& filepath)
{

    ByteArray gltfFile;
    ByteArray chunk1Bin;

    // 统一转为小写后缀进行判定
    std::wstring fileExt = Utility::ToLower(Utility::GetFileExtension(filepath));

    if (fileExt == L"glb")
    {
        ifstream glbFile(filepath, ios::in | ios::binary);

        // 1. 读取 GLB 文件头 (12 字节)
        struct GLBHeader
        {
            char magic[4];     // 必须为 "glTF"
            uint32_t version;  // 协议版本
            uint32_t length;   // 文件总长度
        } header;

        glbFile.read((char*)&header, sizeof(GLBHeader));

        // 验证合法性
        if (strncmp(header.magic, "glTF", 4) != 0)
        {
            Utility::Printf("Error: Invalid glTF binary format\n");
            return;
        }
        if (header.version != 2)
        {
            Utility::Printf("Error: Only glTF 2.0 is supported\n");
            return;
        }

        // 2. 读取 Chunk 0: JSON 数据块
        uint32_t chunk0Length;
        char chunk0Type[4];
        glbFile.read((char*)&chunk0Length, 4);
        glbFile.read((char*)&chunk0Type, 4);

        if (strncmp(chunk0Type, "JSON", 4) != 0)
        {
            Utility::Printf("Error: Expected chunk0 to contain JSON\n");
            return;
        }

        // 为 JSON 字符串分配空间并读取内容 (+1 用于存放 '\0')
        gltfFile = make_shared<vector<std::byte>>(chunk0Length + 1);
        glbFile.read((char*)gltfFile->data(), chunk0Length);
        (*gltfFile)[chunk0Length] = std::byte{ 0 };

        // 3. 读取 Chunk 1: BIN 二进制数据块
        uint32_t chunk1Length;
        char chunk1Type[4];
        glbFile.read((char*)&chunk1Length, 4);
        glbFile.read((char*)&chunk1Type, 4);

        if (strncmp(chunk1Type, "BIN", 3) != 0)
        {
            Utility::Printf("Error: Expected chunk1 to contain BIN\n");
            return;
        }

        // 将原始二进制数据存入 chunk1Bin
        chunk1Bin = make_shared<vector<std::byte>>(chunk1Length);
        glbFile.read((char*)chunk1Bin->data(), chunk1Length);
    }
    else
    {
        // 处理传统的 .gltf (JSON) 文件
        ASSERT(fileExt == L"gltf");

        gltfFile = ReadFileSync(filepath);
        if (gltfFile->size() == 0)
            return;

        // 确保以 Null 结尾，防止 JSON 解析器溢出
        gltfFile->push_back(std::byte{ 0 });

        // 文本模式下，二进制数据通常存储在外部 .bin 文件中，初次解析时不加载
        chunk1Bin = make_shared<vector<std::byte>>(0);
    }

    // 4. 解析 JSON 对象树
    json root = json::parse((const char*)gltfFile->data());
    if (!root.is_object())
    {
        Printf("Invalid glTF file: %s\n", filepath.c_str());
        return;
    }

    // 获取文件根目录，用于定位外部依赖资源（如贴图）
    m_basePath = Utility::GetBasePath(filepath);

    // 5. 按照拓扑依赖顺序解析各个状态组件
    if (root.find("buffers") != root.end())
        ProcessBuffers(root.at("buffers"), chunk1Bin);

    if (root.find("bufferViews") != root.end())
        ProcessBufferViews(root.at("bufferViews"));

    if (root.find("accessors") != root.end())
        ProcessAccessors(root.at("accessors"));

    if (root.find("images") != root.end())
        ProcessImages(root.at("images"));

    if (root.find("samplers") != root.end())
        ProcessSamplers(root.at("samplers"));

    if (root.find("textures") != root.end())
        ProcessTextures(root.at("textures"));

    if (root.find("materials") != root.end())
        ProcessMaterials(root.at("materials"));

    if (root.find("meshes") != root.end())
        ProcessMeshes(root.at("meshes"), root.at("accessors"));

    if (root.find("nodes") != root.end())
        ProcessNodes(root.at("nodes"));



    if (root.find("scenes") != root.end())
        ProcessScenes(root.at("scenes"));



    // 设置默认场景
    if (root.find("scene") != root.end())
        m_scene = &m_scenes[root.at("scene")];
}
