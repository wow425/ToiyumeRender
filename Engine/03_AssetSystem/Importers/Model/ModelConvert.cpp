#include "05_Scene/Model/ModelLoader.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "03_AssetSystem/Importers/glTF.h"
#include "03_AssetSystem/Importers/Texture/TextureConvert.h"
#include "03_AssetSystem/Importers/Mesh/MeshConvert.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "00_Core/Utility/Utility.h"
#include "00_Core/Math/Common.h"

#include <fstream>
#include <map>
#include <unordered_map>

using namespace DirectX;
using namespace Math;
using namespace Renderer;
using namespace Graphics;
using namespace Scene::Model;
using namespace Scene::Material;

static inline Vector3 SafeNormalize(Vector3 x)
{
	float lenSq = LengthSquare(x);
	return lenSq < 1e-10f ? Vector3(kXUnitVector) : x * RecipSqrt(lenSq);
}

void Scene::Model::CompileMesh(
	std::vector<Scene::Model::Mesh*>& meshList,
	std::vector<byte>& bufferMemory,
	glTF::Mesh& srcMesh,
	uint32_t matrixIdx,
	const Matrix4& localToObject,
	BoundingSphere& boundingSphere,
	AxisAlignedBox& boundingBox
)
{
	// We still have a lot of work to do.  Now that we know about all of the primitives in this mesh
	// and have standardized their vertex buffer streams, we must set out to identify which primitives
	// have the same vertex format and material.  These can share a PSO and Vertex/Index buffer views.
	// There may be more than one draw call per group due to 16-bit indices.

	size_t totalVertexSize = 0;
	size_t totalDepthVertexSize = 0;
	size_t totalIndexSize = 0;

	BoundingSphere sphereOS(kZero);
	AxisAlignedBox bboxOS(kZero);

	std::vector<Primitive> primitives(srcMesh.primitives.size());
	for (uint32_t i = 0; i < primitives.size(); ++i)
	{
		OptimizeMesh(primitives[i], srcMesh.primitives[i], localToObject);
		sphereOS = sphereOS.Union(primitives[i].m_BoundsOS);
		bboxOS.AddBoundingBox(primitives[i].m_BBoxOS);
	}

	boundingSphere = sphereOS;
	boundingBox = bboxOS;


	std::map<uint32_t, std::vector<Primitive*>> renderMeshes;
	for (auto& prim : primitives)
	{
		uint32_t hash = prim.hash;
		renderMeshes[hash].push_back(&prim);
		totalVertexSize += prim.VB->size();
		totalDepthVertexSize += prim.DepthVB->size();
		totalIndexSize += Math::AlignUp(prim.IB->size(), 4);
	}

	uint32_t totalBufferSize = (uint32_t)(totalVertexSize + totalDepthVertexSize + totalIndexSize);

	Utility::ByteArray stagingBuffer;
	stagingBuffer = std::make_shared<std::vector<std::byte>>(totalBufferSize);
	// 必须使用 reinterpret_cast 显式打破类型屏障，向编译器声明你的底层意图. C17的byte改了
	uint8_t* uploadMem = reinterpret_cast<uint8_t*>(stagingBuffer->data());

	uint32_t curVBOffset = 0;
	uint32_t curDepthVBOffset = (uint32_t)totalVertexSize;
	uint32_t curIBOffset = curDepthVBOffset + (uint32_t)totalDepthVertexSize;

	for (auto& iter : renderMeshes)
	{
		size_t numDraws = iter.second.size();
		Mesh* mesh = (Scene::Model::Mesh*)malloc(sizeof(Scene::Model::Mesh) + sizeof(Scene::Model::Mesh::Draw) * (numDraws - 1));
		size_t vbSize = 0;
		size_t vbDepthSize = 0;
		size_t ibSize = 0;



		for (auto& draw : iter.second)
		{
			vbSize += draw->VB->size();
			vbDepthSize += draw->DepthVB->size();
			ibSize += draw->IB->size();

		}


		mesh->vbOffset = (uint32_t)bufferMemory.size() + curVBOffset;
		mesh->vbSize = (uint32_t)vbSize;
		mesh->vbDepthOffset = (uint32_t)bufferMemory.size() + curDepthVBOffset;
		mesh->vbDepthSize = (uint32_t)vbDepthSize;
		mesh->ibOffset = (uint32_t)bufferMemory.size() + curIBOffset;
		mesh->ibSize = (uint32_t)ibSize;
		mesh->vbStride = (uint8_t)iter.second[0]->vertexStride;
		mesh->vbDepthStride = (uint8_t)iter.second[0]->depthVertexStride;
		mesh->ibFormat = uint8_t(iter.second[0]->index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);
		mesh->meshCBV = (uint16_t)matrixIdx;
		mesh->materialSlotIdx = iter.second[0]->materialIdx;
		//mesh->psoFlags = iter.second[0]->psoFlags;
		//mesh->pso = 0xFFFF;
		// Asset import records geometry features only.  The renderer combines
		// these vertex flags with the material state at runtime to choose a PSO.
		// 模型导入层只记录几何信息，材质层记录材质信息，渲染层在运行时将两者结合来选择PSO
		mesh->vertexFlags = iter.second[0]->vertexFlags;

		mesh->numDraws = (uint16_t)numDraws;

		uint32_t drawIdx = 0;
		uint32_t curVertOffset = 0;
		uint32_t curIndexOffset = 0;
		for (auto& draw : iter.second)
		{
			Scene::Model::Mesh::Draw& d = mesh->draw[drawIdx++];
			d.primCount = draw->primCount;
			d.baseVertex = curVertOffset;
			d.startIndex = curIndexOffset;
			std::memcpy(uploadMem + curVBOffset + curVertOffset, draw->VB->data(), draw->VB->size());
			curVertOffset += (uint32_t)draw->VB->size() / draw->vertexStride;
			std::memcpy(uploadMem + curDepthVBOffset, draw->DepthVB->data(), draw->DepthVB->size());
			std::memcpy(uploadMem + curIBOffset + curIndexOffset, draw->IB->data(), draw->IB->size());
			curIndexOffset += (uint32_t)draw->IB->size() >> (draw->index32 + 1);
		}

		curVBOffset += (uint32_t)vbSize;
		curDepthVBOffset += (uint32_t)vbDepthSize;
		curIBOffset += (uint32_t)Math::AlignUp(ibSize, 4);
		curIndexOffset = Math::AlignUp(curIndexOffset, 4);

		meshList.push_back(mesh);
	}

	//bufferMemory.insert(bufferMemory.end(), stagingBuffer->begin(), stagingBuffer->end());

	//// 1. 记录原大小，并强制容器扩容到合并后的大小
	size_t oldSize = bufferMemory.size();
	bufferMemory.resize(oldSize + stagingBuffer->size());

	//// 2. 直接在虚拟内存地址上进行批量字节拷贝
	std::memcpy(
		bufferMemory.data() + oldSize,
		stagingBuffer->data(),
		stagingBuffer->size()
	);
}


