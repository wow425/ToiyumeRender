// ============================================================================
// 着色器：EnvScenePS.hlsl（M6 整备室静物建筑/Lightmap/附加光/AuxMask=0.0）
// 对标：AK12-frame7206.rdc FS_8171 / FS_8173 / FS_8175 / FS_7295
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

struct GFL2AddLightGpu
{
    float4 PositionAndType;
    float4 ColorAndRange;
    float4 AttenParams;
    float4 SpotDirUE5;
    float4 ShadowParams;
};

cbuffer EnvAdditionalLightsCB : register(b2)
{
    GFL2AddLightGpu g_AddLights[2];
    float4x4        g_AddLight1WorldToShadow;
    float4          g_AddLightCtrl; // x=enabled, y=scale, z=addShadowSrvIdx(uint as float or pass via b0.AuxTex0SrvIdx), w=activeCount
};

float SampleAddLight1Shadow512(float3 worldPos, uint shadowSrvIdx)
{
    if (shadowSrvIdx == GFL2_INVALID_BINDLESS)
        return 1.0f;
    const float4 sc = mul(float4(worldPos, 1.0f), g_AddLight1WorldToShadow);
    if (abs(sc.w) < 1e-5f)
        return 1.0f;
    const float3 ndc = sc.xyz / sc.w;
    if (ndc.x <= 0.0f || ndc.x >= 1.0f || ndc.y <= 0.0f || ndc.y >= 1.0f || ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;

    Texture2D<float> shadowTex = ResourceDescriptorHeap[shadowSrvIdx];
    const float stepUV = 1.5f / 512.0f;
    float vis = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            vis += shadowTex.SampleCmpLevelZero(
                g_SampShadowCmpGreater,
                ndc.xy + float2((float)dx, (float)dy) * stepUV,
                saturate(ndc.z + 0.002f));
        }
    }
    return vis * (1.0f / 9.0f);
}

