// ============================================================================
// 着色器：CharacterFacePS.hlsl（M4 二次元面部 SDF + 鼻梁高光 + 刘海影重绘 3295）
// 对标：AK12-frame7206.rdc FS_8185_Face_SDF_Ramp_FS & FS_7337_Face_BangsShadowRedraw_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

float GFL2_WrapSdfBand(float val, out float signScale, out float fracVal)
{
    const float x = val + 4.0f;
    const bool  pos = (x >= 0.0f);
    signScale = pos ? 2.0f : -2.0f;
    const float halfScale = pos ? 0.5f : -0.5f;
    fracVal = frac(x * halfScale);
    return fracVal * signScale;
}

float GFL2_EvaluateFaceSDF(
    GFL2DrawData drawData,
    GFL2MaterialData mat,
    float2 uv,
    float3 L_ws,
    float smoothWidth,
    out float outVertFade,
    out float2 outLxz)
{
    // 将世界光向转回 UE5 头部局部空间：L_obj = L_ws * inv(WorldMatrix)
    // UE5 局部空间约定：+X = 前向 (Unity +Z), +Y = 右侧 (Unity +X), +Z = 上方 (Unity +Y)
    float3 L_obj = mul((float3x3)drawData.WorldIT, L_ws);
    const float lenSq = dot(L_obj, L_obj);
    L_obj = (lenSq > 1e-8f) ? (L_obj * rsqrt(lenSq)) : float3(1.0f, 0.0f, 0.0f);

    const float L_right = L_obj.y;
    const float L_fwd   = L_obj.x;
    outVertFade = (1.0f - abs(L_fwd)) * 0.5f + 0.5f;

    float2 xz = float2(L_right, L_fwd);
    const float xzLenSq = dot(xz, xz);
    xz = (xzLenSq > 1e-8f) ? (xz * rsqrt(xzLenSq)) : float2(0.0f, 1.0f);
    outLxz = xz;

    const float4 sdfOrig = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, uv);
    const float4 sdfFlip = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, float2(1.0f - uv.x, uv.y));

    const float r7x = xz.y * 0.5f + 0.5f;
    const float r7y = xz.y * 0.5f + 1.5f;
    const float r7z = sdfFlip.r;
    const float r7w = sdfOrig.r;

    const bool  useLowerBand = ((1.0f - sdfOrig.a) > 0.5f);
    const float r8x = useLowerBand ? r7x : r7y;
    const float r8y = useLowerBand ? r7y : r7z;
    const float r8w = useLowerBand ? r7z : r7w;

    const float angleCoord = (xz.x < 0.0f) ? (2.0f - r8x) : r8x;
    const float s = max(smoothWidth, 1e-3f);

    float s0, f0, s1, f1;
    const float w0 = GFL2_WrapSdfBand(angleCoord - 0.5f * s, s0, f0);
    const float w1 = GFL2_WrapSdfBand(angleCoord + 0.5f * s, s1, f1);

    const float a0   = saturate((r8w - s0 * f0) / s);
    const float a1   = 1.0f - saturate(((2.0f - s0 * f0) - r8y) / s);
    const float band = (w0 < 1.0f) ? a0 : a1;
    const float b1   = 1.0f - saturate((s1 * f1 - r8w) / s);

    return (w1 < 1.0f) ? min(b1, band) : max(a1, band);
}

