#pragma once

#include "Model.h"
#include "ConstantBuffers.h"

#include <cstdint>
#include <vector>

namespace glTF { class Asset; struct Mesh; }

#define CURRENT_MINI_FILE_VERSION 13

namespace Renderer
{
    using namespace Math;

    // Unaligned mirror of MaterialConstants
    struct MaterialConstantData
    {
        float baseColorFactor[4]; // default=[1,1,1,1]
        float emissiveFactor[3]; // default=[0,0,0]
        float normalTextureScale; // default=1
        float metallicFactor; // default=1
        float roughnessFactor; // default=1
        uint32_t flags;
    };

    // Used at load time to construct descriptor tables
    struct MaterialTextureData
    {
        uint16_t stringIdx[kNumTextures];
        uint32_t addressModes;
    };

    // All of the information that needs to be written to a .ty data file
    // 
    struct ModelData
    {
        std::vector<::byte> m_GeometryData; // 几何数据
        std::vector<MaterialTextureData> m_MaterialTextures; // 材质索引
        std::vector<MaterialConstantData> m_MaterialConstants; // 材质常量
        std::vector<Mesh*> m_Meshes;
        std::vector<GraphNode> m_SceneGraph;
        std::vector<std::string> m_TextureNames;
        std::vector<uint8_t> m_TextureOptions;
    };

    struct FileHeader
    {
        char     id[2];   // "TY"
        uint32_t version; // CURRENT_TY_FILE_VERSION
        uint32_t numNodes;
        uint32_t numMeshes;
        uint32_t numMaterials;
        uint32_t meshDataSize;
        uint32_t numTextures;
        uint32_t stringTableSize;
        uint32_t geometrySize;
        float    minPos[3];
        float    maxPos[3];
    };

    void CompileMesh(
        std::vector<Mesh*>& meshList,
        std::vector < ::byte >& bufferMemory,
        glTF::Mesh& srcMesh,
        uint32_t matrixIdx,
        const Matrix4& localToObject
    );

    bool BuildModel(ModelData& model, const glTF::Asset& asset, int sceneIdx = -1);
    bool SaveModel(const std::wstring& filePath, const ModelData& model);

    std::shared_ptr<Model> LoadModel(const std::wstring& filePath, bool forceRebuild = false);
}
