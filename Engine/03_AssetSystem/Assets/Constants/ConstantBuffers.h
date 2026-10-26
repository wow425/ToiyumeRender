#pragma once

// Model专用的cb


#include "00_Core/Math/Matrix3.h"
#include "00_Core/Math/Matrix4.h"
#include <cstdint>

// Mesh CB
struct alignas(256)MeshConstants // alignas C11引入用于内存对齐 alignas(256)
{
	Math::Matrix4 World;          // object to world
	Math::Matrix4 WorldIT;       // Object normal to world normal
};

// ============================================================
// 材质纹理槽位 —— 全工程唯一权威顺序（R1.1）
// ------------------------------------------------------------
// 背景：改造前存在两套互相冲突的顺序
//   glTF.h::Material       { BaseColor, MetallicRoughness, Occlusion, Emissive, Normal }
//   ConstantBuffers.h      { BaseColor, Normal, Emissive, Occlusion, MetallicRoughness }
// 而 GBufferPS.hlsl 的 t0..t4 用的是**glTF 顺序**。
// 于是 ModelConvert.cpp 里「用全局常量去索引 glTF 数组」的代码全部错位，
// 导致 sRGB 标志、默认贴图整体串槽。
//
// 现在统一到 GPU 侧（HLSL register）实际使用的 glTF 顺序：
//   t0 = BaseColor, t1 = MetallicRoughness, t2 = Occlusion, t3 = Emissive, t4 = Normal
// 这样 SRV 表、采样器表、Descriptor 复制、HLSL 绑定四者天然对齐。
//
// 注意：.ty 模型缓存按槽位顺序序列化 MaterialTextureData.stringIdx，
// 因此修改本顺序后**必须重新烘焙模型**（运行时加 -RebuildModels）。
// ============================================================
enum {
	kBaseColor,         // t0 基础色（sRGB 颜色）
	kMetallicRoughness, // t1 G=roughness, B=metallic（线性数据）
	kOcclusion,         // t2 R=AO（线性数据）
	kEmissive,          // t3 自发光（sRGB 颜色）
	kNormal,            // t4 切线空间法线（线性数据）

	kNumTextures
};

// 与 HLSL 的 t0..t4 一一对应，数量变化必须同步改 Shader 与 RootSignature。
static_assert(kNumTextures == 5, "GBufferPS.hlsl 的 t0..t4 固定为 5 个材质纹理槽");

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
};

// ============================================================
// 全局常量缓冲（GBuffer Pass 的 b1 / kCommonCBV）—— R1.7
// ------------------------------------------------------------
// 这是 C++ 与 HLSL 的**唯一权威定义**。两侧字段名、顺序、类型必须完全一致，
// 否则常量会整体错位（DirectXMath 的 Vector3 是 12 字节，但 HLSL 的 float3
// 在 cbuffer 里按 16 字节对齐排布，混用会直接踩坑）。
//
// 因此这里刻意全部使用 16 字节对齐的类型：
//   Matrix4 = 64B、Vector4 = 16B，整个结构 128B，无隐式填充歧义。
//
// 对应 HLSL（StaticMeshVS.hlsl / GBufferPS.hlsl 的 cbuffer GlobalConstants）：
//   offset  0 : float4x4 ViewProjMatrix
//   offset 64 : float4   CameraPos
//   offset 80 : float4   SunDirection
//   offset 96 : float4   SunIntensity
// 合计 112B，alignas(256) 补齐到 256B 以符合 CBV 对齐要求。
// ============================================================
struct alignas(256) GlobalConstants
{
	Math::Matrix4 ViewProjMatrix;   // world -> clip
	Math::Vector4 CameraPos;        // xyz = 相机世界坐标, w = 未使用
	Math::Vector4 SunDirection;     // xyz = 主光方向,   w = 未使用
	Math::Vector4 SunIntensity;     // xyz = 主光颜色*强度, w = 未使用
};

static_assert(sizeof(GlobalConstants) == 256, "CBV 需要 256 字节对齐的常量缓冲");