static uint32_t WalkGraph(
	std::vector<Scene::Model::GraphNode>& sceneGraph,
	BoundingSphere& modelBSphere,
	AxisAlignedBox& modelBBox,
	std::vector<Scene::Model::Mesh*>& meshList,
	std::vector<byte>& bufferMemory,
	const std::vector<glTF::Node*>& siblings,
	uint32_t curPos,
	const Matrix4& xform
)
{
	size_t numSiblings = siblings.size();

	for (size_t i = 0; i < numSiblings; ++i)
	{
		glTF::Node* curNode = siblings[i];
		GraphNode& thisGraphNode = sceneGraph[curPos];
		thisGraphNode.hasChildren = 0;
		thisGraphNode.hasSibling = 0;
		thisGraphNode.matrixIdx = curPos;
		curNode->linearIdx = curPos;

		// They might not be used, but we have space to hold the neutral values which could be
		// useful when updating the matrix via animation.
		std::memcpy((float*)&thisGraphNode.scale, curNode->scale, sizeof(curNode->scale));
		std::memcpy((float*)&thisGraphNode.rotation, curNode->rotation, sizeof(curNode->rotation));

		if (curNode->hasMatrix)
		{
			std::memcpy((float*)&thisGraphNode.xform, curNode->matrix, sizeof(curNode->matrix));
		}
		else
		{
			thisGraphNode.xform = Matrix4(
				Matrix3(thisGraphNode.rotation) * Matrix3::MakeScale(thisGraphNode.scale),
				Vector3(*(const XMFLOAT3*)curNode->translation)
			);
		}

		const Matrix4 LocalXform = xform * thisGraphNode.xform;

		if (!curNode->pointsToCamera && curNode->mesh != nullptr)
		{
			BoundingSphere sphereOS;
			AxisAlignedBox boxOS;
			CompileMesh(meshList, bufferMemory, *curNode->mesh, curPos, LocalXform, sphereOS, boxOS);
			modelBSphere = modelBSphere.Union(sphereOS);
			modelBBox.AddBoundingBox(boxOS);
		}

		uint32_t nextPos = curPos + 1;

		if (curNode->children.size() > 0)
		{
			thisGraphNode.hasChildren = 1;
			nextPos = WalkGraph(sceneGraph, modelBSphere, modelBBox, meshList, bufferMemory, curNode->children, nextPos, LocalXform);
		}

		// Are there more siblings?
		if (i + 1 < numSiblings)
		{
			thisGraphNode.hasSibling = 1;
		}

		curPos = nextPos;
	}

	return curPos;
}

