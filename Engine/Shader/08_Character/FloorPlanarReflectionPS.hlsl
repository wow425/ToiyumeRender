// ============================================================================
// 着色器：FloorPlanarReflectionPS.hlsl（M6 地板平面反射 25-Tap 高斯核合成）
// 对标：AK12-frame7206.rdc EID 3843 FS_7851_FloorPlanarReflection_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

float4 MainPS(CharacterPSInput input) : SV_TARGET0
{
    const uint debugView = (uint)(g_ToggleFlags0.x + 0.5f);
    if (debugView != 0u || g_AuxTex0SrvIdx == GFL2_INVALID_BINDLESS)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float2 uv = saturate(input.Position.xy * g_ScreenSizeAndInv.zw);
    const float2 texel = g_ScreenSizeAndInv.zw * 2.0f;

    Texture2D<float4> reflTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];

    // 9-Tap 柔和地板模糊采样（模拟高光地板拉丝漫散倒影）
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    const float weights[3] = { 0.25f, 0.50f, 0.25f };
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const float w = weights[dx + 1] * weights[dy + 1];
            sum += reflTex.SampleLevel(g_SampLinearClamp, uv + float2((float)dx, (float)dy) * texel, 1.0f).rgb * w;
        }
    }

    const float3 V_ws  = normalize(g_CameraPosAndNear.xyz - input.WorldPos);
    const float  NdotV = saturate(abs(V_ws.z));
    const float  fresnel = lerp(0.45f, 0.16f, NdotV);
    const float  reflStrength = clamp(g_PassParam0, 0.0f, 1.0f) * fresnel;

    return float4(sum, reflStrength);
}
