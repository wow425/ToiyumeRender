// ============================================================================
// 着色器：CharacterBaseColorPS.hlsl（M2 X 光机全通道 0..14,18 + Reverse-Z 阴影 + 5682 IBL + AuxMask MRT）
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

CharacterPSOutput MainPS(CharacterPSInput input)
{
    const GFL2MaterialData mat = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint debugView = (uint)(g_ToggleFlags0.x + 0.5f);

    const float2 uv0 = input.UV01.xy;
    const float2 uv1 = input.UV01.zw;

    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uv0);
    float3 tint = float3(1.0f, 1.0f, 1.0f);
    if (mat.ShadingModelId != GFL2_SHADING_EYE_BASE)
    {
        const float4 rawTint = GFL2_BaseColorTint(mat);
        if (dot(rawTint.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
            tint = rawTint.rgb;
    }
    const float3 baseColor = baseTex.rgb * tint;

    const float3 N_geom = normalize(input.WorldNormal);
    const float3 N_ws   = GFL2_ResolveWorldNormal(mat, uv0, N_geom, input.WorldTangent);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);

    // 解包 RMO (仅 PBR 部位含 RMO; 皮肤/面部/头发/眼睛无 RMO 时采用安全默认)
    const bool hasRmo = (mat.RmoOrSdfIdx > 2u) &&
                        ((mat.MaterialFlags & GFL2_MAT_FLAG_HAS_SDF_MASK) == 0u) &&
                        (mat.ShadingModelId != GFL2_SHADING_FACE) &&
                        (mat.ShadingModelId != GFL2_SHADING_HAIR) &&
                        (mat.ShadingModelId != GFL2_SHADING_EYE_BASE);
    const float4 rmo = hasRmo ? GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampAnisoWrap, uv0)
                              : float4(0.65f, 0.0f, 1.0f, 0.0f);
    const float roughness = rmo.r;
    const float metallic  = rmo.g;
    float ao              = rmo.b;
    const float emisMask  = hasRmo ? rmo.a : 0.0f;

    if (mat.ShadingModelId == GFL2_SHADING_HAIR && (mat.MaterialFlags & GFL2_MAT_FLAG_HAS_HAIR_SPEC_AO) != 0u)
        ao = GFL2_SampleBindless2D(mat.HairSpecAoIdx, g_SampAnisoWrap, uv0).a;

    const float shadowVis = GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_geom);

    // 写入 AuxMask MRT (供后续 M2/M6 复用与 SSAO 角色豁免)
    CharacterPSOutput outPS;
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.498f);

    if (debugView == 1u) // BaseColor
    {
        outPS.LinearHDR = float4(baseColor, 1.0f);
        return outPS;
    }
    if (debugView == 2u) // Normal (Unpacked UE5 World Space)
    {
        outPS.LinearHDR = float4(N_ws * 0.5f + 0.5f, 1.0f);
        return outPS;
    }
    if (debugView == 3u) // Metallic
    {
        outPS.LinearHDR = float4(metallic.xxx, 1.0f);
        return outPS;
    }
    if (debugView == 4u) // Roughness
    {
        outPS.LinearHDR = float4(roughness.xxx, 1.0f);
        return outPS;
    }
    if (debugView == 5u) // AO
    {
        outPS.LinearHDR = float4(ao.xxx, 1.0f);
        return outPS;
    }
    if (debugView == 6u) // Emission
    {
        outPS.LinearHDR = float4(baseColor * emisMask, 1.0f);
        return outPS;
    }
    if (debugView == 7u) // Reverse-Z Depth (线性化高对比灰度)
    {
        const float nearZ = max(g_CameraPosAndNear.w, 0.01f);
        const float zNdc  = max(input.Position.z, 1e-5f);
        const float linZ  = nearZ / zNdc;
        const float vis   = saturate(1.0f - (linZ - 0.5f) * 0.25f);
        outPS.LinearHDR   = float4(vis.xxx, 1.0f);
        return outPS;
    }
    if (debugView == 8u) // FaceSDF (C_face_b on UV1)
    {
        if (mat.ShadingModelId == GFL2_SHADING_FACE)
        {
            const float4 sdf = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, uv1);
            outPS.LinearHDR = float4(sdf.rgb, 1.0f);
        }
        else
        {
            outPS.LinearHDR = float4(0.06f, 0.06f, 0.06f, 1.0f);
        }
        return outPS;
    }
    if (debugView == 10u) // SmoothNormal (COLOR_0.rgb -> UE5 WS)
    {
        const float3 snTS = saturate(input.Color0.rgb) * 2.0f - 1.0f;
        const float3 snWS = GFL2_TransformNormalTStoWS(snTS, N_geom, input.WorldTangent);
        outPS.LinearHDR = float4(snWS * 0.5f + 0.5f, 1.0f);
        return outPS;
    }
    if (debugView == 11u) // OutlineWeight (COLOR_0.a)
    {
        const float w = saturate(input.Color0.a);
        outPS.LinearHDR = float4(w, w, w, 1.0f);
        return outPS;
    }
    if (debugView == 12u) // RMO_Channel (R=Rough, G=Metal, B=AO)
    {
        outPS.LinearHDR = float4(roughness, metallic, ao, 1.0f);
        return outPS;
    }
    if (debugView == 13u) // ShadingModelID
    {
        outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(mat.ShadingModelId), 1.0f);
        return outPS;
    }
    if (debugView == 14u) // UV0_UV1 棋盘格
    {
        const bool useUv1 = (mat.MaterialFlags & GFL2_MAT_FLAG_USE_SECOND_UV) != 0u;
        const float2 uvChk = useUv1 ? uv1 : uv0;
        const float2 grid  = floor(uvChk * 24.0f);
        const float  chk   = fmod(grid.x + grid.y, 2.0f);
        const float3 cA    = useUv1 ? float3(0.15f, 0.85f, 0.65f) : float3(0.88f, 0.86f, 0.82f);
        const float3 cB    = useUv1 ? float3(0.05f, 0.25f, 0.35f) : float3(0.18f, 0.22f, 0.28f);
        outPS.LinearHDR    = float4(lerp(cB, cA, abs(chk)), 1.0f);
        return outPS;
    }
    if (debugView == 18u) // ShadowVisibility (9-Tap Reverse-Z PCF)
    {
        outPS.LinearHDR = float4(shadowVis.xxx, 1.0f);
        return outPS;
    }

    // M2 Final 预览着色：直接光受阴影 + L2 SH 柔漫反射 + 5682 ReflectionProbe6 镜面 IBL (M3 升级为完整 PBR+NPR Ramp)
    const float ndotl   = saturate(dot(N_ws, L_ws));
    const float ndotv   = saturate(dot(N_ws, V_ws));
    const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);

    const float3 diffAlb = baseColor * (1.0f - metallic);
    const float3 F0      = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    const float3 direct  = diffAlb * (ndotl * shadowVis * 0.85f + 0.15f) * lightCol;

    float3 iblSpec = float3(0.0f, 0.0f, 0.0f);
    if (g_ToggleFlags0.w > 0.001f)
    {
        const float3 reflDir = reflect(-V_ws, N_ws);
        const float3 probe   = GFL2_SampleReflectionProbe(mat.ReflectionProbeIdx, reflDir, roughness * 6.0f);
        const float3 F_ibl   = F0 + (max(1.0f - roughness, F0) - F0) * pow(1.0f - ndotv, 5.0f);
        iblSpec = probe * F_ibl * ao * g_ToggleFlags0.w;
    }

    const float3 ambient = baseColor * (0.28f * ao);
    outPS.LinearHDR = float4(direct + ambient + iblSpec, 1.0f);
    return outPS;
}