inline void CompileTexture(const std::wstring& basePath, const std::string& fileName, uint8_t flags)
{
	CompileTextureOnDemand(basePath + Utility::UTF8ToWideString(fileName), flags);
}

inline void SetTextureOptions(std::map<std::string, uint8_t>& optionsMap, glTF::Texture* texture, uint8_t options)
{
	if (texture && texture->source && optionsMap.find(texture->source->path) == optionsMap.end())
		optionsMap[texture->source->path] = options;
}

void BuildMaterials(ModelData& model, const glTF::Asset& asset)
{
	// 因为 D3D12 的 CBV（常量缓冲视图）要求 256 字节对齐
	static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

	// 将 glTF 里的所有图片路径复制到 model 的纹理字符串表中
	// 后面保存模型时会把这些路径作为字符串表写入文件
	// 注释里写“Replace texture filename extensions with DDS”，但这段代码本身只是拷贝路径
	model.m_TextureNames.resize(asset.m_images.size());

	for (size_t i = 0; i < asset.m_images.size(); ++i)
		model.m_TextureNames[i] = asset.m_images[i].path;

	// 记录每张纹理应采用的编译/打包选项
	// key: 纹理文件名
	// value: 纹理选项标记
	std::map<std::string, uint8_t> textureOptions;

	// 材质数量
	const uint32_t numMaterials = (uint32_t)asset.m_materials.size();

	// 为所有材质分配常量数据与纹理描述数据
	model.m_MaterialConstants.resize(numMaterials);
	model.m_MaterialTextures.resize(numMaterials);

	// 逐个处理 glTF 材质
	for (uint32_t i = 0; i < numMaterials; ++i)
	{
		// 取出当前源材质
		const glTF::Material& srcMat = asset.m_materials[i];

		// 获取引擎自己的材质常量结构的成员
		MaterialConstantData& material = model.m_MaterialConstants[i];

		// BaseColorFactor：基础颜色因子
		material.baseColorFactor[0] = srcMat.baseColorFactor[0];
		material.baseColorFactor[1] = srcMat.baseColorFactor[1];
		material.baseColorFactor[2] = srcMat.baseColorFactor[2];
		material.baseColorFactor[3] = srcMat.baseColorFactor[3];
		// EmissiveFactor：自发光颜色因子
		if (srcMat.emissiveFactor)
			material.emissiveFactor[0] = srcMat.emissiveFactor[0];
		material.emissiveFactor[1] = srcMat.emissiveFactor[1];
		material.emissiveFactor[2] = srcMat.emissiveFactor[2];
		// 法线贴图强度
		material.normalTextureScale = srcMat.normalTextureScale;
		// 金属度
		material.metallicFactor = srcMat.metallicFactor;
		// 粗糙度
		material.roughnessFactor = srcMat.roughnessFactor;

		// 材质标志位，例如 alpha blend / alpha test 等
		material.flags = srcMat.flags;

		// 获取引擎自己的材质纹理描述结构的成员
		MaterialTextureData& dstMat = model.m_MaterialTextures[i];

		// 所有纹理的地址模式打包到一个字段里
		// 每张纹理占 4 bit：S 方向 2 bit + T 方向 2 bit
		dstMat.addressModes = 0;

		// 遍历材质使用的每一种纹理槽位
		for (uint32_t ti = 0; ti < kNumTextures; ++ti)
		{
			// 默认设为无效索引 0xFFFF，表示该槽没有有效纹理
			dstMat.stringIdx[ti] = 0xFFFF;

			if (srcMat.textures[ti] != nullptr)
			{
				// 如果这个纹理槽有贴图源，则记录它在 asset.m_images 里的下标
				if (srcMat.textures[ti]->source != nullptr)
				{
					dstMat.stringIdx[ti] = uint16_t(srcMat.textures[ti]->source - asset.m_images.data());
				}

				// 如果这个纹理有 sampler，则使用 sampler 的 wrapS / wrapT
				if (srcMat.textures[ti]->sampler != nullptr)
				{
					// wrapS 写入 ti 对应的低 2 bit
					dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapS << (ti * 4);

					// wrapT 写入 ti 对应的高 2 bit
					dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapT << (ti * 4 + 2);
				}
				else
				{
					// 没有 sampler 时，使用默认地址模式 0x5
					// 这里的 0x5 通常表示一种默认的 Repeat/Wrap 组合
					dstMat.addressModes |= 0x5 << (ti * 4);
				}
			}
			else
			{
				// 没有纹理时，也填默认地址模式，避免后续读取未初始化值
				dstMat.addressModes |= 0x5 << (ti * 4);
			}
		}

		// 收集需要按需编译的纹理选项
		// BaseColor 贴图：false（不使用SRGB），PBR工作流在线性空间进行计算，只有最后输出才进行SRGB转换
		// 如果启用了 alpha blend / alpha test，则保留alpha通道
		SetTextureOptions(textureOptions, srcMat.textures[kBaseColor], TextureOptions(false, srcMat.alphaBlend | srcMat.alphaTest));  // false = Linear
		// 金属度/粗糙度贴图：false 表示通常不需要特殊 alpha 处理
		SetTextureOptions(textureOptions, srcMat.textures[kMetallicRoughness], TextureOptions(false));
		// 遮蔽贴图：false
		SetTextureOptions(textureOptions, srcMat.textures[kOcclusion], TextureOptions(false));
		// 自发光贴图：true（使用SRGB），在线性空间处理
		SetTextureOptions(textureOptions, srcMat.textures[kEmissive], TextureOptions(true));
		// 法线贴图：false
		SetTextureOptions(textureOptions, srcMat.textures[kNormal], TextureOptions(false));
	}

	// 清空模型原有的纹理选项表
	model.m_TextureOptions.clear();

	// 按纹理名称表逐一填充纹理选项
	for (auto name : model.m_TextureNames)
	{
		// 在纹理选项表中查找该纹理名是否有特殊配置
		auto iter = textureOptions.find(name);

		if (iter != textureOptions.end())
		{
			// 找到了就把对应选项写入模型
			model.m_TextureOptions.push_back(iter->second);

			// 这里会立刻触发该纹理的按需编译
			// 路径 = 基础路径 + 纹理文件名
			CompileTextureOnDemand(asset.m_basePath + Utility::UTF8ToWideString(iter->first), iter->second);
		}
		else
		{
			// 没找到则写默认值 0xFF，表示没有特殊选项
			model.m_TextureOptions.push_back(0xFF);
		}
	}

	// 最终断言：纹理选项表数量必须和纹理字符串表数量一致
	ASSERT(model.m_TextureOptions.size() == model.m_TextureNames.size());
}





