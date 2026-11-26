#include "05_Scene/Model/ModelLoader.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "03_AssetSystem/Assets/Importers/glTF.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureConvert.h"
#include "03_AssetSystem/Assets/Importers/Mesh/MeshConvert.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
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
	// 蹇呴』浣跨敤 reinterpret_cast 鏄惧紡鎵撶牬绫诲瀷灞忛殰锛屽悜缂栬瘧鍣ㄥ０鏄庝綘鐨勫簳灞傛剰鍥? C17鐨刡yte鏀逛簡
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
		// 妯″瀷瀵煎叆灞傚彧璁板綍鍑犱綍淇℃伅锛屾潗璐ㄥ眰璁板綍鏉愯川淇℃伅锛屾覆鏌撳眰鍦ㄨ繍琛屾椂灏嗕袱鑰呯粨鍚堟潵閫夋嫨PSO
		mesh->vertexFlags = iter.second[0]->vertexFlags;

		mesh->numDraws = (uint16_t)numDraws;

		uint32_t drawIdx = 0;
		uint32_t curVertOffset = 0;
		uint32_t curIndexOffset = 0;
		// R1.3锛氭繁搴﹂《鐐圭紦鍐插悓鏍烽渶瑕佹寜 draw 閫掑鐨勫瓧鑺傚亸绉汇€?
		// 鍘熷疄鐜颁笁琛?memcpy 閲岋紝VB 鐢?curVertOffset銆両B 鐢?curIndexOffset锛?
		// 鍞嫭 DepthVB 鍥哄畾鍐?curDepthVBOffset 鈥斺€?鍚屼竴 Mesh 鍚涓?draw 鏃讹紝
		// 鍚庝竴涓?draw 浼氳鐩栧墠涓€涓殑娣卞害椤剁偣锛屽鑷存繁搴﹂閫氶亾璇诲埌閿欒鍑犱綍銆?
		// 杩欓噷缁熶竴鎴愩€屽瓧鑺傚亸绉汇€嶈涔夛紙涓?VB/IB 閭ｄ袱涓槸鍏冪礌鍋忕Щ涓嶅悓锛?
		// 鍥犱负娣卞害娴佸彧琚暣鍧?memcpy锛屼笉鍙備笌 baseVertex 璁＄畻锛夈€?
		uint32_t curDepthVertOffset = 0;
		for (auto& draw : iter.second)
		{
			Scene::Model::Mesh::Draw& d = mesh->draw[drawIdx++];
			d.primCount = draw->primCount;
			d.baseVertex = curVertOffset;
			d.startIndex = curIndexOffset;
			// [TD-05b / 瀛楄妭鍋忕Щ vs 鍏冪礌鍋忕Щ涓嶅彉閲廬
			// curVertOffset 涓?curIndexOffset 璇箟鏄€岄《鐐?绱㈠紩鍏冪礌涓暟銆嶏紙渚?DrawIndexed 鐨?
			// BaseVertexLocation / StartIndexLocation 浣跨敤锛夛紝鑰岄潪瀛楄妭鍋忕Щ锛?
			// memcpy 鐩爣鍦板潃蹇呴』涔樹互 vertexStride / indexByteSize 鎹㈢畻涓哄瓧鑺傚亸绉伙紱
			// 鍘熷疄鐜版紡涔樺鑷村悓涓€ Mesh 鍚涓?draw 鏃讹紝鍚庣画 draw 瑕嗙洊鍓嶉潰 draw 鐨勯《鐐逛笌绱㈠紩鏁版嵁銆?
			const size_t vertByteOffset = static_cast<size_t>(curVertOffset) * draw->vertexStride;
			const size_t indexByteSize = draw->index32 ? sizeof(uint32_t) : sizeof(uint16_t);
			const size_t indexByteOffset = static_cast<size_t>(curIndexOffset) * indexByteSize;
			std::memcpy(uploadMem + curVBOffset + vertByteOffset, draw->VB->data(), draw->VB->size());
			curVertOffset += (uint32_t)draw->VB->size() / draw->vertexStride;
			if (draw->DepthVB && !draw->DepthVB->empty())
			{
				std::memcpy(uploadMem + curDepthVBOffset + curDepthVertOffset,
					draw->DepthVB->data(), draw->DepthVB->size());
				curDepthVertOffset += (uint32_t)draw->DepthVB->size();
			}
			std::memcpy(uploadMem + curIBOffset + indexByteOffset, draw->IB->data(), draw->IB->size());
			curIndexOffset += (uint32_t)draw->IB->size() >> (draw->index32 + 1);
		}

		curVBOffset += (uint32_t)vbSize;
		curDepthVBOffset += (uint32_t)vbDepthSize;
		curIBOffset += (uint32_t)Math::AlignUp(ibSize, 4);
		curIndexOffset = Math::AlignUp(curIndexOffset, 4);

		meshList.push_back(mesh);
	}

	//bufferMemory.insert(bufferMemory.end(), stagingBuffer->begin(), stagingBuffer->end());

	//// 1. 璁板綍鍘熷ぇ灏忥紝骞跺己鍒跺鍣ㄦ墿瀹瑰埌鍚堝苟鍚庣殑澶у皬
	size_t oldSize = bufferMemory.size();
	bufferMemory.resize(oldSize + stagingBuffer->size());

	//// 2. 鐩存帴鍦ㄨ櫄鎷熷唴瀛樺湴鍧€涓婅繘琛屾壒閲忓瓧鑺傛嫹璐?
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
	if (!texture || !texture->source)
		return;
	auto it = optionsMap.find(texture->source->path);
	if (it == optionsMap.end())
	{
		optionsMap[texture->source->path] = options;
	}
	else
	{
		// [WP-1D / TD-06f] 鍚屼竴寮犳簮璐村浘琚涓潗璐ㄥ叡浜椂锛堜緥濡備笉閫忔槑涓?MASK 澶嶇敤鍚屼竴寮?BaseColor锛夛紝
		// 淇濈暀鏇撮珮瑕佹眰鐨勯€夐」浣嶏紙濡?kPreserveAlpha锛夛紝閬垮厤鍏堝嚭鐜扮殑鏉愯川闈欓粯鎶规帀 Alpha 閫氶亾闇€姹傘€?
		it->second |= (options & kPreserveAlpha);
	}
}

