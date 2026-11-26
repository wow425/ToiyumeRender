#pragma once

#ifndef __GFL2_CHARACTER_COMMON_HLSLI__
#define __GFL2_CHARACTER_COMMON_HLSLI__

#pragma pack_matrix(row_major)

// ============================================================================
// 模块：CharacterCommon.hlsli（少前2 AK-12 角色 Bindless 公共着色辅助）
// 架构层：Engine/Shader/08_Character（SM 6.6 + UE5 LH Z-Up + Row-Major + Reverse-Z）
// ============================================================================

#include "00_Common/BindlessCommon.hlsli"
#include "00_Common/GFL2SharedTypes.hlsli"

struct CharacterVSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Tangent  : TANGENT;
    float2 UV0      : TEXCOORD0;
    float2 UV1      : TEXCOORD1;
    float4 Color0   : COLOR0;
};

struct CharacterPSInput
{
    float4 Position     : SV_POSITION;
    float3 WorldPos     : TEXCOORD0;
    float3 WorldNormal  : TEXCOORD1;
    float4 WorldTangent : TEXCOORD2; // xyz=WS tangent, w=bitangent handedness sign
    float4 UV01         : TEXCOORD3; // xy=UV0, zw=UV1
    float4 Color0       : COLOR0;    // rgb=packed TS smooth normal, a=outline width mask
};

struct CharacterPSOutput
{
    float4 LinearHDR : SV_TARGET0;
    float4 AuxMask   : SV_TARGET1; // rgb=WS normal*0.5+0.5, a=0.498 character SSAO exemption flag
};

GFL2DrawData GFL2_LoadDrawData(uint drawIdx)
{
    StructuredBuffer<GFL2DrawData> draws = ResourceDescriptorHeap[g_DrawDataSrvIdx];
    return draws[drawIdx];
}

GFL2MaterialData GFL2_LoadMaterialData(uint matIdx)
{
    StructuredBuffer<GFL2MaterialData> mats = ResourceDescriptorHeap[g_MaterialDataSrvIdx];
    return mats[matIdx];
}

float4 GFL2_SampleBindless2D(uint srvIdx, SamplerState samp, float2 uv)
{
    Texture2D tex = LOAD_BINDLESS_NONUNIFORM(Texture2D, srvIdx);
    return tex.Sample(samp, uv);
}

// UE5 LH Z-Up (+X Fwd, +Y Right, +Z Up) -> Unity LH Y-Up (+X Right, +Y Up, +Z Fwd)
// 供 5682 ReflectionProbe6 等源截帧导出 Cubemap 采样方向精确对齐
float3 GFL2_UE5ToUnityDir(float3 dirUE5)
{
    return float3(dirUE5.y, dirUE5.z, dirUE5.x);
}

float3 GFL2_SampleReflectionProbe(uint cubeSrvIdx, float3 dirUE5, float mipLevel)
{
    if (cubeSrvIdx == GFL2_INVALID_BINDLESS || cubeSrvIdx <= 6u)
        return float3(0.0f, 0.0f, 0.0f);
    TextureCube probe = LOAD_BINDLESS_NONUNIFORM(TextureCube, cubeSrvIdx);
    return probe.SampleLevel(g_SampLinearClamp, normalize(GFL2_UE5ToUnityDir(dirUE5)), mipLevel).rgb;
}

// GF2 *_n 切线空间法线解包（G 通道翻转 + DXT5nm 兼容 nx = R * A）
float3 GFL2_UnpackNormalTS(float4 rawSample)
{
    float2 nXY = float2(rawSample.r * rawSample.a, 1.0f - rawSample.g) * 2.0f - 1.0f;
    float  nZ  = sqrt(saturate(1.0f - dot(nXY, nXY)));
    return float3(nXY, nZ);
}

float3 GFL2_TransformNormalTStoWS(float3 nTS, float3 N_ws, float4 T_ws_and_sign)
{
    float3 N = normalize(N_ws);
    float3 T = normalize(T_ws_and_sign.xyz - N * dot(T_ws_and_sign.xyz, N));
    float3 B = cross(N, T) * T_ws_and_sign.w;
    return normalize(nTS.x * T + nTS.y * B + nTS.z * N);
}

