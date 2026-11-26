#pragma once

#ifndef __GFL2_STYLIZED_BRDF_HLSLI__
#define __GFL2_STYLIZED_BRDF_HLSLI__

#pragma pack_matrix(row_major)

// ============================================================================
// 模块：GFL2StylizedBRDF.hlsli（少前2 AK-12 主体 PBR+NPR 4行 Ramp 核心着色库）
// 对标：AK12-frame7206.rdc FS_8179_Character_PBR_NPR_Ramp_FS & FS_8177_BodyNails
// ============================================================================

#include "08_Character/CharacterCommon.hlsli"

struct GFL2StylizedSurface
{
    float3 BaseColor;
    float3 N_ws;
    float3 V_ws;
    float3 L_ws;
    float  Roughness;
    float  Metallic;
    float  AO;
    float  EmissiveMask;
    float  ShadowVis;
};

struct GFL2StylizedLightingResult
{
    float3 DirectDiffuse;
    float3 DirectSpecular;
    float3 IblDiffuse;
    float3 IblSpecular;
    float3 Emissive;
    float3 FinalHDR;
};

// 按 UE5 正交矩阵 + 归一化 ST 防退化安全应用 UV 缩放偏移
float2 GFL2_ApplyUVTransform(float2 uv, float4 st)
{
    if (abs(st.x) > 100.0f || abs(st.y) > 100.0f || abs(st.z) > 100.0f || abs(st.w) > 100.0f)
        return uv;
    if (abs(st.x) < 1e-5f && abs(st.y) < 1e-5f)
        return uv;
    return uv * st.xy + st.zw;
}

// UE5 基 L2 SH 求值（逐项严格对应 C++ GFL2::MathConv::EvaluateSH_L2）
float3 GFL2_EvaluateSH_L2(float4 sh[7], float3 n)
{
    const float4 n4 = float4(n, 1.0f);
    float3 res;
    res.r = dot(sh[0], n4);
    res.g = dot(sh[1], n4);
    res.b = dot(sh[2], n4);

    const float4 vB = n.xyzz * n.yzzx; // (x*y, y*z, z*z, z*x)
    res.r += dot(sh[3], vB);
    res.g += dot(sh[4], vB);
    res.b += dot(sh[5], vB);

    const float vC = n.x * n.x - n.y * n.y;
    res += sh[6].rgb * vC;
    return res;
}

// L2 SH 环境漫反射 + 皮肤 DC 亮度归一化（对应 FS_8179:160-184, cb2[30].w）
float3 GFL2_ComputeCharacterSHDiffuse(
    GFL2DrawData drawData,
    GFL2MaterialData mat,
    float3 N_ws,
    bool enableSkinDCNorm)
{
    if ((drawData.Flags & 1u) == 0u)
        return float3(0.22f, 0.22f, 0.24f);

    const float3 shRaw = GFL2_EvaluateSH_L2(drawData.SH_L2, N_ws);
    const bool useSkinNorm = enableSkinDCNorm && ((mat.MaterialFlags & GFL2_MAT_FLAG_SKIN_SH_DC_NORM) != 0u);
    if (!useSkinNorm)
        return max(shRaw, float3(0.0f, 0.0f, 0.0f));

    // L0 DC 不变项 = sh[0..2].w + sh[3..5].z / 3（经 UnityToUE5SH_L2 严格保真）
    const float3 kLumW     = float3(0.212673f, 0.715152f, 0.072175f);
    const float3 shClamped = max(shRaw, float3(0.001f, 0.001f, 0.001f));
    const float  shLum     = max(dot(shClamped, kLumW), 1e-5f);
    const float3 shDC      = float3(drawData.SH_L2[3].z, drawData.SH_L2[4].z, drawData.SH_L2[5].z) * 0.3333f
                           + float3(drawData.SH_L2[0].w, drawData.SH_L2[1].w, drawData.SH_L2[2].w);
    const float  dcLum     = max(dot(shDC, kLumW), 0.0f);
    return (shClamped / shLum) * dcLum;
}

// 采样 4 行 HDR RampMap (V = 0.125 Diffuse, 0.375 Specular, 0.625 Rim, 0.875 AddLight)
float3 GFL2_SampleRampRow(uint rampSrvIdx, float u, float rowV)
{
    if (rampSrvIdx == GFL2_INVALID_BINDLESS || rampSrvIdx <= 2u)
        return float3(u, u, u);
    Texture2D rampTex = LOAD_BINDLESS_NONUNIFORM(Texture2D, rampSrvIdx);
    return rampTex.SampleLevel(g_SampLinearClamp, float2(saturate(u), rowV), 0.0f).rgb;
}