void BuildMaterials(ModelData& model, const glTF::Asset& asset)
{
	// 鍥犱负 D3D12 鐨?CBV锛堝父閲忕紦鍐茶鍥撅級瑕佹眰 256 瀛楄妭瀵归綈
	static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

	// 灏?glTF 閲岀殑鎵€鏈夊浘鐗囪矾寰勫鍒跺埌 model 鐨勭汗鐞嗗瓧绗︿覆琛ㄤ腑
	// 鍚庨潰淇濆瓨妯″瀷鏃朵細鎶婅繖浜涜矾寰勪綔涓哄瓧绗︿覆琛ㄥ啓鍏ユ枃浠?
	// 娉ㄩ噴閲屽啓鈥淩eplace texture filename extensions with DDS鈥濓紝浣嗚繖娈典唬鐮佹湰韬彧鏄嫹璐濊矾寰?
	model.m_TextureNames.resize(asset.m_images.size());

	for (size_t i = 0; i < asset.m_images.size(); ++i)
		model.m_TextureNames[i] = asset.m_images[i].path;

	// 璁板綍姣忓紶绾圭悊搴旈噰鐢ㄧ殑缂栬瘧/鎵撳寘閫夐」
	// key: 绾圭悊鏂囦欢鍚?
	// value: 绾圭悊閫夐」鏍囪
	std::map<std::string, uint8_t> textureOptions;

	// 鏉愯川鏁伴噺
	const uint32_t numMaterials = (uint32_t)asset.m_materials.size();

	// 涓烘墍鏈夋潗璐ㄥ垎閰嶅父閲忔暟鎹笌绾圭悊鎻忚堪鏁版嵁
	model.m_MaterialConstants.resize(numMaterials);
	model.m_MaterialTextures.resize(numMaterials);

	// 閫愪釜澶勭悊 glTF 鏉愯川
	for (uint32_t i = 0; i < numMaterials; ++i)
	{
		// 鍙栧嚭褰撳墠婧愭潗璐?
		const glTF::Material& srcMat = asset.m_materials[i];

		// 鑾峰彇寮曟搸鑷繁鐨勬潗璐ㄥ父閲忕粨鏋勭殑鎴愬憳
		MaterialConstantData& material = model.m_MaterialConstants[i];

		// BaseColorFactor锛氬熀纭€棰滆壊鍥犲瓙
		material.baseColorFactor[0] = srcMat.baseColorFactor[0];
		material.baseColorFactor[1] = srcMat.baseColorFactor[1];
		material.baseColorFactor[2] = srcMat.baseColorFactor[2];
		material.baseColorFactor[3] = srcMat.baseColorFactor[3];
		// EmissiveFactor锛氳嚜鍙戝厜棰滆壊鍥犲瓙
		if (srcMat.emissiveFactor)
			material.emissiveFactor[0] = srcMat.emissiveFactor[0];
		material.emissiveFactor[1] = srcMat.emissiveFactor[1];
		material.emissiveFactor[2] = srcMat.emissiveFactor[2];
		// 娉曠嚎璐村浘寮哄害
		material.normalTextureScale = srcMat.normalTextureScale;
		// 閲戝睘搴?
		material.metallicFactor = srcMat.metallicFactor;
		// 绮楃硻搴?
		material.roughnessFactor = srcMat.roughnessFactor;

		// 鏉愯川鏍囧織浣嶏紝渚嬪 alpha blend / alpha test 绛?
		material.flags = srcMat.flags;

		// 鑾峰彇寮曟搸鑷繁鐨勬潗璐ㄧ汗鐞嗘弿杩扮粨鏋勭殑鎴愬憳
		MaterialTextureData& dstMat = model.m_MaterialTextures[i];

		// 鎵€鏈夌汗鐞嗙殑鍦板潃妯″紡鎵撳寘鍒颁竴涓瓧娈甸噷
		// 姣忓紶绾圭悊鍗?4 bit锛歋 鏂瑰悜 2 bit + T 鏂瑰悜 2 bit
		dstMat.addressModes = 0;

		// 閬嶅巻鏉愯川浣跨敤鐨勬瘡涓€绉嶇汗鐞嗘Ы浣?
		for (uint32_t ti = 0; ti < kNumTextures; ++ti)
		{
			// 榛樿璁句负鏃犳晥绱㈠紩 0xFFFF锛岃〃绀鸿妲芥病鏈夋湁鏁堢汗鐞?
			dstMat.stringIdx[ti] = 0xFFFF;

			if (srcMat.textures[ti] != nullptr)
			{
				// 濡傛灉杩欎釜绾圭悊妲芥湁璐村浘婧愶紝鍒欒褰曞畠鍦?asset.m_images 閲岀殑涓嬫爣
				if (srcMat.textures[ti]->source != nullptr)
				{
					dstMat.stringIdx[ti] = uint16_t(srcMat.textures[ti]->source - asset.m_images.data());
				}

				// 濡傛灉杩欎釜绾圭悊鏈?sampler锛屽垯浣跨敤 sampler 鐨?wrapS / wrapT
				if (srcMat.textures[ti]->sampler != nullptr)
				{
					// wrapS 鍐欏叆 ti 瀵瑰簲鐨勪綆 2 bit
					dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapS << (ti * 4);

					// wrapT 鍐欏叆 ti 瀵瑰簲鐨勯珮 2 bit
					dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapT << (ti * 4 + 2);
				}
				else
				{
					// 娌℃湁 sampler 鏃讹紝浣跨敤榛樿鍦板潃妯″紡 0x5
					// 杩欓噷鐨?0x5 閫氬父琛ㄧず涓€绉嶉粯璁ょ殑 Repeat/Wrap 缁勫悎
					dstMat.addressModes |= 0x5 << (ti * 4);
				}
			}
			else
			{
				// 娌℃湁绾圭悊鏃讹紝涔熷～榛樿鍦板潃妯″紡锛岄伩鍏嶅悗缁鍙栨湭鍒濆鍖栧€?
				dstMat.addressModes |= 0x5 << (ti * 4);
			}
		}

		// ============================================================
		// 鏀堕泦闇€瑕佹寜闇€缂栬瘧鐨勭汗鐞嗛€夐」锛圧1.1锛?
		// ------------------------------------------------------------
		// 缁熶竴妲戒綅椤哄簭鍚庯紝杩欓噷鏄惧紡鐢?glTF::Material:: 闄愬畾鍚嶏細
		// 鍗充娇灏嗘潵 ConstantBuffers.h 鐨勯『搴忓啀鍙橈紝鏈涔熶笉浼氳闈欓粯甯﹀亸銆?
		//
		// 鑹插僵绌洪棿瑙勫垯锛坓lTF 2.0 瑙勮寖锛夛細
		//   棰滆壊璐村浘锛圔aseColor / Emissive锛夆啋 sRGB 缂栫爜锛岄噰鏍锋椂纭欢瑙ｇ爜鍒扮嚎鎬?
		//   鏁版嵁璐村浘锛圢ormal / Occlusion / MetallicRoughness锛夆啋 蹇呴』淇濇寔绾挎€э紝
		//   鑻ヨ褰撲綔 sRGB 瑙ｇ爜锛屾硶绾夸笌绮楃硻搴︿細鏁翠綋鍋忕Щ
		//
		// R1 淇锛欱aseColor 鍘熷厛缂栬瘧涓?Linear锛圱extureOptions(false)锛夛紝
		// 鑰岀鐩樹笂鐨?PNG 鏄?sRGB 缂栫爜鐨勶紝绛変簬鎶?sRGB 鍊肩洿鎺ュ綋绾挎€у€肩敤锛岀敾闈㈠亸鏆椼€?
		// ============================================================
		const bool bAlpha = (srcMat.alphaBlend || srcMat.alphaTest) != 0;
		SetTextureOptions(textureOptions, srcMat.textures[glTF::Material::kBaseColor],
			TextureOptions(true, bAlpha));    // 棰滆壊璐村浘 鈫?sRGB锛涙湁 alpha 鏃朵繚鐣?
		SetTextureOptions(textureOptions, srcMat.textures[glTF::Material::kMetallicRoughness],
			TextureOptions(false));           // 鏁版嵁璐村浘 鈫?绾挎€?
		SetTextureOptions(textureOptions, srcMat.textures[glTF::Material::kOcclusion],
			TextureOptions(false));           // 鏁版嵁璐村浘 鈫?绾挎€?
		SetTextureOptions(textureOptions, srcMat.textures[glTF::Material::kEmissive],
			TextureOptions(true));            // 棰滆壊璐村浘 鈫?sRGB
		SetTextureOptions(textureOptions, srcMat.textures[glTF::Material::kNormal],
			TextureOptions(false));           // 鏁版嵁璐村浘 鈫?绾挎€?
	}

	// 娓呯┖妯″瀷鍘熸湁鐨勭汗鐞嗛€夐」琛?
	model.m_TextureOptions.clear();

	// 鎸夌汗鐞嗗悕绉拌〃閫愪竴濉厖绾圭悊閫夐」
	for (auto name : model.m_TextureNames)
	{
		// 鍦ㄧ汗鐞嗛€夐」琛ㄤ腑鏌ユ壘璇ョ汗鐞嗗悕鏄惁鏈夌壒娈婇厤缃?
		auto iter = textureOptions.find(name);

		if (iter != textureOptions.end())
		{
			// 鎵惧埌浜嗗氨鎶婂搴旈€夐」鍐欏叆妯″瀷
			model.m_TextureOptions.push_back(iter->second);

			// 杩欓噷浼氱珛鍒昏Е鍙戣绾圭悊鐨勬寜闇€缂栬瘧
			// 璺緞 = 鍩虹璺緞 + 绾圭悊鏂囦欢鍚?
			CompileTextureOnDemand(asset.m_basePath + Utility::UTF8ToWideString(iter->first), iter->second);
		}
		else
		{
			// 娌℃壘鍒板垯鍐欓粯璁ゅ€?0xFF锛岃〃绀烘病鏈夌壒娈婇€夐」
			model.m_TextureOptions.push_back(0xFF);
		}
	}

	// 鏈€缁堟柇瑷€锛氱汗鐞嗛€夐」琛ㄦ暟閲忓繀椤诲拰绾圭悊瀛楃涓茶〃鏁伴噺涓€鑷?
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
	// [WP-1D / TD-08c] 鍘熷瓙鍐欑洏鍗忚锛氬厛鍐欏叆鍚岀洰褰曚复鏃舵枃浠?<filePath>.tmp锛屽畬鏁村埛鐩樺苟鏍￠獙娴佺姸鎬佸悗锛?
	// 鍐嶇敤 MoveFileExW 鍘熷瓙鏇挎崲鐩爣 .ty 缂撳瓨锛涗换浣曚腑閫斿け璐?鏂數閮戒笉浼氬湪纾佺洏鐣欎笅鍗婃埅鎹熷潖鐨?.ty 鏂囦欢銆?
	const std::wstring tmpPath = filePath + L".tmp";
	std::ofstream outFile(tmpPath, std::ios::out | std::ios::binary | std::ios::trunc);
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

	outFile.flush();
	const bool writeOk = outFile.good();
	outFile.close();
	if (!writeOk)
	{
		::DeleteFileW(tmpPath.c_str());
		return false;
	}

	if (!::MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		::DeleteFileW(tmpPath.c_str());
		return false;
	}

	return true;
}
