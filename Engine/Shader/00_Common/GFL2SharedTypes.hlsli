#pragma once

#ifndef __GFL2_SHARED_TYPES_HLSLI__
#define __GFL2_SHARED_TYPES_HLSLI__

#pragma pack_matrix(row_major)

// ============================================================================
// 模块：GFL2SharedTypes.hlsli（少前2复刻 · HLSL / C++ 16B 同构契约）
// 约定：与 Engine/03_AssetSystem/Assets/Constants/GFL2SharedTypes.h 逐字节对齐。
// 基准：UE5 LH Z-Up (+X Fwd, +Y Right, +Z Up) · 行向量左乘 mul(v, M) · Reverse-Z
// ============================================================================

static const uint GFL2_INVALID_BINDLESS = 0xFFFFFFFFu;

// ShadingModel 离散 ID（对齐 C++ GFL2::ShadingModel & DebugView 13）
static const uint GFL2_SHADING_BODY        = 0u;
static const uint GFL2_SHADING_CLOTH       = 1u;
static const uint GFL2_SHADING_WEAPON      = 2u;
static const uint GFL2_SHADING_NAILS       = 3u;
static const uint GFL2_SHADING_FACE        = 4u;
static const uint GFL2_SHADING_HAIR        = 5u;
static const uint GFL2_SHADING_EYE_BASE    = 6u;
static const uint GFL2_SHADING_EYE_ADD     = 7u;
static const uint GFL2_SHADING_EYE_MUL     = 8u;
static const uint GFL2_SHADING_OUTLINE     = 9u;
static const uint GFL2_SHADING_TRANSPARENT = 10u;
static const uint GFL2_SHADING_ENV         = 11u;

// MaterialFlagBits
static const uint GFL2_MAT_FLAG_USE_SECOND_UV       = (1u << 0);
static const uint GFL2_MAT_FLAG_RAMP_ENABLE         = (1u << 1);
static const uint GFL2_MAT_FLAG_SKIN_SH_DC_NORM     = (1u << 2);
static const uint GFL2_MAT_FLAG_SHADOW_LIGHT_BIAS   = (1u << 3);
static const uint GFL2_MAT_FLAG_HAS_BASE_ALPHA      = (1u << 4);
static const uint GFL2_MAT_FLAG_HAS_HAIR_SPEC_AO    = (1u << 5);
static const uint GFL2_MAT_FLAG_HAS_SDF_MASK        = (1u << 6);
static const uint GFL2_MAT_FLAG_HAS_EYE_BLEND       = (1u << 7);
static const uint GFL2_MAT_FLAG_HAS_VALID_SH        = (1u << 8);
static const uint GFL2_MAT_FLAG_IS_TRANS_BACK       = (1u << 9);

// 256 字节 Per-Draw 结构（StructuredBuffer<GFL2DrawData>）
struct GFL2DrawData
{
    float4x4 WorldMatrix;
    float4x4 WorldIT;
    float4   SH_L2[7];
    uint     MaterialIndex;
    uint     SubmeshIndex;
    uint     EventId;
    uint     Flags;
};

// 560 字节 Per-Material 结构（StructuredBuffer<GFL2MaterialData>）
struct GFL2MaterialData
{
    uint   BaseColorIdx;
    uint   NormalIdx;
    uint   RmoOrSdfIdx;
    uint   RampMapIdx;

    uint   HairSpecAoIdx;
    uint   EyeBlendIdx;
    uint   ReflectionProbeIdx;
    uint   MaterialFlags;

    uint   EventId;
    uint   ShadingModelId;
    uint   CullMode;
    uint   StencilRefAndPreset;

    float4 RawCb2[32];
};

