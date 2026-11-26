// ============================================================================
// 着色器：GFL2UberPostLUTPS.hlsl（M6 Bloom 叠加 + Vignette + LinearToLogC + 7281 32³ 3D LUT）
// 对标：AK12-frame7206.rdc EID 4739 FS_7451_UberPost_LUT_Vignette_FS
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

float3 Sample3DLut7281(uint lutSrvIdx, float3 linearCol)
{
    Texture2D<float4> lutTex = ResourceDescriptorHeap[lutSrvIdx];
    // Unity URP LinearToLogC (FS_7451:140-142)
    const float3 logC = saturate(log2(max(linearCol, 0.0f) * 5.555556f + 0.047996f) * 0.073500f + 0.386036f);

    const float3 scaled = logC * 31.0f;
    const float  slice0 = floor(scaled.b);
    const float  fracB  = scaled.b - slice0;

    // 1024x32 Strip: 32 slices along U, R along U-in-slice, G along V
    const float uInSlice = scaled.r * (1.0f / 1024.0f) + 0.5f / 1024.0f;
    const float vCoord   = scaled.g * (1.0f / 32.0f) + 0.5f / 32.0f;
    const float u0       = slice0 * (1.0f / 32.0f) + uInSlice;
    const float u1       = min(slice0 + 1.0f, 31.0f) * (1.0f / 32.0f) + uInSlice;

    const float3 s0 = lutTex.SampleLevel(g_SampLinearClamp, float2(u0, vCoord), 0.0f).rgb;
    const float3 s1 = lutTex.SampleLevel(g_SampLinearClamp, float2(u1, vCoord), 0.0f).rgb;
    return lerp(s0, s1, fracB);
}

float4 MainPS(VSOutput input) : SV_TARGET0
{
    const float2 uv = input.PositionH.xy * g_ScreenSizeAndInv.zw;
    Texture2D<float4> hdrTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    float3 col = hdrTex.Load(int3(input.PositionH.xy, 0)).rgb;

    // 1. Bloom 加法 (g_AuxTex1SrvIdx != INVALID, g_PassParam0 = bloomIntensity)
    if (g_AuxTex1SrvIdx != GFL2_INVALID_BINDLESS && g_PassParam0 > 1e-4f)
    {
        Texture2D<float4> bloomTex = ResourceDescriptorHeap[g_AuxTex1SrvIdx];
        col += bloomTex.SampleLevel(g_SampLinearClamp, uv, 0.0f).rgb * g_PassParam0;
    }

    // 2. Vignette 暗角 (EID 4739: color=[0.2,0.2,0.2], center=(0.5,0.5), intensity=g_PassParam1, smoothness=0.6)
    const float vigInt = max(g_PassParam1, 0.0f);
    if (vigInt > 1e-4f)
    {
        const float2 dist = abs(uv - 0.5f) * vigInt;
        const float  vfac = pow(saturate(1.0f - dot(dist, dist)), 0.6f);
        col *= lerp(float3(0.2f, 0.2f, 0.2f), float3(1.0f, 1.0f, 1.0f), vfac);
    }

    // 3. 7281 32³ 3D LUT 色彩分级与 ACEScct 色调映射 (g_MainShadowMapSrvIdx = LUT SRV, g_PassParam2 = lutIntensity)
    const float lutInt = clamp(g_PassParam2, 0.0f, 1.0f);
    if (g_MainShadowMapSrvIdx != GFL2_INVALID_BINDLESS && lutInt > 1e-4f)
    {
        const float3 graded = Sample3DLut7281(g_MainShadowMapSrvIdx, col);
        col = lerp(col, graded, lutInt);
    }

    return float4(max(col, 0.0f), 1.0f);
}
