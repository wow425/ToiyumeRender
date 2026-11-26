// ============================================================================
// 着色器：CharacterOpaqueVS.hlsl（M1 UE5 LH Z-Up + 行向量左乘主顶点着色器）
// 编译配置：compile_shader_bindless (vs_6_6, MainVS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

CharacterPSInput MainVS(CharacterVSInput input)
{
    const GFL2DrawData drawData = GFL2_LoadDrawData(g_DrawIndex);

    const float4 posWS = mul(float4(input.Position, 1.0f), drawData.WorldMatrix);
    const float3 nrmWS = normalize(mul(input.Normal, (float3x3)drawData.WorldIT));
    const float3 tanWS = normalize(mul(input.Tangent.xyz, (float3x3)drawData.WorldMatrix));
    const float  worldDetSign = (determinant((float3x3)drawData.WorldMatrix) < 0.0f) ? -1.0f : 1.0f;

    CharacterPSInput output;
    output.Position     = mul(posWS, g_ViewProj);
    output.WorldPos     = posWS.xyz;
    output.WorldNormal  = nrmWS;
    output.WorldTangent = float4(tanWS, input.Tangent.w * worldDetSign);
    output.UV01         = float4(input.UV0, input.UV1);
    output.Color0       = input.Color0;
    return output;
}
