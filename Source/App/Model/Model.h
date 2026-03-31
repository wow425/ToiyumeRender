#pragma once
#include <vector>
#include <string>
#include <DirectXMath.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

// 顶点结构体 (适配 DX12 Input Layout)
struct ModelVertex
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT2 TexCoord;
    DirectX::XMFLOAT3 TangentU;
};

// 纹理数据结构体 (纯 CPU 数据，脱离 DirectX API)
struct RawTextureData
{
    std::string Name;       // 纹理名称或相对路径（用于调试）
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    unsigned char* Pixels = nullptr; // stb_image 解码后的 RGBA 像素
};

// 子网格体
struct Submesh
{
    std::vector<ModelVertex> Vertices;
    std::vector<uint16_t> Indices;
    unsigned int vbByteSize;
    unsigned int ibByteSize;
    int MaterialIndex = -1; // 指向 Model 内部维护的材质/纹理数组
};

class Model
{
public:
    Model() = default;
    ~Model(); // 负责清理 stb_image 分配的内存

    // 核心加载接口
    bool LoadFromFile(const std::string& path);

    // 获取解析后的数据供 DX12 使用
    const std::vector<Submesh>& GetMeshes() const { return Meshes; }
    const std::vector<RawTextureData>& GetLoadedTextures() const { return LoadedTextures; }

    // 新增：打印并检验纹理加载状态的方法
    void PrintTextureInfo() const;

private:
    void ProcessNode(aiNode* node, const aiScene* scene);
    Submesh ProcessMesh(aiMesh* mesh, const aiScene* scene);
    int LoadMaterialTexture(aiMaterial* mat, aiTextureType type, const aiScene* scene);

private:
    std::vector<Submesh> Meshes;
    std::vector<RawTextureData> LoadedTextures; // 统一存储解码后的贴图
    std::string Directory;                      // 模型所在文件夹
};