bool Scene::Model::BuildModel(ModelData& model, const glTF::Asset& asset, int sceneIdx)
{
	BuildMaterials(model, asset);

	// Generate scene graph and meshes
	model.m_SceneGraph.resize(asset.m_nodes.size());
	const glTF::Scene* scene = sceneIdx < 0 ? asset.m_scene : &asset.m_scenes[sceneIdx];
	if (scene == nullptr)
		return false;

	// Aggregate all of the vertex and index buffers in this unified buffer
	std::vector<byte>& bufferMemory = model.m_GeometryData;

	model.m_BoundingSphere = BoundingSphere(kZero);
	model.m_BoundingBox = AxisAlignedBox(kZero);
	uint32_t numNodes = WalkGraph(model.m_SceneGraph, model.m_BoundingSphere, model.m_BoundingBox, model.m_Meshes, bufferMemory, scene->nodes, 0, Matrix4(kIdentity));
	model.m_SceneGraph.resize(numNodes);

	return true;
}

bool Scene::Model::SaveModel(const std::wstring& filePath, const ModelData& data)
{
	std::ofstream outFile(filePath, std::ios::out | std::ios::binary);
	if (!outFile)
		return false;

	FileHeader header;
	std::memcpy(header.id, "TY", 2);
	header.version = CURRENT_MINI_FILE_VERSION;
	header.numNodes = (uint32_t)data.m_SceneGraph.size();
	header.numMeshes = (uint32_t)data.m_Meshes.size();
	header.numMaterials = (uint32_t)data.m_MaterialConstants.size();
	header.meshDataSize = 0;
	for (const Mesh* mesh : data.m_Meshes)
		header.meshDataSize += (uint32_t)sizeof(Mesh) + (mesh->numDraws - 1) * (uint32_t)sizeof(Mesh::Draw);
	header.numTextures = (uint32_t)data.m_TextureNames.size();
	header.stringTableSize = 0;
	for (const std::string& str : data.m_TextureNames)
		header.stringTableSize += (uint32_t)str.size() + 1;
	header.geometrySize = (uint32_t)data.m_GeometryData.size();

	header.boundingSphere[0] = data.m_BoundingSphere.GetCenter().GetX();
	header.boundingSphere[1] = data.m_BoundingSphere.GetCenter().GetY();
	header.boundingSphere[2] = data.m_BoundingSphere.GetCenter().GetZ();
	header.boundingSphere[3] = data.m_BoundingSphere.GetRadius();
	header.minPos[0] = data.m_BoundingBox.GetMin().GetX();
	header.minPos[1] = data.m_BoundingBox.GetMin().GetY();
	header.minPos[2] = data.m_BoundingBox.GetMin().GetZ();
	header.maxPos[0] = data.m_BoundingBox.GetMax().GetX();
	header.maxPos[1] = data.m_BoundingBox.GetMax().GetY();
	header.maxPos[2] = data.m_BoundingBox.GetMax().GetZ();

	outFile.write((char*)&header, sizeof(FileHeader));
	outFile.write((char*)data.m_GeometryData.data(), header.geometrySize);
	outFile.write((char*)data.m_SceneGraph.data(), header.numNodes * sizeof(Scene::Model::GraphNode));
	for (const Scene::Model::Mesh* mesh : data.m_Meshes)
		outFile.write((char*)mesh, sizeof(Scene::Model::Mesh) + (mesh->numDraws - 1) * sizeof(Scene::Model::Mesh::Draw));
	outFile.write((char*)data.m_MaterialConstants.data(), header.numMaterials * sizeof(MaterialConstantData));
	outFile.write((char*)data.m_MaterialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));
	for (uint32_t i = 0; i < header.numTextures; ++i)
		outFile << data.m_TextureNames[i] << '\0';
	outFile.write((char*)data.m_TextureOptions.data(), header.numTextures * sizeof(uint8_t));



	return true;
}
