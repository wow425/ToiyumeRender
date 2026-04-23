#pragma once

#include "../Utility//FileUtility.h"
#include "../Math/VectorMath.h"


#ifndef _WIN32
#define _WIN32
#endif
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#include "json.hpp"
#pragma warning(pop)

#include <string>

// 定位是一个完整的场景描述格式 (Scene Description Format)。它不仅能容纳单个模型，还能打包整个数字世界的信息。
// glTF 2.0 的官方标准中，贴图系统是由三部分组成的：Image，Sampler，Texture
namespace glTF
{
    using json = nlohmann::json;
    using Utility::ByteArray;

    struct BufferView
    {
        uint32_t buffer;
        uint32_t byteLength;
        uint32_t byteOffset;
        uint16_t byteStride;
        bool     elementArrayBuffer;
    };

    // 访问器，解析glTF模型文件的数据结构，充当解码器，解释二进制字节流
    struct Accessor
    {
        enum // 组件类型
        {
            kByte,
            kUnsignedByte,
            kShort,
            kUnsignedShort,
            kSignedInt, // 不会发生
            kUnsignedInt,
            kFloat
        };

        enum // 类型
        {
            kScalar,
            kVec2,
            kVec3,
            kVec4,
            kMat2,
            kMat3,
            kMat4
        };

        std::byte* dataPtr;   // buffer指针
        uint32_t stride; // 元素步长
        uint32_t count;  // 元素数量
        uint16_t componentType; // 元素组件类型
        uint16_t type;          // 元素类型


        // 不这么做是为了数据访问扁平化(Data Flattening) 和 实用主义至上(YAGNI - You Aren't Gonna Need It)。
        // 是什么与为什么
        // 
        // 在官方的 glTF 2.0 规范中，如果要定位一个顶点的数据，逻辑是非常繁琐的树状引用：
        // Accessor(访问器)->包含 byteOffset->指向 BufferView(缓冲视图)->包含 byteOffset->指向 Buffer(底层二进制文件)。
        //如果严格按照规范定义结构体，每次读取数据时，CPU 都要经过三次指针跳转和两次加法运算。在底层硬件层面，这种由于数据结构在内存中不连续而导致的连续间接寻址，
        // 被称为指针追逐(Pointer Chasing / ポインタチェイシング)。它是 CPU 缓存未命中(Cache Miss) 的万恶之源。
        // 
        //怎么做(引擎的优化思路)
        //MiniEngine 的作者（微软的高级图形工程师）直接把规范里的这俩字段注释掉了，替换成了一个绝对内存指针 byte * dataPtr。
        //
        // 底层逻辑：空间换时间与预计算。
        //  引擎在加载阶段(Load Time) 就在 CPU 端把所有的层级偏移量全部展开，直接算出了数据在 RAM 中的真实物理起始地址 dataPtr。
        //  这样在后续真正的模型构建循环中，CPU 只需要 dataPtr + i * stride 就能以 $O(1)$ 的复杂度、0 间接跳转的方式极速遍历顶点。
        //BufferView* bufferView;
        //uint32_t byteOffset; offset from start of buffer view




        // Nobody is doing this in the samples.  Seems dumb.
        //uint32_t sparseCount;   // Number of sparse elements
        //uint32_t sparseIndices; // Buffer view for indices into sparse data
        //uint32_t sparseValues;  // Buffer view for packed array of sparse values
    };

    // 贴图
    struct Image
    {
        std::string path; // UTF8
    };
    struct Sampler
    {
        // 定义了 GPU 应该如何“读取”这张图像的数学过滤规则
        D3D12_FILTER filter;
        D3D12_TEXTURE_ADDRESS_MODE wrapS;
        D3D12_TEXTURE_ADDRESS_MODE wrapT;
    };
    struct Texture
    {
        Image* source;
        Sampler* sampler;
    };

    struct Material
    {
        struct
        {
            float baseColorFactor[4]; // default=[1,1,1,1]
            float metallicFactor; // default=1
            float roughnessFactor; // default=1
        };

