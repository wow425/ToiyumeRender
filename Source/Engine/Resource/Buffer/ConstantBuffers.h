#pragma once

#include "../Math/Matrix3.h"
#include "../Math/Matrix4.h"
#include <cstdint>

// 网格world矩阵
struct alignas(256) MeshConstants // alignas C11引入用于内存对齐
{
    Math::Matrix4 World;   // object to world
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
    float normalTextureScale; // default=1
    float emissiveFactor[3]; // default=[0,0,0]
    float metallicFactor; // default=1
    float roughnessFactor; // default=1

    // union结构体用于优化，暂不实现
};

struct alignas(256) GlobalConstants
{
    Math::Matrix4 ViewProjMatrix;
    Math::Matrix4 CameraPos;
};
