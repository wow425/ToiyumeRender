// ============================================================================
// 着色器：GFL2TAACS.hlsl（M6 UE5 Reverse-Z 时域抗锯齿 TAA Resolve）
// 对标：AK12-frame7206.rdc EID 4538 (3x3 Reverse-Z Max Closest Depth + 1.5σ Clamping)
// 编译配置：compile_shader_bindless (cs_6_6, MainCS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

cbuffer GFL2TAACB : register(b2)
{
    float4x4 g_PrevViewProj;
    float4   g_TaaParams; // x=historyValid, y=blendMin(0.04), z=blendMax(0.20), w=sharpness
};

float Luminance709(float3 c)
{
    return dot(max(c, 0.0f), float3(0.212673f, 0.715152f, 0.072175f));
}

float3 ReinhardCompress(float3 c)
{
    return c / (1.0f + Luminance709(c));
}

float3 ReinhardDecompress(float3 c)
{
    return c / max(1.0f - Luminance709(c), 1e-4f);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[g_AuxUavIdx];
    uint w = 1u, h = 1u;
    outTex.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h)
        return;

    Texture2D<float4> currTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    Texture2D<float4> histTex = ResourceDescriptorHeap[g_AuxTex1SrvIdx];
    Texture2D<float>  depthTex = ResourceDescriptorHeap[g_MainShadowMapSrvIdx];

    const int2   px      = int2(dtid.xy);
    const float2 invSize = 1.0f / float2(w, h);
    const float2 uv      = (float2(px) + 0.5f) * invSize;
    const float4 curr    = currTex.Load(int3(px, 0));

    if (g_TaaParams.x < 0.5f)
    {
        outTex[px] = curr;
        return;
    }

    // UE5 Reverse-Z: 前景最近像素拥有最大深度 (> 1e-5)
    float  closestDepth = 0.0f;
    int2   closestOffset = int2(0, 0);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 sp = clamp(px + int2(dx, dy), int2(0, 0), int2(w - 1u, h - 1u));
            const float d = depthTex.Load(int3(sp, 0));
            if (d > closestDepth)
            {
                closestDepth  = d;
                closestOffset = int2(dx, dy);
            }
        }
    }

    float2 prevUV = uv;
    if (closestDepth > 1e-5f)
    {
        const float2 cuv  = (float2(px + closestOffset) + 0.5f) * invSize;
        const float4 clip = float4(cuv.x * 2.0f - 1.0f, 1.0f - cuv.y * 2.0f, closestDepth, 1.0f);
        const float4 ws   = mul(clip, g_InvViewProj);
        const float4 pClip = mul(ws, g_PrevViewProj);
        if (abs(pClip.w) > 1e-5f)
        {
            const float2 pNdc = pClip.xy / pClip.w;
            prevUV = float2(pNdc.x * 0.5f + 0.5f, -pNdc.y * 0.5f + 0.5f);
        }
    }

    if (prevUV.x <= 0.0f || prevUV.x >= 1.0f || prevUV.y <= 0.0f || prevUV.y >= 1.0f)
    {
        outTex[px] = curr;
        return;
    }

    // 5-Tap 十字邻域 Karis 亮度空间方差裁剪 (μ ± 1.5σ)
    const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    float3 m1 = float3(0.0f, 0.0f, 0.0f);
    float3 m2 = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        const int2 sp = clamp(px + offsets[i], int2(0, 0), int2(w - 1u, h - 1u));
        const float3 c = ReinhardCompress(currTex.Load(int3(sp, 0)).rgb);
        m1 += c;
        m2 += c * c;
    }
    const float3 mean   = m1 * 0.2f;
    const float3 sigma  = sqrt(max(m2 * 0.2f - mean * mean, 0.0f));
    const float3 boxMin = mean - sigma * 1.5f;
    const float3 boxMax = mean + sigma * 1.5f;

    float3 hist = histTex.SampleLevel(g_SampLinearClamp, prevUV, 0.0f).rgb;
    if (any(isnan(hist)) || any(isinf(hist)))
        hist = curr.rgb;
    const float3 histComp    = clamp(ReinhardCompress(hist), boxMin, boxMax);
    const float3 histClipped = ReinhardDecompress(histComp);

    const float velPx = length((prevUV - uv) * float2(w, h));
    const float alpha = lerp(g_TaaParams.y, g_TaaParams.z, saturate(velPx * 0.05f));
    float3 resolved = lerp(histClipped, curr.rgb, alpha);
    if (any(isnan(resolved)) || any(isinf(resolved)))
        resolved = curr.rgb;

    outTex[px] = float4(resolved, curr.a);
}
