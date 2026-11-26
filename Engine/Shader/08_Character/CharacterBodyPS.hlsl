// ============================================================================
// 着色器：CharacterBodyPS.hlsl（M3 AK-12 主体 PBR+NPR 4行 Ramp + L2 SH + DebugView 0..18）
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

CharacterPSOutput MainPS(CharacterPSInput input, bool isFrontFace : SV_IsFrontFace)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint  debugView        = (uint)(g_ToggleFlags0.x + 0.5f);
    const bool  enableRamp       = (g_ToggleFlags0.y > 0.5f);
    const bool  enableSkinDCNorm = (g_ToggleFlags0.z > 0.5f);
    const float iblIntensity     = max(g_ToggleFlags0.w, 0.0f);

    const float2 uv0     = input.UV01.xy;
    const float2 uv1     = input.UV01.zw;
    const float2 uvBase  = GFL2_ApplyUVTransform(uv0, GFL2_BaseMapST(mat));
    const float2 uvRmo   = GFL2_ApplyUVTransform(uv0, GFL2_RmoMapST(mat));
    const float2 uvNorm  = GFL2_ApplyUVTransform(uv0, GFL2_NormalMapST(mat));

    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);
    float3 tint = float3(1.0f, 1.0f, 1.0f);
    if (mat.ShadingModelId != GFL2_SHADING_EYE_BASE)
    {
        const float4 rawTint = GFL2_BaseColorTint(mat);
        if (dot(rawTint.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
            tint = rawTint.rgb;
    }

    // 双面外套 (EID 2994 CullNone) 背面自动翻转几何法线，保证内衬正确受光
    const float3 N_geom = normalize(isFrontFace ? input.WorldNormal : -input.WorldNormal);
    const float3 N_ws   = GFL2_ResolveWorldNormal(mat, uvNorm, N_geom, input.WorldTangent);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);

    float3 baseColor = baseTex.rgb * tint;
    // FS:8177 指甲/丝袜变体支持视角菲涅耳前景色插值
    if (mat.ShadingModelId == GFL2_SHADING_NAILS)
    {
        const float fresPow = max(mat.RawCb2[7].x, 0.001f);
        const float fresT   = pow(saturate(dot(N_ws, V_ws)), fresPow);
        baseColor *= lerp(GFL2_FresnelColorB(mat).rgb, GFL2_FresnelColorA(mat).rgb, fresT);
    }

    // 解包 RMO (仅 PBR/Ramp 主体含 RMO；面部/头发/眼睛采用安全默认值)
    const bool hasRmo = (mat.RmoOrSdfIdx > 2u) &&
                        ((mat.MaterialFlags & GFL2_MAT_FLAG_HAS_SDF_MASK) == 0u) &&
                        (mat.ShadingModelId != GFL2_SHADING_FACE) &&
                        (mat.ShadingModelId != GFL2_SHADING_HAIR) &&
                        (mat.ShadingModelId != GFL2_SHADING_EYE_BASE);
    const float4 rmo      = hasRmo ? GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampAnisoWrap, uvRmo)
                                   : float4(0.65f, 0.0f, 1.0f, 0.0f);
    const float roughness = rmo.r;
    const float metallic  = rmo.g;
    float       ao        = rmo.b;
    const float emisMask  = hasRmo ? rmo.a : 0.0f;

    if (mat.ShadingModelId == GFL2_SHADING_HAIR && (mat.MaterialFlags & GFL2_MAT_FLAG_HAS_HAIR_SPEC_AO) != 0u)
        ao = GFL2_SampleBindless2D(mat.HairSpecAoIdx, g_SampAnisoWrap, uv0).a;

    const float shadowVis = GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_geom);

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.498f);

    if (debugView == 1u) // BaseColor
    {
        outPS.LinearHDR = float4(baseColor, 1.0f);
        return outPS;
    }
    if (debugView == 2u) // Normal (GF2 解包 -> UE5 WS)
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
    if (debugView == 7u) // Reverse-Z Depth
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
    if (debugView == 10u) // SmoothNormal
    {
        const float3 snTS = saturate(input.Color0.rgb) * 2.0f - 1.0f;
        const float3 snWS = GFL2_TransformNormalTStoWS(snTS, N_geom, input.WorldTangent);
        outPS.LinearHDR = float4(snWS * 0.5f + 0.5f, 1.0f);
        return outPS;
    }
    if (debugView == 11u) // OutlineWeight
    {
        const float w = saturate(input.Color0.a);
        outPS.LinearHDR = float4(w, w, w, 1.0f);
        return outPS;
    }
    if (debugView == 12u) // RMO_Channel
    {
        outPS.LinearHDR = float4(roughness, metallic, ao, 1.0f);
        return outPS;
    }
    if (debugView == 13u) // ShadingModelID
    {
        outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(mat.ShadingModelId), 1.0f);
        return outPS;
    }
    if (debugView == 14u) // UV0_UV1 Checker
    {
        const bool   useUv1 = (mat.MaterialFlags & GFL2_MAT_FLAG_USE_SECOND_UV) != 0u;
        const float2 uvChk  = useUv1 ? uv1 : uv0;
        const float2 grid   = floor(uvChk * 24.0f);
        const float  chk    = fmod(grid.x + grid.y, 2.0f);
        const float3 cA     = useUv1 ? float3(0.15f, 0.85f, 0.65f) : float3(0.88f, 0.86f, 0.82f);
        const float3 cB     = useUv1 ? float3(0.05f, 0.25f, 0.35f) : float3(0.18f, 0.22f, 0.28f);
        outPS.LinearHDR     = float4(lerp(cB, cA, abs(chk)), 1.0f);
        return outPS;
    }
    if (debugView == 18u) // ShadowVisibility
    {
        outPS.LinearHDR = float4(shadowVis.xxx, 1.0f);
        return outPS;
    }

    GFL2StylizedSurface surf;
    surf.BaseColor    = baseColor;
    surf.N_ws         = N_ws;
    surf.V_ws         = V_ws;
    surf.L_ws         = L_ws;
    surf.Roughness    = roughness;
    surf.Metallic     = metallic;
    surf.AO           = ao;
    surf.EmissiveMask = emisMask;
    surf.ShadowVis    = shadowVis;

    const GFL2StylizedLightingResult lit = GFL2_EvaluateBodyStylizedBRDF(
        drawData, mat, surf, enableRamp, enableSkinDCNorm, iblIntensity);

    if (debugView == 6u) // Emission
    {
        outPS.LinearHDR = float4(lit.Emissive, 1.0f);
        return outPS;
    }
    if (debugView == 15u) // DirectDiffuseRamp (V=0.125)
    {
        outPS.LinearHDR = float4(lit.DirectDiffuse, 1.0f);
        return outPS;
    }
    if (debugView == 16u) // SpecularGGXRamp (V=0.375)
    {
        outPS.LinearHDR = float4(lit.DirectSpecular, 1.0f);
        return outPS;
    }
    if (debugView == 17u) // RimAndIBL (EnvBRDFApprox + V=0.625 + 5682 + SH)
    {
        outPS.LinearHDR = float4(lit.IblDiffuse + lit.IblSpecular, 1.0f);
        return outPS;
    }

    outPS.LinearHDR = float4(lit.FinalHDR, 1.0f);
    return outPS;
}
