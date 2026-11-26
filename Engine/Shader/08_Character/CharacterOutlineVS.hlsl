// ============================================================================
// 着色器：CharacterOutlineVS.hlsl（M5 背面平滑法线挤出 + 1080p 像素钳位 + Reverse-Z 退让）
// 对标：AK12-frame7206.rdc VS_7343_Outline_BackfaceExtrude_VS
// 编译配置：compile_shader_bindless (vs_6_6, MainVS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

struct OutlinePSInput
{
    float4 Position     : SV_POSITION;
    float2 UV0          : TEXCOORD0;
    float4 OutlineColor : TEXCOORD1;
    float  OutlineWeight: TEXCOORD2;
};

OutlinePSInput MainVS(CharacterVSInput input)
{
    const GFL2DrawData     drawData = GFL2_LoadDrawData(g_DrawIndex);
    const GFL2MaterialData mat      = GFL2_LoadMaterialData(g_MaterialIndex);

    const float4 posWS = mul(float4(input.Position, 1.0f), drawData.WorldMatrix);
    const float3 nrmWS = normalize(mul(input.Normal, (float3x3)drawData.WorldIT));
    const float3 tanWS = normalize(mul(input.Tangent.xyz, (float3x3)drawData.WorldMatrix));
    const float  worldDetSign = (determinant((float3x3)drawData.WorldMatrix) < 0.0f) ? -1.0f : 1.0f;
    const float4 T_ws = float4(tanWS, input.Tangent.w * worldDetSign);

    const float3 snTS = saturate(input.Color0.rgb) * 2.0f - 1.0f;
    const float3 snWS = GFL2_TransformNormalTStoWS(snTS, nrmWS, T_ws);

    const float4 posVS  = mul(posWS, g_View);
    const float3 snVS   = mul(snWS, (float3x3)g_View);
    float4       clipP  = mul(posWS, g_ViewProj);
    const float4 stepVS = float4(posVS.xyz + snVS * 0.000521f, 1.0f);
    const float4 stepC  = mul(stepVS, g_Proj);

    const float  matWidth = max(mat.RawCb2[15].x, 0.0f);
    const float  globScale = max(g_ToggleFlags1.x, 0.0f);
    const float2 deltaClip = abs(stepC.xy - clipP.xy) * (matWidth * 1.3f * globScale);

    const float  resScale = max(min(g_ScreenSizeAndInv.x / 1920.0f, g_ScreenSizeAndInv.y / 1080.0f), 0.5f);
    const float2 pxToClip = (2.0f * g_ScreenSizeAndInv.zw) * max(clipP.w, 1e-4f) * resScale;
    const float2 minClip  = pxToClip * max(g_ToggleFlags1.z, 0.0f);
    const float2 maxClip  = pxToClip * max(g_ToggleFlags1.w, g_ToggleFlags1.z);

    float2 nClipXY = mul(float4(snWS, 0.0f), g_ViewProj).xy;
    if (dot(nClipXY, nClipXY) < 1e-8f)
        nClipXY = float2(1.0f, 0.0f);
    const float2 signXY = float2((nClipXY.x >= 0.0f) ? 1.0f : -1.0f, (nClipXY.y >= 0.0f) ? 1.0f : -1.0f);
    const float2 offset = signXY * clamp(deltaClip, minClip, maxClip) * saturate(input.Color0.a);

    if (globScale > 1e-4f && matWidth > 1e-4f)
        clipP.xy += offset;

    // UE5 Reverse-Z 深度退让：减去正数向远平面 (0.0) 推后，消除侧面胫骨缝处 1px Z-Fighting
    clipP.z -= (4.5f + mat.RawCb2[15].y + g_ToggleFlags1.y) * 0.001f;

    const float3 L_ws = normalize(g_MainLightDirToLightAndInt.xyz);
    const float2 pXY  = (dot(posWS.xy, posWS.xy) > 1e-8f) ? normalize(posWS.xy) : float2(1.0f, 0.0f);
    const float2 lXY  = (dot(L_ws.xy, L_ws.xy) > 1e-8f) ? normalize(L_ws.xy) : float2(1.0f, 0.0f);
    const float  litT = dot(pXY, lXY) * 0.5f + 0.5f;
    const float  colMult = (mat.RawCb2[15].z > 1e-4f) ? mat.RawCb2[15].z : 1.0f;

    OutlinePSInput output;
    output.Position      = clipP;
    output.UV0           = input.UV0;
    output.OutlineColor  = lerp(GFL2_OutlineShadowCol(mat), GFL2_OutlineLitColor(mat), litT) * colMult;
    output.OutlineWeight = saturate(input.Color0.a);
    return output;
}
