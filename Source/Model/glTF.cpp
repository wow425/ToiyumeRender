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
// 啃到此处 ,明日继续 
void glTF::Asset::ProcessMeshes(json& meshes, json& accessors)
{
    m_meshes.resize(meshes.size());

    uint32_t curMesh = 0;
    for (json::iterator meshIt = meshes.begin(); meshIt != meshes.end(); ++meshIt, ++curMesh)
    {
        json& thisMesh = meshIt.value();
        json& primitives = thisMesh.at("primitives");

        m_meshes[curMesh].primitives.resize(primitives.size());

        uint32_t curSubMesh = 0;
        for (json::iterator primIt = primitives.begin(); primIt != primitives.end(); ++primIt, ++curSubMesh)
        {
            glTF::Primitive& prim = m_meshes[curMesh].primitives[curSubMesh];
            json& thisPrim = primIt.value();

            prim.attribMask = 0;
            json& attributes = thisPrim.at("attributes");

            FindAttribute(prim, attributes, Primitive::kPosition, "POSITION");
            FindAttribute(prim, attributes, Primitive::kNormal, "NORMAL");
            FindAttribute(prim, attributes, Primitive::kTangent, "TANGENT");
            FindAttribute(prim, attributes, Primitive::kTexcoord0, "TEXCOORD_0");
            FindAttribute(prim, attributes, Primitive::kTexcoord1, "TEXCOORD_1");
            FindAttribute(prim, attributes, Primitive::kColor0, "COLOR_0");



            prim.mode = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            prim.indices = nullptr;
            prim.material = nullptr;
            prim.minIndex = 0;
            prim.maxIndex = 0;
            prim.mode = 4;

            if (thisPrim.find("mode") != thisPrim.end())
                prim.mode = thisPrim.at("mode");

            if (thisPrim.find("indices") != thisPrim.end())
            {
                uint32_t accessorIndex = thisPrim.at("indices");
                json& indicesAccessor = accessors[accessorIndex];
                prim.indices = &m_accessors[accessorIndex];
                if (indicesAccessor.find("max") != indicesAccessor.end())
                    prim.maxIndex = indicesAccessor.at("max")[0];
                if (indicesAccessor.find("min") != indicesAccessor.end())
                    prim.minIndex = indicesAccessor.at("min")[0];
            }

            if (thisPrim.find("material") != thisPrim.end())
                prim.material = &m_materials[thisPrim.at("material")];

            // TODO:  Add morph targets
            //if (thisPrim.find("targets") != thisPrim.end())
        }
    }
}


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

    if (info_json.find("index") != info_json.end())
        info = &m_textures[info_json.at("index")];

    if (info_json.find("texCoord") != info_json.end())
        return info_json.at("texCoord");
    else
        return 0;
}

void glTF::Asset::ProcessMaterials(json& materials)
{
    m_materials.reserve(materials.size());

    uint32_t materialIdx = 0;

    for (json::iterator it = materials.begin(); it != materials.end(); ++it)
    {
        glTF::Material material;
        json& thisMaterial = it.value();

        material.index = materialIdx++;
        material.flags = 0;
        material.alphaCutoff = floatToHalf(0.5f);
        material.normalTextureScale = 1.0f;

        if (thisMaterial.find("alphaMode") != thisMaterial.end())
        {
            string alphaMode = thisMaterial.at("alphaMode");
            if (alphaMode == "BLEND")
                material.alphaBlend = true;
            else if (alphaMode == "MASK")
                material.alphaTest = true;
        }

        if (thisMaterial.find("alphaCutoff") != thisMaterial.end())
        {
            material.alphaCutoff = floatToHalf(thisMaterial.at("alphaCutoff"));
            //material.alphaTest = true;  // Should we alpha test and alpha blend?
        }

        if (thisMaterial.find("pbrMetallicRoughness") != thisMaterial.end())
        {
            json& metallicRoughness = thisMaterial.at("pbrMetallicRoughness");

            material.baseColorFactor[0] = 1.0f;
            material.baseColorFactor[1] = 1.0f;
            material.baseColorFactor[2] = 1.0f;
            material.baseColorFactor[3] = 1.0f;
            material.metallicFactor = 1.0f;
            material.roughnessFactor = 1.0f;
            for (uint32_t i = 0; i < Material::kNumTextures; ++i)
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

        if (thisMaterial.find("doubleSided") != thisMaterial.end())
            material.twoSided = thisMaterial.at("doubleSided");

        if (thisMaterial.find("normalTextureScale") != thisMaterial.end())
            material.normalTextureScale = thisMaterial.at("normalTextureScale");

        if (thisMaterial.find("emissiveFactor") != thisMaterial.end())
            ReadFloats(thisMaterial.at("emissiveFactor"), material.emissiveFactor);

        if (thisMaterial.find("occlusionTexture") != thisMaterial.end())
            material.occlusionUV = ReadTextureInfo(thisMaterial.at("occlusionTexture"),
                material.textures[Material::kOcclusion]);

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
    struct _stat64 fileStat;
    int fileExists = _wstat64(fileName.c_str(), &fileStat);
    if (fileExists == -1)
        return false;

    ifstream file(fileName, ios::in | ios::binary);
    if (!file)
        return false;

    ASSERT(Size == (size_t)fileStat.st_size);
    file.read((char*)Dest, Size);
    file.close();

    return true;
}

void glTF::Asset::ProcessBuffers(json& buffers, ByteArray chunk1bin) {}

void glTF::Asset::ProcessBufferViews(json& bufferViews) {}

void glTF::Asset::ProcessImages(json& images) {}

D3D12_TEXTURE_ADDRESS_MODE GLtoD3DTextureAddressMode(int32_t glWrapMode) {}

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

void glTF::Asset::ProcessSamplers(json& samplers) {}

void glTF::Asset::ProcessTextures(json& textures) {}


void glTF::Asset::Parse(const std::wstring& filepath) {}
