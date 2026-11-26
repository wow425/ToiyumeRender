// ============================================================================
// 着色器：CharacterOutlinePS.hlsl（M5 背面描边着色 + AuxMask.a = 0.499 SSAO 豁免）
// 对标：AK12-frame7206.rdc FS_7344_Outline_BackfaceExtrude_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/GFL2StylizedBRDF.hlsli"

struct OutlinePSInput
{
    float4 Position     : SV_POSITION;
    float2 UV0          : TEXCOORD0;
    float4 OutlineColor : TEXCOORD1;
    float  OutlineWeight: TEXCOORD2;
};

CharacterPSOutput MainPS(OutlinePSInput input)
{
    const GFL2MaterialData mat = GFL2_LoadMaterialData(g_MaterialIndex);
    const uint debugView = (uint)(g_ToggleFlags0.x + 0.5f);

    const float2 uvBase  = GFL2_ApplyUVTransform(input.UV0, GFL2_BaseMapST(mat));
    const float4 baseTex = GFL2_SampleBindless2D(mat.BaseColorIdx, g_SampAnisoWrap, uvBase);

    CharacterPSOutput outPS;
    outPS.AuxMask = float4(0.5f, 0.5f, 1.0f, 0.499f);

    if (debugView == 11u) { outPS.LinearHDR = float4(input.OutlineWeight.xxx, 1.0f); return outPS; }
    if (debugView == 13u) { outPS.LinearHDR = float4(GFL2_ShadingModelFalseColor(GFL2_SHADING_OUTLINE), 1.0f); return outPS; }
    if (debugView != 0u && debugView != 9u)
    {
        outPS.LinearHDR = float4(input.OutlineColor.rgb, 1.0f);
        return outPS;
    }

    const float3 lightCol = g_MainLightColorAndShadow.rgb * max(g_MainLightDirToLightAndInt.w, 0.0f);
    const float3 tintCol  = max(input.OutlineColor.rgb, float3(0.015f, 0.015f, 0.015f));
    const float3 outRGB   = tintCol * lerp(float3(1.0f, 1.0f, 1.0f), baseTex.rgb, 0.35f) * lightCol;

    outPS.LinearHDR = float4(outRGB, 1.0f);
    return outPS;
}
