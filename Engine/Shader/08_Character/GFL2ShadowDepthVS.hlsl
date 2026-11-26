// ============================================================================
// 着色器：GFL2ShadowDepthVS.hlsl（M2 UE5 Reverse-Z 4096x4096 主方向光阴影深度 VS）
// 编译配置：compile_shader_bindless (vs_6_6, MainVS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

float4 MainVS(CharacterVSInput input) : SV_POSITION
{
    const GFL2DrawData drawData = GFL2_LoadDrawData(g_DrawIndex);
    const float4 posWS = mul(float4(input.Position, 1.0f), drawData.WorldMatrix);
    return mul(posWS, g_MainLightViewProj);
}
