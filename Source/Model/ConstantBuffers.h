#pragma once

// Model专用的cb


#include "../Math/Matrix3.h"
#include "../Math/Matrix4.h"
#include <cstdint>

// 网格world矩阵
struct alignas(256) MeshConstants // alignas C11引入用于内存对齐
{
    Math::Matrix4 World;          // object to world
    Math::Matrix3 WorldIT;       // Object normal to world normal
};

// k表常量。PBR材质顺序 the order of textures for PBR materials
enum {
    kBaseColor, // 基础色
    kNormal,    // 法线
    kEmissive,  // 自发光
    kOcclusion, // 环境光屏蔽
    kMetallicRoughness, // 金属粗造度

    kNumTextures
};

// PBR Material
struct alignas(256) MaterialConstants
{
    float baseColorFactor[4]; // default=[1,1,1,1]
    float emissiveFactor[3]; // default=[0,0,0]
    float normalTextureScale; // default=1
    float metallicFactor; // default=1
    float roughnessFactor; // default=1
    union
    {
        uint32_t flags;
        struct
        {
            // UV0 or UV1 for each texture
            uint32_t baseColorUV : 1;
            uint32_t metallicRoughnessUV : 1;
            uint32_t occlusionUV : 1;
            uint32_t emissiveUV : 1;
            uint32_t normalUV : 1;

            // Three special modes
            uint32_t twoSided : 1;
            uint32_t alphaTest : 1;
            uint32_t alphaBlend : 1;

            uint32_t _pad : 8;

            uint32_t alphaRef : 16; // half float
        };
    };
    float _pad1;

};

struct alignas(256) GlobalConstants
{
    Math::Matrix4 ViewProjMatrix;
    Math::Vector3 CameraPos;
    float         _pad;
};