CharacterPSOutput MainPS(CharacterPSInput input)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint  debugView           = (uint)(g_ToggleFlags0.x + 0.5f);
    const bool  enableRamp          = (g_ToggleFlags0.y > 0.5f);
    const bool  enableSkinDCNorm    = (g_ToggleFlags0.z > 0.5f);
    const float iblIntensity        = max(g_ToggleFlags0.w, 0.0f);
    const bool  enableFaceSDF       = (g_PassParam0 > 0.5f);
    const float sdfSmooth           = (g_ToggleFlags2.x > 1e-4f) ? g_ToggleFlags2.x : max(mat.RawCb2[17].x, 0.01f);
    const bool  isBangsShadowRedraw = (g_PassSubMode == 1u);

    const float2 uv0    = input.UV01.xy;
    const float2 uv1    = input.UV01.zw;
    const bool   useUv1 = (mat.MaterialFlags & GFL2_MAT_FLAG_USE_SECOND_UV) != 0u;
    const float2 uvSdf  = useUv1 ? uv1 : uv0;

    const float2 uvBase  = GFL2_ApplyUVTransform(uv0, GFL2_BaseMapST(mat));
    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);

    float3 tint = float3(1.0f, 1.0f, 1.0f);
    const float4 rawTint = GFL2_BaseColorTint(mat);
    if (dot(rawTint.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
        tint = rawTint.rgb;
    const float3 baseColor = baseTex.rgb * tint;

    const float3 N_geom = normalize(input.WorldNormal);
    const float3 N_ws   = GFL2_ResolveWorldNormal(mat, uvBase, N_geom, input.WorldTangent);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);
    const float  shadowVis = isBangsShadowRedraw ? 0.0f : GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_geom);

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.498f);

    if (debugView == 1u) { outPS.LinearHDR = float4(baseColor, 1.0f); return outPS; }
    if (debugView == 2u) { outPS.LinearHDR = float4(N_ws * 0.5f + 0.5f, 1.0f); return outPS; }
    if (debugView == 3u) { outPS.LinearHDR = float4(0.0f, 0.0f, 0.0f, 1.0f); return outPS; }
    if (debugView == 4u) { outPS.LinearHDR = float4(0.65f, 0.65f, 0.65f, 1.0f); return outPS; }
    if (debugView == 5u) { outPS.LinearHDR = float4(1.0f, 1.0f, 1.0f, 1.0f); return outPS; }
    if (debugView == 6u) { outPS.LinearHDR = float4(0.0f, 0.0f, 0.0f, 1.0f); return outPS; }
    if (debugView == 7u)
    {
        const float nearZ = max(g_CameraPosAndNear.w, 0.01f);
        const float linZ  = nearZ / max(input.Position.z, 1e-5f);
        outPS.LinearHDR   = float4(saturate(1.0f - (linZ - 0.5f) * 0.25f).xxx, 1.0f);
        return outPS;
    }
    if (debugView == 8u)
    {
        const float4 sdfDbg = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, uvSdf);
        outPS.LinearHDR = float4(sdfDbg.rgb, 1.0f);
        return outPS;
    }
    if (debugView == 10u)
    {
        const float3 snTS = saturate(input.Color0.rgb) * 2.0f - 1.0f;
        const float3 snWS = GFL2_TransformNormalTStoWS(snTS, N_geom, input.WorldTangent);
        outPS.LinearHDR = float4(snWS * 0.5f + 0.5f, 1.0f);
        return outPS;
    }
    if (debugView == 11u) { outPS.LinearHDR = float4(saturate(input.Color0.a).xxx, 1.0f); return outPS; }
    if (debugView == 12u) { outPS.LinearHDR = float4(0.65f, 0.0f, 1.0f, 1.0f); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(mat.ShadingModelId), 1.0f); return outPS; }
    if (debugView == 14u)
    {
        const float2 uvChk = useUv1 ? uv1 : uv0;
        const float2 grid  = floor(uvChk * 24.0f);
        const float  chk   = abs(fmod(grid.x + grid.y, 2.0f));
        outPS.LinearHDR = float4(lerp(float3(0.05f, 0.25f, 0.35f), float3(0.15f, 0.85f, 0.65f), chk), 1.0f);
        return outPS;
    }
    if (debugView == 18u) { outPS.LinearHDR = float4(shadowVis.xxx, 1.0f); return outPS; }

    const bool hasSdf = enableFaceSDF && ((mat.MaterialFlags & GFL2_MAT_FLAG_HAS_SDF_MASK) != 0u) && (mat.RmoOrSdfIdx > 2u);
    float3 directDiff = float3(0.0f, 0.0f, 0.0f);
    float3 noseSpec   = float3(0.0f, 0.0f, 0.0f);

    const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
    const bool   useRamp  = enableRamp && ((mat.MaterialFlags & GFL2_MAT_FLAG_RAMP_ENABLE) != 0u) && (mat.RampMapIdx > 2u);

    if (isBangsShadowRedraw)
    {
        // EID 3295 (FS_7337): 刘海投影重绘强采面部 Ramp 最暗端 (0.000061, 0.125)
        const float3 shadowRamp = useRamp ? GFL2_SampleRampRow(mat.RampMapIdx, 0.000061f, 0.125f) : float3(0.0f, 0.0f, 0.0f);
        directDiff = baseColor * shadowRamp * lightCol;
    }
    else if (hasSdf)
    {
        float  vertFade = 1.0f;
        float2 L_xz     = float2(0.0f, 1.0f);
        const float faceSDF = GFL2_EvaluateFaceSDF(drawData, mat, uvSdf, L_ws, sdfSmooth, vertFade, L_xz);
        const float atten   = shadowVis * faceSDF * vertFade;
        const float3 diffRamp = useRamp ? GFL2_SampleRampRow(mat.RampMapIdx, max(atten, 0.000061f), 0.125f) : atten.xxx;
        directDiff = baseColor * diffRamp * lightCol;

        // 视角相关鼻尖/脸颊双阈值高光（FS_8185:236-274）
        float3 V_obj = mul((float3x3)drawData.WorldIT, V_ws);
        const float vLenSq = dot(V_obj, V_obj);
        V_obj = (vLenSq > 1e-8f) ? (V_obj * rsqrt(vLenSq)) : float3(1.0f, 0.0f, 0.0f);

        const float2 V_xz = float2(V_obj.y, V_obj.x);
        float2 H_xz = lerp(L_xz, V_xz, 0.85f);
        const float hLenSq = dot(H_xz, H_xz);
        H_xz = (hLenSq > 1e-8f) ? (H_xz * rsqrt(hLenSq)) : float2(0.0f, 1.0f);

        const float2 specUV = float2((H_xz.x < 0.0f) ? (1.0f - uvSdf.x) : uvSdf.x,
                                     uvSdf.y - V_ws.z * mat.RawCb2[16].w);
        const float4 specTexOrig = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, uvSdf);
        const float4 specTexDir  = GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampLinearClamp, specUV);

        const float t = clamp((-H_xz.y - 0.707107f) * 3.414213f, 0.01f, 0.99f);
        const float inBand = ((specTexDir.b >= t) && (specTexDir.g >= (1.0f - t))) ? 1.0f : 0.0f;
        const float areaMask = saturate(specTexOrig.g * 4.0f - 3.0f) * specTexOrig.a;
        const float specGain = inBand * areaMask * saturate(dot(N_ws, V_ws)) * mat.RawCb2[16].z * 0.318310f;
        const float specLight = specGain * (0.9f * shadowVis * saturate(dot(N_ws, L_ws)) + 0.1f);
        noseSpec = specLight.xxx * lightCol;
    }
    else
    {
        const float NdotL = saturate(dot(N_ws, L_ws));
        const float atten = shadowVis * NdotL;
        const float3 diffRamp = useRamp ? GFL2_SampleRampRow(mat.RampMapIdx, max(atten, 0.000061f), 0.125f) : atten.xxx;
        directDiff = baseColor * diffRamp * lightCol;
    }

    // 侧缘 Rim Light (EnvBRDFApprox + V=0.625) + 5682 IBL + L2 SH DC 归一化
    const float NdotV = saturate(dot(N_ws, V_ws));
    float envB = 0.018096f;
    if (useRamp && !isBangsShadowRedraw)
    {
        float3 camRight = float3(g_View._11, g_View._21, g_View._31);
        const float crLenSq = dot(camRight, camRight);
        camRight = (crLenSq > 1e-6f) ? (camRight * rsqrt(crLenSq)) : float3(0.0f, 1.0f, 0.0f);
        const float lViewX = dot(L_ws, camRight);
        const float sideSign = (lViewX > 1e-5f) ? 1.0f : ((lViewX < -1e-5f) ? -1.0f : 0.0f);
        const float uRim = 0.018096f * saturate(dot(N_ws, camRight * sideSign));
        envB = GFL2_SampleRampRow(mat.RampMapIdx, uRim, 0.625f).x;
    }
    const float3 probe      = GFL2_SampleReflectionProbe(mat.ReflectionProbeIdx, reflect(-V_ws, N_ws), 4.0f);
    const float3 iblSpec    = probe * (float3(0.04f, 0.04f, 0.04f) * 0.35f + envB.xxx) * max(iblIntensity, 0.0f);
    const float3 shDiffuse  = GFL2_ComputeCharacterSHDiffuse(drawData, mat, N_ws, enableSkinDCNorm);
    const float3 iblDiff    = baseColor * shDiffuse;

    if (debugView == 15u) { outPS.LinearHDR = float4(directDiff, 1.0f); return outPS; }
    if (debugView == 16u) { outPS.LinearHDR = float4(noseSpec, 1.0f); return outPS; }
    if (debugView == 17u) { outPS.LinearHDR = float4(iblDiff + iblSpec, 1.0f); return outPS; }

    const float4 rawPost  = GFL2_PostMultiplyTint(mat);
    const float3 postMult = (dot(rawPost.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f) ? rawPost.rgb : float3(1.0f, 1.0f, 1.0f);
    const float3 finalHDR = (directDiff + noseSpec + iblDiff + iblSpec) * postMult;

    outPS.LinearHDR = float4(finalHDR, 1.0f);
    return outPS;
}
