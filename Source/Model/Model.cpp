
#define STB_IMAGE_IMPLEMENTATION
#include "../ThirdParty/stb_image/stb_image.h"
#include "Model.h"
#include <iostream>
#include <filesystem>
#include <windows.h>

// 引入 stb_image 进行解码


Model::~Model()
{
    // 释放所有 stb_image 分配的 CPU 内存
    for (auto& tex : LoadedTextures)
    {
        if (tex.Pixels)
        {
            stbi_image_free(tex.Pixels);
            tex.Pixels = nullptr;
        }
    }
}

bool Model::LoadFromFile(const std::string& path)
{
    // 绝对路径打印=================================================
    std::filesystem::path p = path;
    std::string absPath = std::filesystem::absolute(p).string();
    OutputDebugStringA((">>> Target Absolute Path: " + absPath + "\n").c_str());

    if (!std::filesystem::exists(p))
    {
        OutputDebugStringA(">>> [ERROR] File DOES NOT EXIST at the above path!\n");
        return false; // 文件都不存在，直接返回，别让 Assimp 去送死了
    }
    else
    {
        OutputDebugStringA(">>> [SUCCESS] File found! Handing over to Assimp...\n");
    }
    // 绝对路径打印=================================================

    Assimp::Importer importer;

    // DX12 标准转换：三角化、左手坐标系翻转 (包含翻转 UV 和 Z 轴)、生成法线
    const unsigned int importFlags =
        aiProcess_Triangulate |
        aiProcess_ConvertToLeftHanded |
        aiProcess_GenNormals;

    const aiScene* scene = importer.ReadFile(path, importFlags);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        std::string msg = "Assimp Error: " + std::string(importer.GetErrorString()) + "\n";
        OutputDebugStringA(msg.c_str());
        return false;
    }

    // 提取目录路径，用于处理 .gltf 的外部贴图
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash != std::string::npos) {
        Directory = path.substr(0, lastSlash);
    }
    else {
        Directory = ".";
    }

    ProcessNode(scene->mRootNode, scene);

    OutputDebugStringA("Model Loaded Successfully!\n");
    return true;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene)
{
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        Meshes.push_back(ProcessMesh(mesh, scene));
    }

    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        ProcessNode(node->mChildren[i], scene);
    }
}

Submesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
{
    Submesh submesh;

    // 1. 提取顶点
    for (unsigned int i = 0; i < mesh->mNumVertices; i++)
    {
        ModelVertex vertex;
        vertex.Position = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };

        if (mesh->HasNormals()) {
            vertex.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };
        }

        if (mesh->mTextureCoords[0]) {
            vertex.TexCoord = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
        }
        else {
            vertex.TexCoord = { 0.0f, 0.0f };
        }
        submesh.Vertices.push_back(vertex);
    }

    // 2. 提取索引
    for (unsigned int i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            submesh.Indices.push_back(face.mIndices[j]);
        }
    }

    // 3. 提取材质与纹理
    if (mesh->mMaterialIndex >= 0)
    {
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

        // 优先获取 BaseColor (PBR glTF)，如果没有则退化为 Diffuse (OBJ)
        int texIndex = LoadMaterialTexture(material, aiTextureType_BASE_COLOR, scene);
        if (texIndex == -1) {
            texIndex = LoadMaterialTexture(material, aiTextureType_DIFFUSE, scene);
        }
        submesh.MaterialIndex = texIndex;
    }

    // 4. 计算子网格体的顶点，索引总大小
    submesh.vbByteSize = (UINT)submesh.Vertices.size() * sizeof(ModelVertex);
    submesh.ibByteSize = (UINT)submesh.Indices.size() * sizeof(uint16_t);

    return submesh;
}