// 主体角色/服饰/武器/指甲完整 PBR+NPR Ramp 着色评估
GFL2StylizedLightingResult GFL2_EvaluateBodyStylizedBRDF(
    GFL2DrawData drawData,
    GFL2MaterialData mat,
    GFL2StylizedSurface surf,
    bool enableRamp,
    bool enableSkinDCNorm,
    float iblIntensity)
{
    GFL2StylizedLightingResult res;

    const float rough = clamp(surf.Roughness, 0.02f, 1.0f);
    const float alpha = rough * rough;

    const float NdotL        = saturate(dot(surf.N_ws, surf.L_ws));
    const float NdotV        = saturate(dot(surf.N_ws, surf.V_ws));
    const float NdotVClamped = clamp(NdotV + 1e-5f, 0.0f, 1.0f);

    float3 H_ws = surf.V_ws + surf.L_ws;
    const float hLenSq = dot(H_ws, H_ws);
    H_ws = (hLenSq > 1e-6f) ? (H_ws * rsqrt(hLenSq)) : surf.N_ws;
    const float NdotH = saturate(dot(surf.N_ws, H_ws));
    const float VdotH = saturate(dot(surf.V_ws, H_ws));
    const float LdotH = saturate(dot(surf.L_ws, H_ws));

    const float3 diffAlbedo = surf.BaseColor * (1.0f - surf.Metallic);
    const float3 F0         = lerp(float3(0.04f, 0.04f, 0.04f), surf.BaseColor, surf.Metallic);

    const bool useRamp = enableRamp &&
                         ((mat.MaterialFlags & GFL2_MAT_FLAG_RAMP_ENABLE) != 0u) &&
                         (mat.RampMapIdx > 2u) &&
                         (mat.RampMapIdx != GFL2_INVALID_BINDLESS);

    // 1. 直接漫反射 Ramp (V = 0.125)
    const float  atten    = surf.ShadowVis * NdotL;
    const float  uDiff    = max(atten, 0.000061f);
    const float3 diffRamp = useRamp ? GFL2_SampleRampRow(mat.RampMapIdx, uDiff, 0.125f) : atten.xxx;

    // 2. 峰值归一化 GGX 高光 Ramp (V = 0.375)
    const float aNoH   = alpha * NdotH;
    const float dDenom = max((1.0f - NdotH * NdotH) + aNoH * aNoH, 1e-6f);
    const float dRatio = alpha / dDenom;
    const float D      = min(dRatio * dRatio, 2048.0f);

    const float oneMinusA = 1.0f - alpha;
    const float vDenom    = NdotVClamped * (NdotL * oneMinusA + alpha) + NdotL * (NdotVClamped * oneMinusA + alpha);
    const float V         = min(0.5f / max(vDenom, 1e-4f), 1.0f);

    const float F90         = saturate(F0.g * 50.0f);
    const float oneMinusVoH = max(1.0f - VdotH, 0.001f);
    const float x5          = oneMinusVoH * oneMinusVoH * oneMinusVoH * oneMinusVoH * oneMinusVoH;
    const float F_mod       = 1.0f + (F90 - 1.0f) * x5;

    const float dMax      = min(1.0f / max(alpha * alpha, 1e-7f), 2048.0f);
    const float vMaxDenom = VdotH * (LdotH * oneMinusA + alpha) + LdotH * (VdotH * oneMinusA + alpha);
    const float vMax      = min(0.5f / max(vMaxDenom, 1e-4f), 1.0f);

    float3 specRaw;
    if (useRamp)
    {
        const float  uSpec    = saturate((D * V) / max(dMax * vMax, 1e-6f));
        const float3 specRamp = GFL2_SampleRampRow(mat.RampMapIdx, uSpec, 0.375f);
        specRaw = clamp(dMax * vMax * F_mod * specRamp, 0.0f, 10.0f);
    }
    else
    {
        specRaw = clamp((D * V * F_mod).xxx, 0.0f, 10.0f);
    }
    const float3 specBRDF = F0 * specRaw;

    const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
    res.DirectDiffuse  = diffAlbedo * diffRamp * lightCol;
    res.DirectSpecular = specBRDF   * diffRamp * lightCol;

    // 3. EnvBRDFApprox + 主光同侧相机 Rim Light (V = 0.625) + 5682 反射探针
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.040f, -0.040f);
    const float4 rApprox = rough * c0 + c1;
    const float  a004    = min(rApprox.x * rApprox.x, exp2(-9.28f * NdotV)) * rApprox.x + rApprox.y;
    const float  envA    = -1.04f * a004 + rApprox.z;
    float        envB    =  1.04f * a004 + rApprox.w;

    if (useRamp)
    {
        // UE5 View 矩阵第一列对应 CameraRight_ws
        float3 camRight = float3(g_View._11, g_View._21, g_View._31);
        const float crLenSq = dot(camRight, camRight);
        camRight = (crLenSq > 1e-6f) ? (camRight * rsqrt(crLenSq)) : float3(0.0f, 1.0f, 0.0f);

        const float  lViewX   = dot(surf.L_ws, camRight);
        const float  sideSign = (lViewX > 1e-5f) ? 1.0f : ((lViewX < -1e-5f) ? -1.0f : 0.0f);
        const float3 sideDir  = camRight * sideSign;
        const float  uRim     = envB * saturate(dot(surf.N_ws, sideDir));
        envB = GFL2_SampleRampRow(mat.RampMapIdx, uRim, 0.625f).x;
    }

    const float3 envFresnel = F0 * envA + envB.xxx;
    const float3 reflDir    = reflect(-surf.V_ws, surf.N_ws);
    const float3 probe      = GFL2_SampleReflectionProbe(mat.ReflectionProbeIdx, reflDir, surf.Roughness * 6.0f);
    res.IblSpecular         = probe * envFresnel * max(iblIntensity, 0.0f);

    // 4. L2 球谐环境漫反射 + 皮肤 DC 去脏归一化
    const float3 shDiffuse = GFL2_ComputeCharacterSHDiffuse(drawData, mat, surf.N_ws, enableSkinDCNorm);
    res.IblDiffuse         = diffAlbedo * shDiffuse * surf.AO;

    // 5. 自发光 (cb2[16].x * rmo.a) 与后乘校色 (cb2[12].rgb)
    res.Emissive = surf.BaseColor * mat.RawCb2[16].x * surf.EmissiveMask;

    const float4 rawPost  = GFL2_PostMultiplyTint(mat);
    const float3 postMult = (dot(rawPost.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
        ? rawPost.rgb
        : float3(1.0f, 1.0f, 1.0f);

    res.FinalHDR = (res.DirectDiffuse + res.DirectSpecular + res.IblDiffuse + res.IblSpecular + res.Emissive) * postMult;
    return res;
}

#endif
