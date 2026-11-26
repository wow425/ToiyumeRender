// ============================================================================
// 着色器：CharacterBangsShadowVS.hlsl（M4 EID 3198 前刘海投影偏移写模板 200->201）
// 对标：AK12-frame7206.rdc VS_7327_HairBangs_ShadowStencil_VS
//   Unity 偏移 (-0.0045 * L_unity.x, -0.0075, 0)
//   UE5 循环置换 (0.0, -0.0045 * L_ue5.y, -0.0075)
// 编译配置：compile_shader_bindless (vs_6_6, MainVS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

float4 MainVS(CharacterVSInput input) : SV_POSITION
{
    const GFL2DrawData drawData = GFL2_LoadDrawData(g_DrawIndex);
    float4 posWS = mul(float4(input.Position, 1.0f), drawData.WorldMatrix);

    const float3 L_ws = normalize(g_MainLightDirToLightAndInt.xyz);
    posWS.y += -0.0045f * L_ws.y;
    posWS.z += -0.0075f;

    return mul(posWS, g_ViewProj);
}
