#pragma once

#include "04_Renderer/Material/Material.h"
#include "05_Scene/Model/Model.h"
#include "03_AssetSystem/Assets/Constants/ConstantBuffers.h"

#include <cstdint>
#include <vector>

namespace glTF { class Asset; struct Mesh; }

// [TD-05b] 版本 2 -> 3：修复 CompileMesh 多 draw 合并时 VB/IB 字节偏移漏乘 stride/indexSize 的
// 数据覆写 Bug，递增版本号强制旧 .ty 缓存自动重新烘焙。
// [GFL2 / M1] 版本 3 -> 4：MeshConvert 支持 COLOR_0 (kVertex_Color0, R32G32B32A32_FLOAT)，
// 递增版本号以触发 .ty 缓存重烘并支持 -RebuildModels 验收。
#define CURRENT_MINI_FILE_VERSION 4

namespace Scene::Model
{
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
		Math::BoundingSphere m_BoundingSphere;
		Math::AxisAlignedBox m_BoundingBox;
		std::vector<::byte> m_GeometryData; // 几何数据
		std::vector<MaterialTextureData> m_MaterialTextures; // 材质纹理描述
		std::vector<Scene::Material::MaterialConstantData> m_MaterialConstants; // 材质常量数据
		std::vector<Mesh*> m_Meshes;
		std::vector<Scene::Model::GraphNode> m_SceneGraph;
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
		float    boundingSphere[4];
		float    minPos[3];
		float    maxPos[3];
	};

	void CompileMesh(
		std::vector<Scene::Model::Mesh*>& meshList,
		std::vector < ::byte >& bufferMemory,
		glTF::Mesh& srcMesh,
		uint32_t matrixIdx,
		const Math::Matrix4& localToObject,
		Math::BoundingSphere& boundingSphere,
		Math::AxisAlignedBox& boundingBox
	);

	bool BuildModel(ModelData& model, const glTF::Asset& asset, int sceneIdx = -1);
	bool SaveModel(const std::wstring& filePath, const ModelData& model);

	std::shared_ptr<Scene::Model::Model> LoadModel(const std::wstring& filePath, bool forceRebuild = false);
}
