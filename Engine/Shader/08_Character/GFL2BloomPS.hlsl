// ============================================================================
// 着色器：GFL2BloomPS.hlsl（M6 8-Pass Karis 软阈值 Prefilter / Downsample / Upsample）
// 对标：AK12-frame7206.rdc EID 4601..4713
//   g_PassSubMode == 0 : Karis Prefilter (AuxTex0 = LinearHDR, Param0 = threshold, Param1 = knee, Param2 = clampMax)
//   g_PassSubMode == 1 : 13-Tap Downsample (AuxTex0 = srcMip, Param0/1 = invSrcW/H)
//   g_PassSubMode == 2 : Tent Upsample (AuxTex0 = srcMip, Param0/1 = invSrcW/H, Param2 = scatterWeight)
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

float3 KarisPrefilter(float3 c, float threshold, float knee, float clampMax)
{
    c = min(c, clampMax.xxx);
    const float br = max(c.r, max(c.g, c.b));
    float soft = br - threshold + knee;
    soft = clamp(soft, 0.0f, 2.0f * knee);
    soft = (soft * soft) / max(4.0f * knee, 1e-5f);
    const float contrib = max(soft, br - threshold) / max(br, 1e-5f);
    return max(c * saturate(contrib), 0.0f);
}

float4 MainPS(VSOutput input) : SV_TARGET0
{
    Texture2D<float4> srcTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    uint sw = 1u, sh = 1u;
    srcTex.GetDimensions(sw, sh);
    const float2 invSrc = 1.0f / float2(max(sw, 1u), max(sh, 1u));

    // 目标尺寸反算 UV (由 g_PassParam2/3 或直接用 SV_POSITION / destDims; 注意 Prefilter 时 dest=half, 传 invDest 在 Param2/3)
    const float2 uv = input.PositionH.xy * float2(g_PassParam2, g_PassParam3);

    if (g_PassSubMode == 0u) // Prefilter
    {
        const float threshold = g_PassParam0; // 0.9
        const float knee      = 0.2f;
        const float clampMax  = g_PassParam1; // 1.8

        const float3 c0 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-0.5f, -0.5f) * invSrc, 0.0f).rgb;
        const float3 c1 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.5f, -0.5f) * invSrc, 0.0f).rgb;
        const float3 c2 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-0.5f,  0.5f) * invSrc, 0.0f).rgb;
        const float3 c3 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.5f,  0.5f) * invSrc, 0.0f).rgb;
        const float3 avg = (c0 + c1 + c2 + c3) * 0.25f;
        return float4(KarisPrefilter(avg, threshold, knee, clampMax), 1.0f);
    }

    if (g_PassSubMode == 1u) // 13-Tap Downsample
    {
        const float3 a = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-2.0f, -2.0f) * invSrc, 0.0f).rgb;
        const float3 b = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.0f, -2.0f) * invSrc, 0.0f).rgb;
        const float3 c = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 2.0f, -2.0f) * invSrc, 0.0f).rgb;
        const float3 d = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-1.0f, -1.0f) * invSrc, 0.0f).rgb;
        const float3 e = srcTex.SampleLevel(g_SampLinearClamp, uv,                                 0.0f).rgb;
        const float3 f = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 1.0f, -1.0f) * invSrc, 0.0f).rgb;
        const float3 g = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-2.0f,  0.0f) * invSrc, 0.0f).rgb;
        const float3 h = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 2.0f,  0.0f) * invSrc, 0.0f).rgb;
        const float3 i = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-1.0f,  1.0f) * invSrc, 0.0f).rgb;
        const float3 j = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 1.0f,  1.0f) * invSrc, 0.0f).rgb;
        const float3 k = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-2.0f,  2.0f) * invSrc, 0.0f).rgb;
        const float3 l = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.0f,  2.0f) * invSrc, 0.0f).rgb;
        const float3 m = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 2.0f,  2.0f) * invSrc, 0.0f).rgb;

        float3 res = e * 0.125f;
        res += (d + f + i + j) * 0.125f;
        res += (a + b + g + e) * 0.03125f;
        res += (b + c + e + h) * 0.03125f;
        res += (g + e + k + l) * 0.03125f;
        res += (e + h + l + m) * 0.03125f;
        return float4(max(res, 0.0f), 1.0f);
    }

    // 9-Tap Tent Upsample (Additive blend onto dest mip)
    const float3 t0 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-1.0f, -1.0f) * invSrc, 0.0f).rgb;
    const float3 t1 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.0f, -1.0f) * invSrc, 0.0f).rgb;
    const float3 t2 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 1.0f, -1.0f) * invSrc, 0.0f).rgb;
    const float3 t3 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-1.0f,  0.0f) * invSrc, 0.0f).rgb;
    const float3 t4 = srcTex.SampleLevel(g_SampLinearClamp, uv,                                 0.0f).rgb;
    const float3 t5 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 1.0f,  0.0f) * invSrc, 0.0f).rgb;
    const float3 t6 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2(-1.0f,  1.0f) * invSrc, 0.0f).rgb;
    const float3 t7 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 0.0f,  1.0f) * invSrc, 0.0f).rgb;
    const float3 t8 = srcTex.SampleLevel(g_SampLinearClamp, uv + float2( 1.0f,  1.0f) * invSrc, 0.0f).rgb;

    const float3 up = ((t0 + t2 + t6 + t8) + 2.0f * (t1 + t3 + t5 + t7) + 4.0f * t4) * (1.0f / 16.0f);
    return float4(up * g_PassParam0, 1.0f);
}