        union
        {
            uint32_t flags;
            // 材质状态掩码material state mask，用于标记材质存在哪些状态
            struct // 刚好32位，4字节
            {
                uint32_t baseColorUV : 1;
                uint32_t metallicRoughnessUV : 1;
                uint32_t occlusionUV : 1;
                uint32_t emissiveUV : 1;
                uint32_t normalUV : 1;
                uint32_t twoSided : 1; // 需不需要双面渲染（关闭背面剔除）？
                uint32_t alphaTest : 1; // 需不需要开启透明度测试（像树叶那样的镂空边缘）？
                uint32_t alphaBlend : 1;
                uint32_t _pad : 8;
                uint32_t alphaCutoff : 16;
            };
        };
        float emissiveFactor[3];
        float normalTextureScale;
        enum { kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal, kNumTextures };
        Texture* textures[kNumTextures];
        uint32_t index;
    };

    // 图元，最小的可绘制单元。Model->多Mesh->多Primitive
    struct Primitive
    {

        enum eAttribType { kPosition, kNormal, kTangent, kTexcoord0, kTexcoord1, kColor0, kNumAttribs };
        Accessor* attributes[kNumAttribs]; // 顶点属性
        Accessor* indices;  // 索引
        Material* material; // 材质
        uint16_t attribMask; // 顶点属性掩码图，用位运算便可O(1)内知晓包含哪些属性，不用for遍历
        uint16_t mode;      // 绘制模式D3D_PRIMITIVE_TOPOLOGY
        // 索引范围边界
        uint32_t minIndex;
        uint32_t maxIndex;
    };

    struct Mesh
    {
        std::vector<Primitive> primitives;
        // int32_t skin;
    };

    // struct Camera {};

    struct Node
    {
        union // flags掩码图
        {
            uint8_t flags;
            struct
            {
                bool hasMatrix : 1;
            };
        };
        union
        {
            Mesh* mesh;
            // Camera* camera;
        };
        union // 变换矩阵
        {
            alignas(16) float matrix[16];
            struct
            {
                alignas(16) float scale[3];
                alignas(16) float rotation[4];
                alignas(16) float translation[3];
            };
        };
        std::vector<Node*> children;
        int32_t linearIdx; // assists with mapping scene nodes to flat arrays
    };

    struct Scene
    {
        std::vector<Node*> nodes;
    };

    class Asset
    {
    public:
        Asset() : m_scene(nullptr) {}
        Asset(const std::wstring& filepath) : m_scene(nullptr) { Parse(filepath); }
        ~Asset() { m_meshes.clear(); }

        void Parse(const std::wstring& filepath);

        Scene* m_scene;
        std::wstring m_basePath;
        std::vector<Image> m_images;

        // 1
        std::vector<Scene> m_scenes;
        // 2
        std::vector<Node> m_nodes;
        // 3
        std::vector<Mesh> m_meshes;
        // 4.Primitive(m_materials,m_accessors)
        // 5.m_materials()
        std::vector<Material> m_materials;
        std::vector<Sampler> m_samplers;
        std::vector<Texture> m_textures;
        // 6.m_accessors()
        std::vector<Accessor> m_accessors;
        std::vector<BufferView> m_bufferViews;
        std::vector<ByteArray> m_buffers; // 模型数据


    private:
        void ProcessBuffers(json& buffers, ByteArray chunk1bin);
        void ProcessBufferViews(json& bufferViews);
        void ProcessAccessors(json& accessors);
        void ProcessMaterials(json& materials);
        void ProcessTextures(json& textures);
        void ProcessSamplers(json& samplers);
        void ProcessImages(json& images);
        void ProcessMeshes(json& meshes, json& accessors);
        void ProcessNodes(json& nodes);
        void ProcessScenes(json& scenes);
        void FindAttribute(Primitive& prim, json& attributes, Primitive::eAttribType type, const std::string& name);
        uint32_t ReadTextureInfo(json& info_json, glTF::Texture*& info);
    };


} // namespace glTF
