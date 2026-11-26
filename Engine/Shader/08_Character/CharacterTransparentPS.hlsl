// ============================================================================
// 着色器：CharacterTransparentPS.hlsl（M5 5组双面半透明件 · 高光不衰减）
// 对标：AK12-frame7206.rdc FS_8202 (CullFront) & FS_8204 (CullBack)
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

    const float2 uv0     = input.UV01.xy;
    const float2 uvBase  = GFL2_ApplyUVTransform(uv0, GFL2_BaseMapST(mat));
    const float2 uvRmo   = GFL2_ApplyUVTransform(uv0, GFL2_RmoMapST(mat));
    const float2 uvNorm  = GFL2_ApplyUVTransform(uv0, GFL2_NormalMapST(mat));

    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);
    float3 tint = float3(1.0f, 1.0f, 1.0f);
    const float4 rawTint = GFL2_BaseColorTint(mat);
    if (dot(rawTint.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f)
        tint = rawTint.rgb;

    const float  tintAlpha = (rawTint.a > 1e-4f) ? rawTint.a : 1.0f;
    const float  alphaMask = (mat.RawCb2[0].x > 1e-4f) ? mat.RawCb2[0].x : 1.0f;
    const float  outAlpha  = saturate(lerp(1.0f, baseTex.a * tintAlpha, alphaMask));

    const float3 N_geom = normalize(isFrontFace ? input.WorldNormal : -input.WorldNormal);
    const float3 N_ws   = GFL2_ResolveWorldNormal(mat, uvNorm, N_geom, input.WorldTangent);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);

    const bool hasRmo = (mat.RmoOrSdfIdx > 2u);
    const float4 rmo  = hasRmo ? GFL2_SampleBindless2D(mat.RmoOrSdfIdx, g_SampAnisoWrap, uvRmo)
                               : float4(0.18f, 0.0f, 1.0f, 0.0f);

    const float shadowVis = GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_geom);

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(N_ws * 0.5f + 0.5f, 0.498f);

    if (debugView == 1u)  { outPS.LinearHDR = float4(baseTex.rgb * tint, outAlpha); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(GFL2_SHADING_TRANSPARENT), 1.0f); return outPS; }
    if (debugView != 0u && debugView != 9u)
    {
        outPS.LinearHDR = float4(baseTex.rgb * tint, outAlpha);
        return outPS;
    }

    GFL2StylizedSurface surf;
    surf.BaseColor    = baseTex.rgb * tint;
    surf.N_ws         = N_ws;
    surf.V_ws         = V_ws;
    surf.L_ws         = L_ws;
    surf.Roughness    = rmo.r;
    surf.Metallic     = rmo.g;
    surf.AO           = rmo.b;
    surf.EmissiveMask = hasRmo ? rmo.a : 0.0f;
    surf.ShadowVis    = shadowVis;

    const GFL2StylizedLightingResult lit = GFL2_EvaluateBodyStylizedBRDF(
        drawData, mat, surf, enableRamp, enableSkinDCNorm, iblIntensity);

    // 关键风格化规则（FS_8202:257,260）：仅漫反射随 Alpha 衰减，镜面高光与 IBL 反射保持全强度
    const float4 rawPost  = GFL2_PostMultiplyTint(mat);
    const float3 postMult = (dot(rawPost.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.001f) ? rawPost.rgb : float3(1.0f, 1.0f, 1.0f);
    const float3 diffPart = (lit.DirectDiffuse + lit.IblDiffuse) * outAlpha;
    const float3 specPart = (lit.DirectSpecular + lit.IblSpecular + lit.Emissive);
    const float3 finalRGB = (diffPart + specPart) * postMult;

    outPS.LinearHDR = float4(finalRGB, outAlpha);
    return outPS;
}
