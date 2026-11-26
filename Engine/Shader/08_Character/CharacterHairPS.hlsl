// ============================================================================
// 着色器：CharacterHairPS.hlsl（M4 头发 UV1 天使环 + 发丝 AO + 3335 眉眼半透覆盖）
// 对标：AK12-frame7206.rdc FS_8187_Hair_AngelRing_FS & FS_8193_HairBangs_AlphaOverEye_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

CharacterPSOutput MainPS(CharacterPSInput input, bool isFrontFace : SV_IsFrontFace)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint  debugView           = (uint)(g_ToggleFlags0.x + 0.5f);
    const bool  enableRamp          = (g_ToggleFlags0.y > 0.5f);
    const bool  enableSkinDCNorm    = (g_ToggleFlags0.z > 0.5f);
    const float iblIntensity        = max(g_ToggleFlags0.w, 0.0f);
    const bool  enableHairAniso     = (g_PassParam0 > 0.5f);
    const bool  isAlphaOverEye      = (g_PassSubMode == 1u);

    const float2 uv0    = input.UV01.xy;
    const float2 uv1    = input.UV01.zw;
    const bool   useUv1 = (mat.MaterialFlags & GFL2_MAT_FLAG_USE_SECOND_UV) != 0u;
    const float2 uvHair = useUv1 ? uv1 : uv0;

    const float2 uvBase  = GFL2_ApplyUVTransform(uv0, GFL2_BaseMapST(mat));
    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);

    float3 tint = float3(1.0f, 1.0f, 1.0f);
    const float4 rawTint = GFL2_BaseColorTint(mat);
    if (dot(rawTint.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
        tint = rawTint.rgb;
    const float3 baseColor = baseTex.rgb * tint;

    const float3 N_geom = normalize(isFrontFace ? input.WorldNormal : -input.WorldNormal);
    const float3 N_ws   = GFL2_ResolveWorldNormal(mat, uvBase, N_geom, input.WorldTangent);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);

    const bool hasHairSpecAo = (mat.MaterialFlags & GFL2_MAT_FLAG_HAS_HAIR_SPEC_AO) != 0u && (mat.HairSpecAoIdx > 2u);
    const float hairAO = hasHairSpecAo ? GFL2_SampleBindless2D(mat.HairSpecAoIdx, g_SampAnisoWrap, uv0).a : 1.0f;

    const float rawShadow = GFL2_SampleMainShadow9TapPCF(input.WorldPos + L_ws * (mat.RawCb2[17].y * 0.1f), N_geom);
    const float shadowVis = rawShadow * hairAO;

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.498f);

    if (debugView == 1u) { outPS.LinearHDR = float4(baseColor, 1.0f); return outPS; }
    if (debugView == 2u) { outPS.LinearHDR = float4(N_ws * 0.5f + 0.5f, 1.0f); return outPS; }
    if (debugView == 3u) { outPS.LinearHDR = float4(0.0f, 0.0f, 0.0f, 1.0f); return outPS; }
    if (debugView == 4u) { outPS.LinearHDR = float4(0.55f, 0.55f, 0.55f, 1.0f); return outPS; }
    if (debugView == 5u) { outPS.LinearHDR = float4(hairAO.xxx, 1.0f); return outPS; }
    if (debugView == 6u) { outPS.LinearHDR = float4(0.0f, 0.0f, 0.0f, 1.0f); return outPS; }
    if (debugView == 7u)
    {
        const float nearZ = max(g_CameraPosAndNear.w, 0.01f);
        const float linZ  = nearZ / max(input.Position.z, 1e-5f);
        outPS.LinearHDR   = float4(saturate(1.0f - (linZ - 0.5f) * 0.25f).xxx, 1.0f);
        return outPS;
    }
    if (debugView == 8u)  { outPS.LinearHDR = float4(0.06f, 0.06f, 0.06f, 1.0f); return outPS; }
    if (debugView == 10u)
    {
        const float3 snTS = saturate(input.Color0.rgb) * 2.0f - 1.0f;
        const float3 snWS = GFL2_TransformNormalTStoWS(snTS, N_geom, input.WorldTangent);
        outPS.LinearHDR = float4(snWS * 0.5f + 0.5f, 1.0f);
        return outPS;
    }
    if (debugView == 11u) { outPS.LinearHDR = float4(saturate(input.Color0.a).xxx, 1.0f); return outPS; }
    if (debugView == 12u) { outPS.LinearHDR = float4(0.55f, 0.0f, hairAO, 1.0f); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(mat.ShadingModelId), 1.0f); return outPS; }
    if (debugView == 14u)
    {
        const float2 grid = floor(uvHair * 24.0f);
        const float  chk  = abs(fmod(grid.x + grid.y, 2.0f));
        outPS.LinearHDR = float4(lerp(float3(0.05f, 0.25f, 0.35f), float3(0.15f, 0.85f, 0.65f), chk), 1.0f);
        return outPS;
    }
    if (debugView == 18u) { outPS.LinearHDR = float4(shadowVis.xxx, 1.0f); return outPS; }

    const float  NdotL    = saturate(dot(N_ws, L_ws));
    const float  NdotV    = saturate(dot(N_ws, V_ws));
    const float  atten    = NdotL * shadowVis;
    const bool   useRamp  = enableRamp && ((mat.MaterialFlags & GFL2_MAT_FLAG_RAMP_ENABLE) != 0u) && (mat.RampMapIdx > 2u);
    const float3 diffRamp = useRamp ? GFL2_SampleRampRow(mat.RampMapIdx, max(atten, 0.000061f), 0.125f) : atten.xxx;

    // 头发天使环：UE5 Z-Up 垂直视线偏移 UV1.y - V_ws.z * viewZShift（FS_8187:184-191）
    float3 hairSpec = float3(0.0f, 0.0f, 0.0f);
    if (enableHairAniso && hasHairSpecAo)
    {
        const float viewShift = g_ToggleFlags2.z; // 默认 0.05
        const float specInt   = g_ToggleFlags2.y; // 默认 2.0
        const float2 angelUV  = float2(uvHair.x, uvHair.y - V_ws.z * viewShift);
        const float3 specMap  = GFL2_SampleBindless2D(mat.HairSpecAoIdx, g_SampAnisoWrap, angelUV).rgb;
        hairSpec = specMap * NdotV * specInt * (atten * 0.286479f + 0.031831f);
    }

    const float3 lightCol   = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
    const float3 directDiff = baseColor * diffRamp * lightCol;
    const float3 directSpec = hairSpec * lightCol;

    // Rim + IBL + SH
    float envB = 0.018096f;
    if (useRamp)
    {
        float3 camRight = float3(g_View._11, g_View._21, g_View._31);
        const float crLenSq = dot(camRight, camRight);
        camRight = (crLenSq > 1e-6f) ? (camRight * rsqrt(crLenSq)) : float3(0.0f, 1.0f, 0.0f);
        const float lViewX = dot(L_ws, camRight);
        const float sideSign = (lViewX > 1e-5f) ? 1.0f : ((lViewX < -1e-5f) ? -1.0f : 0.0f);
        const float uRim = 0.018096f * saturate(dot(N_ws, camRight * sideSign));
        envB = GFL2_SampleRampRow(mat.RampMapIdx, uRim, 0.625f).x;
    }
    const float3 probe     = GFL2_SampleReflectionProbe(mat.ReflectionProbeIdx, reflect(-V_ws, N_ws), 3.0f);
    const float3 iblSpec   = probe * envB.xxx * max(iblIntensity, 0.0f);
    const float3 shDiffuse = GFL2_ComputeCharacterSHDiffuse(drawData, mat, N_ws, enableSkinDCNorm);
    const float3 iblDiff   = baseColor * shDiffuse * hairAO;

    if (debugView == 15u) { outPS.LinearHDR = float4(directDiff, 1.0f); return outPS; }
    if (debugView == 16u) { outPS.LinearHDR = float4(directSpec, 1.0f); return outPS; }
    if (debugView == 17u) { outPS.LinearHDR = float4(iblDiff + iblSpec, 1.0f); return outPS; }

    const float4 rawPost  = GFL2_PostMultiplyTint(mat);
    const float3 postMult = (dot(rawPost.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f) ? rawPost.rgb : float3(1.0f, 1.0f, 1.0f);
    const float3 finalHDR = (directDiff + directSpec + iblDiff + iblSpec) * postMult;
    const float  outAlpha = isAlphaOverEye ? saturate(g_ToggleFlags2.w) : 1.0f;

    outPS.LinearHDR = float4(finalHDR, outAlpha);
    return outPS;
}