// 若材质未绑定法线贴图（Sentinel <= 2u，注意 kSentinelFlatNormal2D Alpha=0 不可直接走 r*a），回退几何法线
float3 GFL2_ResolveWorldNormal(GFL2MaterialData mat, float2 uv, float3 N_geom, float4 T_and_sign)
{
    if (mat.NormalIdx <= 2u || mat.NormalIdx == GFL2_INVALID_BINDLESS)
        return normalize(N_geom);
    const float4 nRaw = GFL2_SampleBindless2D(mat.NormalIdx, g_SampAnisoWrap, uv);
    const float3 nTS  = GFL2_UnpackNormalTS(nRaw);
    return GFL2_TransformNormalTStoWS(nTS, N_geom, T_and_sign);
}

// UE5 Reverse-Z 9-Tap PCF 主方向光阴影采样（使用 s6 g_SampShadowCmpGreater）
float GFL2_SampleMainShadow9TapPCF(float3 worldPos, float3 worldNormal)
{
    const uint shadowEnabled = (uint)(g_ShadowBiasParams.w + 0.5f);
    if ((shadowEnabled & 1u) == 0u || g_MainShadowMapSrvIdx == GFL2_INVALID_BINDLESS)
        return 1.0f;

    const float normalBias = g_ShadowBiasParams.x;
    const float depthBias  = g_ShadowBiasParams.y;
    const float pcfRadius  = max(g_ShadowBiasParams.z, 0.25f);

    const float3 L = normalize(g_MainLightDirToLightAndInt.xyz);
    const float  ndotl = saturate(dot(worldNormal, L));
    const float3 biasedPosWS = worldPos + worldNormal * (normalBias * (1.0f - 0.5f * ndotl));
    const float4 shadowClip  = mul(float4(biasedPosWS, 1.0f), g_MainLightViewProj);
    if (abs(shadowClip.w) < 1e-6f)
        return 1.0f;
    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;

    const float2 shadowUV = float2(shadowNdc.x * 0.5f + 0.5f, -shadowNdc.y * 0.5f + 0.5f);
    if (shadowUV.x <= 0.0f || shadowUV.x >= 1.0f || shadowUV.y <= 0.0f || shadowUV.y >= 1.0f)
        return 1.0f;
    if (shadowNdc.z <= 0.0f || shadowNdc.z >= 1.0f)
        return 1.0f;

    // Reverse-Z: 更靠近光源深度值更大；接收面深度微加偏置 (> shadowTexel) 即为受光
    const float compareDepth = saturate(shadowNdc.z + depthBias * (1.0f + (1.0f - ndotl)));

    Texture2D<float> shadowTex = ResourceDescriptorHeap[g_MainShadowMapSrvIdx];
    uint w = 4096u, h = 4096u;
    shadowTex.GetDimensions(w, h);
    const float2 texelStep = pcfRadius / float2(max(w, 1u), max(h, 1u));

    float vis = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            vis += shadowTex.SampleCmpLevelZero(
                g_SampShadowCmpGreater,
                shadowUV + float2((float)dx, (float)dy) * texelStep,
                compareDepth);
        }
    }
    vis *= (1.0f / 9.0f);
    return lerp(1.0f, vis, saturate(g_MainLightColorAndShadow.w));
}

float3 GFL2_ShadingModelFalseColor(uint id)
{
    if (id == GFL2_SHADING_BODY)        return float3(0.95f, 0.72f, 0.58f);
    if (id == GFL2_SHADING_CLOTH)       return float3(0.25f, 0.62f, 0.96f);
    if (id == GFL2_SHADING_WEAPON)      return float3(0.95f, 0.80f, 0.18f);
    if (id == GFL2_SHADING_NAILS)       return float3(0.96f, 0.32f, 0.45f);
    if (id == GFL2_SHADING_FACE)        return float3(1.00f, 0.88f, 0.82f);
    if (id == GFL2_SHADING_HAIR)        return float3(0.65f, 0.42f, 0.92f);
    if (id == GFL2_SHADING_EYE_BASE)    return float3(0.15f, 0.88f, 0.75f);
    if (id == GFL2_SHADING_EYE_ADD)     return float3(0.98f, 0.98f, 0.98f);
    if (id == GFL2_SHADING_EYE_MUL)     return float3(0.45f, 0.25f, 0.35f);
    if (id == GFL2_SHADING_OUTLINE)     return float3(0.15f, 0.15f, 0.15f);
    if (id == GFL2_SHADING_TRANSPARENT) return float3(0.35f, 0.95f, 0.45f);
    return float3(0.50f, 0.50f, 0.50f);
}

#endif