// 通用 RootConstants（b0，16 DWORD = 64 字节）
cbuffer GFL2RootCB : register(b0)
{
    uint  g_DrawIndex;
    uint  g_MaterialIndex;
    uint  g_DrawDataSrvIdx;
    uint  g_MaterialDataSrvIdx;

    uint  g_MainShadowMapSrvIdx;
    uint  g_ReflectionProbeSrvIdx;
    uint  g_AuxTex0SrvIdx;
    uint  g_AuxTex1SrvIdx;

    uint  g_AuxUavIdx;
    uint  g_PassSubMode;
    float g_PassParam0;
    float g_PassParam1;

    float g_PassParam2;
    float g_PassParam3;
    uint  g_Reserved0;
    uint  g_Reserved1;
};

// 全局帧常量（b1 Dynamic CBV，512 字节）
cbuffer GFL2FrameCB : register(b1)
{
    float4x4 g_ViewProj;
    float4x4 g_InvViewProj;
    float4x4 g_View;
    float4x4 g_Proj;
    float4x4 g_MainLightViewProj;

    float4   g_CameraPosAndNear;          // xyz=UE5 Camera Pos, w=NearClip
    float4   g_CameraFwdAndFar;           // xyz=UE5 Camera Fwd, w=FarClip
    float4   g_MainLightDirToLightAndInt; // xyz=UE5 指向光源单位方向, w=Intensity
    float4   g_MainLightColorAndShadow;   // rgb=Color, w=ShadowStrength
    float4   g_ScreenSizeAndInv;          // xy=(W,H), zw=(1/W, 1/H)

    float4   g_HeadOriginUE5;
    float4   g_HeadForwardUE5;
    float4   g_HeadRightUE5;

    float4   g_ToggleFlags0;              // x=DebugView, y=EnableRamp, z=SkinSHDCNorm, w=IBLIntensity
    float4   g_ToggleFlags1;              // x=OutlineWidthScale, y=OutlineZBias, z=MinPx, w=MaxPx
    float4   g_ToggleFlags2;              // x=FaceSdfSmooth, y=HairSpecInt, z=HairViewZShift, w=BangsAlpha
    float4   g_ShadowBiasParams;          // x=NormalBias, y=DepthBias, z=PCFRadius, w=FeatureMaskBits
};

// 语义辅助读取器（对标 cb2 decodedSemantics）
float4 GFL2_BaseMapST(GFL2MaterialData mat)        { return mat.RawCb2[1]; }
float4 GFL2_NormalMapST(GFL2MaterialData mat)      { return mat.RawCb2[3]; }
float4 GFL2_RmoMapST(GFL2MaterialData mat)         { return mat.RawCb2[2]; }
float4 GFL2_FresnelColorA(GFL2MaterialData mat)    { return mat.RawCb2[5]; }
float4 GFL2_FresnelColorB(GFL2MaterialData mat)    { return mat.RawCb2[6]; }
float4 GFL2_BaseColorTint(GFL2MaterialData mat)    { return mat.RawCb2[11]; }
float4 GFL2_PostMultiplyTint(GFL2MaterialData mat) { return mat.RawCb2[12]; }
float4 GFL2_OutlineLitColor(GFL2MaterialData mat)  { return mat.RawCb2[13]; }
float4 GFL2_OutlineShadowCol(GFL2MaterialData mat) { return mat.RawCb2[14]; }
float4 GFL2_OutlineParams(GFL2MaterialData mat)    { return mat.RawCb2[15]; } // x=width, y=zBias, z=mult
float4 GFL2_EmissiveHairSpec(GFL2MaterialData mat) { return mat.RawCb2[16]; } // x=emissiveScale, z=specInt, w=viewShift
float4 GFL2_FaceSdfParams(GFL2MaterialData mat)    { return mat.RawCb2[17]; } // x=smooth, y=shadowDirOffset
float4 GFL2_FeatureFlags(GFL2MaterialData mat)     { return mat.RawCb2[30]; } // x=bias, y=ramp, z=uv1, w=shDCNorm
float4 GFL2_Anisotropy(GFL2MaterialData mat)       { return mat.RawCb2[31]; }

#endif