CharacterPSOutput MainPS(CharacterPSInput input)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint  debugView           = (uint)(g_ToggleFlags0.x + 0.5f);
    const float iblIntensity        = max(g_ToggleFlags0.w, 0.0f);

    const float2 uv0     = input.UV01.xy;
    const float2 uv1     = input.UV01.zw;
    const float2 uvBase  = GFL2_ApplyUVTransform(uv0, mat.RawCb2[1]);
    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);

    // fs=8175 贴花裁剪
    if ((mat.MaterialFlags & 2u) != 0u)
    {
        const float cutoff = (mat.RawCb2[7].y > 0.01f) ? mat.RawCb2[7].y : 0.35f;
        clip(baseTex.a - cutoff);
    }

    float3 tint = float3(1.0f, 1.0f, 1.0f);
    if (dot(mat.RawCb2[2].rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
        tint = mat.RawCb2[2].rgb;
    const float3 baseColor = baseTex.rgb * tint;

    const float3 N_geom = normalize(input.WorldNormal);
    float3 N_ws = N_geom;
    if (mat.NormalIdx > 2u && mat.NormalIdx != GFL2_INVALID_BINDLESS)
    {
        const float4 nRaw = GFL2_SampleBindless2D(mat.NormalIdx, g_SampAnisoWrap, uvBase);
        float2 nXY = float2(nRaw.r * nRaw.a, 1.0f - nRaw.g) * 2.0f - 1.0f;
        nXY *= max(mat.RawCb2[6].x, 0.0f);
        const float nZ = sqrt(saturate(1.0f - dot(nXY, nXY)));
        N_ws = GFL2_TransformNormalTStoWS(float3(nXY, nZ), N_geom, input.WorldTangent);
    }

    const float3 V_ws = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws = normalize(g_MainLightDirToLightAndInt.xyz);

    // SMOE 贴图 (Smoothness, Metallic, AO, EmissiveMask)
    const bool hasSmoe = (mat.RmoOrSdfIdx > 2u && mat.RmoOrSdfIdx != GFL2_INVALID_BINDLESS);
    const float4 smoe  = hasSmoe ? GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampAnisoWrap, uvBase)
                                 : float4(0.35f, 0.0f, 1.0f, 0.0f);
    const float roughness = clamp(1.0f - smoe.r * max(mat.RawCb2[6].z, 0.0f), 0.05f, 1.0f);
    const float metallic  = saturate(smoe.g * mat.RawCb2[6].y);
    const float ao        = lerp(1.0f, smoe.b, saturate(mat.RawCb2[6].w));
    const float emisMask  = hasSmoe ? smoe.a : 0.0f;

    CharacterPSOutput outPS;
    // Env 场景 AuxMask.a = 0.0 (< 0.495) -> 正常接受 SSAO
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.0f);

    if (debugView == 1u)  { outPS.LinearHDR = float4(baseColor, 1.0f); return outPS; }
    if (debugView == 2u)  { outPS.LinearHDR = float4(N_ws * 0.5f + 0.5f, 1.0f); return outPS; }
    if (debugView == 3u)  { outPS.LinearHDR = float4(metallic.xxx, 1.0f); return outPS; }
    if (debugView == 4u)  { outPS.LinearHDR = float4(roughness.xxx, 1.0f); return outPS; }
    if (debugView == 5u)  { outPS.LinearHDR = float4(ao.xxx, 1.0f); return outPS; }
    if (debugView == 7u)
    {
        const float nearZ = max(g_CameraPosAndNear.w, 0.01f);
        const float linZ  = nearZ / max(input.Position.z, 1e-5f);
        outPS.LinearHDR   = float4(saturate(1.0f - (linZ - 0.5f) * 0.25f).xxx, 1.0f);
        return outPS;
    }
    if (debugView == 12u) { outPS.LinearHDR = float4(roughness, metallic, ao, 1.0f); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(GFL2_SHADING_ENV), 1.0f); return outPS; }

    // 发光灯条 (fs=7295)
    if ((mat.MaterialFlags & 4u) != 0u)
    {
        outPS.LinearHDR = float4(baseColor * 2.5f, 1.0f);
        return outPS;
    }

    const float3 diffAlbedo = baseColor * (1.0f - metallic);
    const float3 F0         = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);

    // 1. Unity RGBM Lightmap (5685) + ShadowMask (5686)
    float3 bakedGI = float3(0.18f, 0.20f, 0.23f);
    float  shadowMask = 1.0f;
    if ((mat.MaterialFlags & 1u) != 0u && mat.RampMapIdx > 2u)
    {
        const float2 lmUV  = uv1 * mat.RawCb2[0].xy + mat.RawCb2[0].zw;
        const float4 lmRaw = GFL2_SampleBindless2D(mat.RampMapIdx, g_SampLinearClamp, lmUV);
        const float  rgbm  = pow(max(abs(lmRaw.a), 1e-4f), 2.2f) * 34.49324f;
        bakedGI = lmRaw.rgb * rgbm;
        if (mat.HairSpecAoIdx > 2u)
            shadowMask = GFL2_SampleBindless2D(mat.HairSpecAoIdx, g_SampLinearClamp, lmUV).r;
    }
    else if ((drawData.Flags & 1u) != 0u)
    {
        bakedGI = max(GFL2_EvaluateSH_L2(drawData.SH_L2, N_ws), float3(0.0f, 0.0f, 0.0f));
    }

    // 2. 主方向光 + Reverse-Z Shadow
    const float mainShadow = min(GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_geom), shadowMask);
    const float NdotL      = saturate(dot(N_ws, L_ws));
    const float NdotV      = clamp(saturate(dot(N_ws, V_ws)) + 1e-5f, 0.0f, 1.0f);
    const float3 lightCol  = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);

    float3 H_ws = normalize(V_ws + L_ws);
    const float NdotH = saturate(dot(N_ws, H_ws));
    const float VdotH = saturate(dot(V_ws, H_ws));
    const float alpha = roughness * roughness;
    const float denom = max((1.0f - NdotH * NdotH) + (alpha * NdotH) * (alpha * NdotH), 1e-5f);
    const float D     = min((alpha / denom) * (alpha / denom), 64.0f);
    const float3 F    = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
    const float3 directMain = (diffAlbedo * 0.31831f + F * (D * 0.25f)) * lightCol * (NdotL * mainShadow);

    // 3. 整备室 2 盏附加光 (Light[0] 局域补光 + Light[1] 512² 阴影顶灯)
    float3 addLit = float3(0.0f, 0.0f, 0.0f);
    if (g_AddLightCtrl.x > 0.5f)
    {
        const float addScale = max(g_AddLightCtrl.y, 0.0f);
        [unroll]
        for (int li = 0; li < 2; ++li)
        {
            const float3 toL = g_AddLights[li].PositionAndType.xyz - input.WorldPos;
            const float  distSq = max(dot(toL, toL), 1e-4f);
            const float3 lDir = toL * rsqrt(distSq);
            const float  rangeAtt = pow(max(1.0f - pow(distSq * g_AddLights[li].AttenParams.x, 2.0f), 0.0f), 2.0f) / distSq;
            const float  spotDot  = dot(g_AddLights[li].SpotDirUE5.xyz, lDir);
            const float  spotAtt  = pow(saturate(spotDot * g_AddLights[li].AttenParams.z + g_AddLights[li].AttenParams.w), 2.0f);
            float sh = 1.0f;
            if (li == 1 && g_AddLights[li].ShadowParams.x > 0.5f)
                sh = SampleAddLight1Shadow512(input.WorldPos, g_AuxTex0SrvIdx);
            const float ndl = saturate(dot(N_ws, lDir));
            addLit += diffAlbedo * 0.31831f * g_AddLights[li].ColorAndRange.rgb * (ndl * rangeAtt * spotAtt * sh * addScale);
        }
    }

    // 4. IBL + Emissive
    const float3 probe   = GFL2_SampleReflectionProbe(mat.ReflectionProbeIdx, reflect(-V_ws, N_ws), roughness * 6.0f);
    const float3 iblSpec = probe * (F0 + (1.0f - F0) * pow(1.0f - NdotV, 4.0f) * (1.0f - roughness)) * max(iblIntensity, 0.0f) * 0.5f;
    const float3 emissive = baseColor * mat.RawCb2[3].rgb * emisMask * max(mat.RawCb2[7].x, 0.0f);

    if (debugView == 6u)  { outPS.LinearHDR = float4(emissive, 1.0f); return outPS; }
    if (debugView == 18u) { outPS.LinearHDR = float4(mainShadow.xxx, 1.0f); return outPS; }

    const float3 finalHDR = diffAlbedo * bakedGI * ao + directMain + addLit + iblSpec * ao + emissive;
    outPS.LinearHDR = float4(finalHDR, 1.0f);
    return outPS;
}
