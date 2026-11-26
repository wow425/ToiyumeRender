// ============================================================================
// 着色器：GFL2SSAOCompositePS.hlsl（M6 双边滤波 + SSAO 乘法合成，角色严格豁免）
// 对标：AK12-frame7206.rdc EID 3657/3669/3690 (Blend DstColor, Zero)
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

float4 MainPS(VSOutput input) : SV_TARGET0
{
    const float2 uv = input.PositionH.xy * g_ScreenSizeAndInv.zw;
    Texture2D<float>  ssaoTex  = ResourceDescriptorHeap[g_MainShadowMapSrvIdx];
    Texture2D<float>  depthTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    Texture2D<float4> auxTex   = ResourceDescriptorHeap[g_AuxTex1SrvIdx];

    const float  centerDepth = depthTex.SampleLevel(g_SampPointClamp, uv, 0.0f);
    const float4 aux         = auxTex.SampleLevel(g_SampPointClamp, uv, 0.0f);
    const bool bypassExempt  = (g_PassSubMode != 0u);

    if (centerDepth <= 1e-5f || (!bypassExempt && aux.a >= 0.495f))
        return float4(1.0f, 1.0f, 1.0f, 1.0f);

    uint ssaoW = 1u, ssaoH = 1u;
    ssaoTex.GetDimensions(ssaoW, ssaoH);
    const float2 texel = 1.0f / float2(max(ssaoW, 1u), max(ssaoH, 1u));

    float sumAO = 0.0f;
    float sumW  = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const float2 sUV = uv + float2((float)dx, (float)dy) * texel;
            const float  sAO = ssaoTex.SampleLevel(g_SampLinearClamp, sUV, 0.0f);
            const float  sD  = depthTex.SampleLevel(g_SampPointClamp, sUV, 0.0f);
            const float  w   = exp(-abs(sD - centerDepth) * 400.0f);
            sumAO += sAO * w;
            sumW  += w;
        }
    }

    const float blurredAO = (sumW > 1e-5f) ? (sumAO / sumW) : 1.0f;
    const float intensity = clamp(g_PassParam0, 0.0f, 2.0f);
    const float finalAO   = saturate(lerp(1.0f, blurredAO, intensity));
    return float4(finalAO, finalAO, finalAO, 1.0f);
}