// 核心逻辑：自动处理外部路径与 GLB 嵌入式内存纹理
int Model::LoadMaterialTexture(aiMaterial* mat, aiTextureType type, const aiScene* scene)
{
    if (mat->GetTextureCount(type) == 0) return -1;

    aiString str;
    mat->GetTexture(type, 0, &str);
    std::string texPath = str.C_Str();

    // 查重：如果这张贴图已经加载过，直接返回索引，避免重复解码消耗内存
    for (int i = 0; i < LoadedTextures.size(); i++) {
        if (LoadedTextures[i].Name == texPath) return i;
    }

    RawTextureData rawData;
    rawData.Name = texPath;
    int desired_channels = 4; // DX12 统一要求 RGBA (DXGI_FORMAT_R8G8B8A8_UNORM)

    // 情况 A：处理 GLB 嵌入式内存纹理 (例如 "*0")
    if (texPath.length() > 0 && texPath[0] == '*')
    {
        int embedIndex = std::stoi(texPath.substr(1));
        if (embedIndex < scene->mNumTextures)
        {
            aiTexture* aiTex = scene->mTextures[embedIndex];
            if (aiTex->mHeight == 0)
            {
                // mHeight 为 0 表示这是压缩图像数据流 (PNG/JPG)
                rawData.Pixels = stbi_load_from_memory(
                    reinterpret_cast<unsigned char*>(aiTex->pcData),
                    aiTex->mWidth, // 此时 mWidth 代表字节数
                    &rawData.Width,
                    &rawData.Height,
                    &rawData.Channels,
                    desired_channels);

                OutputDebugStringA(("Decoded embedded texture: " + texPath + "\n").c_str());
            }
        }
    }
    // 情况 B：处理 glTF/OBJ 外部文件路径
    else
    {
        // 拼接绝对/相对路径
        std::string fullPath = Directory + "/" + texPath;
        rawData.Pixels = stbi_load(fullPath.c_str(), &rawData.Width, &rawData.Height, &rawData.Channels, desired_channels);

        OutputDebugStringA(("Loaded external texture: " + fullPath + "\n").c_str());
    }

    if (!rawData.Pixels) {
        OutputDebugStringA(("Failed to load texture: " + texPath + "\n").c_str());
        return -1;
    }

    // 存入数组，返回索引
    LoadedTextures.push_back(rawData);
    return (int)LoadedTextures.size() - 1;
}

void Model::PrintTextureInfo() const
{
    OutputDebugStringA("\n========== [TEXTURE LOAD REPORT] ==========\n");

    // 1. 打印物理内存中实际加载了多少张唯一的图片
    if (LoadedTextures.empty())
    {
        OutputDebugStringA("[Warning] No textures were loaded for this model.\n");
    }
    else
    {
        std::string summary = "Total Unique Textures Loaded: " + std::to_string(LoadedTextures.size()) + "\n";
        OutputDebugStringA(summary.c_str());

        for (size_t i = 0; i < LoadedTextures.size(); ++i)
        {
            const auto& tex = LoadedTextures[i];

            // 拼接纹理详情
            std::string msg = "  TexIndex [" + std::to_string(i) + "] | Path: " + tex.Name;

            // 检查解码是否成功
            if (tex.Pixels != nullptr) {
                msg += " | Size: " + std::to_string(tex.Width) + "x" + std::to_string(tex.Height)
                    + " | Channels: " + std::to_string(tex.Channels) + "\n";
            }
            else {
                msg += " | [ERROR] Pixels data is NULL!\n";
            }

            OutputDebugStringA(msg.c_str());
        }
    }

    OutputDebugStringA("-------------------------------------------\n");

    // 2. 打印逻辑绑定关系 (哪个模型部位用了哪张图)
    std::string meshSummary = "Total Submeshes: " + std::to_string(Meshes.size()) + "\n";
    OutputDebugStringA(meshSummary.c_str());

    for (size_t i = 0; i < Meshes.size(); ++i)
    {
        int matIdx = Meshes[i].MaterialIndex;
        std::string texName = "None (No Texture)";

        if (matIdx >= 0 && matIdx < LoadedTextures.size()) {
            texName = LoadedTextures[matIdx].Name;
        }

        std::string msg = "  Mesh [" + std::to_string(i) + "] binds to TexIndex [" + std::to_string(matIdx) + "] -> " + texName + "\n";
        OutputDebugStringA(msg.c_str());
    }

    OutputDebugStringA("===========================================\n\n");
}
