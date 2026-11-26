// ============================================================================
// 着色器：CharacterEyePS.hlsl（M4 眼球三层：3137 视差虹膜 / 3229 加法高光 / 3252 眼窝乘法）
// 对标：AK12-frame7206.rdc FS_8189 / FS_8191 / FS_7335
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

CharacterPSOutput MainPS(CharacterPSInput input)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint debugView            = (uint)(g_ToggleFlags0.x + 0.5f);
    const bool enableSkinDCNorm     = (g_ToggleFlags0.z > 0.5f);

    const float2 uv0    = input.UV01.xy;
    const float3 N_ws   = normalize(input.WorldNormal);
    const float3 V_ws   = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float3 L_ws   = normalize(g_MainLightDirToLightAndInt.xyz);

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(0.0f, 0.95f, 0.999f, 0.999f);

    // 切线空间视线视差 UV 偏移（对应 VS_8188 + FS_8189 cb2[4].yz）
    const float3 T_ws = normalize(input.WorldTangent.xyz - N_ws * dot(input.WorldTangent.xyz, N_ws));
    const float3 B_ws = cross(N_ws, T_ws) * input.WorldTangent.w;
    const float2 V_ts = float2(dot(V_ws, T_ws), dot(V_ws, B_ws));
    const float  pMask = saturate(input.Color0.g);

    if (g_PassSubMode == 1u) // EID 3229 EyeHighlightAdd (Blend One, One)
    {
        if (debugView != 0u)
        {
            outPS.LinearHDR = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return outPS;
        }
        const float2 uvBlend  = uv0 + V_ts * mat.RawCb2[4].yz * pMask;
        const float4 blendTex = GFL2_SampleBindless2D(mat.EyeBlendIdx, g_SampAnisoWrap, uvBlend);
        const float  shadow   = GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_ws);
        const float  atten    = saturate(dot(N_ws, L_ws)) * shadow;
        const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
        const float3 addCol   = blendTex.rgb * blendTex.a * (0.8f * atten + 0.2f) * lightCol;
        outPS.LinearHDR = float4(addCol, 1.0f);
        return outPS;
    }

    if (g_PassSubMode == 2u) // EID 3252 EyeSocketMul (Blend DstColor, Zero)
    {
        if (debugView != 0u)
        {
            outPS.LinearHDR = float4(1.0f, 1.0f, 1.0f, 1.0f);
            return outPS;
        }
        const float4 mulTex = GFL2_SampleBindless2D(mat.EyeBlendIdx, g_SampAnisoWrap, uv0);
        outPS.LinearHDR = float4(mulTex.rgb, 1.0f);
        return outPS;
    }

    // Layer 1: EID 3137 EyeBase
    const float2 uvBase  = uv0 + V_ts * mat.RawCb2[4].yz * pMask;
    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);
    const float  shadow  = GFL2_SampleMainShadow9TapPCF(input.WorldPos, N_ws);

    if (debugView == 1u)  { outPS.LinearHDR = float4(baseTex.rgb, 1.0f); return outPS; }
    if (debugView == 2u)  { outPS.LinearHDR = float4(N_ws * 0.5f + 0.5f, 1.0f); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(mat.ShadingModelId), 1.0f); return outPS; }
    if (debugView == 14u)
    {
        const float2 grid = floor(uv0 * 24.0f);
        const float  chk  = abs(fmod(grid.x + grid.y, 2.0f));
        outPS.LinearHDR = float4(lerp(float3(0.18f, 0.22f, 0.28f), float3(0.88f, 0.86f, 0.82f), chk), 1.0f);
        return outPS;
    }
    if (debugView == 18u) { outPS.LinearHDR = float4(shadow.xxx, 1.0f); return outPS; }
    if (debugView != 0u && debugView != 9u)
    {
        outPS.LinearHDR = float4(baseTex.rgb, 1.0f);
        return outPS;
    }

    // 眼球保底亮底：lerp(0.25, 1.0, shadow * NdotL) + 归一化 L2 SH（FS_8189:90-148）
    const float  NdotL    = saturate(dot(N_ws, L_ws));
    const float  minLit   = (mat.RawCb2[8].y > 0.001f) ? mat.RawCb2[8].y : 0.25f;
    const float  litFac   = lerp(minLit, 1.0f, shadow * NdotL);
    const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
    const float3 shDiff   = GFL2_ComputeCharacterSHDiffuse(drawData, mat, N_ws, enableSkinDCNorm);

    const float3 finalHDR = baseTex.rgb * (lightCol * litFac + shDiff);
    outPS.LinearHDR = float4(finalHDR, 1.0f);
    return outPS;
}
