// ============================================================================
// 着色器：GFL2TAACS.hlsl（M6 UE5 Reverse-Z 时域抗锯齿 TAA Resolve）
// 修复要点：
//   1) 配合管线端 16 相 Halton(2,3) LH 亚像素投影抖动，提供真实亚像素超采样抗锯齿；
//   2) 3x3 ClosestDepth 膨胀仅用于算运动矢量 velocityUV = curUV - prevUV，
//      再用 centerUV - velocityUV 采样历史，修复旧版直接采邻像素 prevUV 导致的边缘 1px 涂抹；
//   3) 5-Tap Catmull-Rom 历史重采样 + 3x3 Karis 亮度压缩 YCoCg 方差裁剪，消除双线性发糊与鬼影。
// 编译配置：compile_shader_bindless (cs_6_6, MainCS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

cbuffer GFL2TAACB : register(b2)
{
    float4x4 g_CurUnjitteredViewProj;
    float4x4 g_PrevUnjitteredViewProj;
    float4   g_TaaParams; // x=historyValid, y=blendMin(0.08), z=blendMax(0.35), w=varianceGamma(1.15)
};

float Luminance709(float3 c)
{
    return dot(max(c, 0.0f), float3(0.212673f, 0.715152f, 0.072175f));
}

float3 ReinhardCompress(float3 c)
{
    c = max(c, 0.0f);
    return c / (1.0f + Luminance709(c));
}

float3 ReinhardDecompress(float3 c)
{
    return max(c, 0.0f) / max(1.0f - Luminance709(c), 1e-4f);
}

float3 RGBToYCoCg(float3 rgb)
{
    const float Y  =  0.25f * rgb.r + 0.50f * rgb.g + 0.25f * rgb.b;
    const float Co =  0.50f * rgb.r                 - 0.50f * rgb.b;
    const float Cg = -0.25f * rgb.r + 0.50f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    const float t = ycocg.x - ycocg.z;
    const float r = t + ycocg.y;
    const float g = ycocg.x + ycocg.z;
    const float b = t - ycocg.y;
    return max(float3(r, g, b), 0.0f);
}

// 5-Tap Catmull-Rom 双三次近似采样：抑制双线性历史滤波造成的时域积累模糊
float3 SampleHistoryCatmullRom(Texture2D<float4> historyTex, float2 uv, float2 screenSize, float2 invSize)
{
    const float2 samplePos = uv * screenSize;
    const float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    const float2 f         = samplePos - texPos1;

    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);

    const float2 w12      = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-5f);

    const float2 texPos0  = (texPos1 - 1.0f) * invSize;
    const float2 texPos3  = (texPos1 + 2.0f) * invSize;
    const float2 texPos12 = (texPos1 + offset12) * invSize;

    float3 result = float3(0.0f, 0.0f, 0.0f);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos0.y),  0).rgb * (w12.x * w0.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos0.x,  texPos12.y), 0).rgb * (w0.x  * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos12.y), 0).rgb * (w12.x * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos3.x,  texPos12.y), 0).rgb * (w3.x  * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos3.y),  0).rgb * (w12.x * w3.y);

    const float totalWeight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / max(totalWeight, 1e-5f), 0.0f);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[g_AuxUavIdx];
    uint w = 1u, h = 1u;
    outTex.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h)
        return;

    Texture2D<float4> currTex  = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    Texture2D<float4> histTex  = ResourceDescriptorHeap[g_AuxTex1SrvIdx];
    Texture2D<float>  depthTex = ResourceDescriptorHeap[g_MainShadowMapSrvIdx];

    const int2   px         = int2(dtid.xy);
    const float2 screenSize = float2(w, h);
    const float2 invSize    = 1.0f / screenSize;
    const float2 centerUV   = (float2(px) + 0.5f) * invSize;
    const float4 curr       = currTex.Load(int3(px, 0));

    if (g_TaaParams.x < 0.5f)
    {
        outTex[px] = curr;
        return;
    }

    // 1. UE5 Reverse-Z 3x3 最近深度膨胀（Near=1.0, Far=0.0 => 取最大值）+ 3x3 Karis-YCoCg 矩统计
    float  closestDepth  = 0.0f;
    int2   closestOffset = int2(0, 0);
    float3 m1            = float3(0.0f, 0.0f, 0.0f);
    float3 m2            = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2  sp = clamp(px + int2(dx, dy), int2(0, 0), int2(int(w) - 1, int(h) - 1));
            const float d  = depthTex.Load(int3(sp, 0));
            if (d > closestDepth)
            {
                closestDepth  = d;
                closestOffset = int2(dx, dy);
            }

            const float3 tapYCoCg = RGBToYCoCg(ReinhardCompress(currTex.Load(int3(sp, 0)).rgb));
            m1 += tapYCoCg;
            m2 += tapYCoCg * tapYCoCg;
        }
    }

    // 2. 用带抖动 g_InvViewProj 反投影世界坐标，再用无抖动矩阵计算纯速度矢量 velocityUV
    //    静止帧下 curUV == prevUV => velocityUV == 0 => historyUV == centerUV，彻底消除边缘 1px 涂抹
    const float2 closestUV   = (float2(px + closestOffset) + 0.5f) * invSize;
    const float  reprojDepth = max(closestDepth, 1e-6f);
    const float4 clipPos     = float4(closestUV.x * 2.0f - 1.0f, 1.0f - closestUV.y * 2.0f, reprojDepth, 1.0f);
    const float4 wsRaw       = mul(clipPos, g_InvViewProj);
    if (abs(wsRaw.w) <= 1e-6f)
    {
        outTex[px] = curr;
        return;
    }

    const float3 worldPos = wsRaw.xyz / wsRaw.w;
    const float4 curClip  = mul(float4(worldPos, 1.0f), g_CurUnjitteredViewProj);
    const float4 prevClip = mul(float4(worldPos, 1.0f), g_PrevUnjitteredViewProj);
    if (curClip.w <= 1e-5f || prevClip.w <= 1e-5f)
    {
        outTex[px] = curr;
        return;
    }

    const float2 curNdc     = curClip.xy / curClip.w;
    const float2 prevNdc    = prevClip.xy / prevClip.w;
    const float2 curUV      = float2(curNdc.x * 0.5f + 0.5f, 0.5f - curNdc.y * 0.5f);
    const float2 prevUV     = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    const float2 velocityUV = curUV - prevUV;
    const float2 historyUV  = centerUV - velocityUV;

    if (any(historyUV <= 0.0f) || any(historyUV >= 1.0f))
    {
        outTex[px] = curr;
        return;
    }

    // 3. 3x3 Karis-YCoCg 方差裁剪盒（防止高速运动鬼影，同时保持发丝/轮廓清晰）
    const float3 mean   = m1 * (1.0f / 9.0f);
    const float3 sigma  = sqrt(max(m2 * (1.0f / 9.0f) - mean * mean, 0.0f));
    const float  gamma  = max(g_TaaParams.w, 0.25f);
    const float3 boxMin = mean - sigma * gamma;
    const float3 boxMax = mean + sigma * gamma;

    float3 histRGB = SampleHistoryCatmullRom(histTex, historyUV, screenSize, invSize);
    if (any(isnan(histRGB)) || any(isinf(histRGB)))
        histRGB = curr.rgb;

    const float3 histYCoCg = clamp(RGBToYCoCg(ReinhardCompress(histRGB)), boxMin, boxMax);
    const float3 currYCoCg = RGBToYCoCg(ReinhardCompress(curr.rgb));

    // 4. 速度自适应时域融合并在解压回 Linear HDR 后兜底 NaN/Inf
    const float  velPx        = length(velocityUV * screenSize);
    const float  alpha        = lerp(g_TaaParams.y, g_TaaParams.z, saturate(velPx * 0.15f));
    const float3 blendedYCoCg = lerp(histYCoCg, currYCoCg, alpha);
    float3 resolved           = ReinhardDecompress(YCoCgToRGB(blendedYCoCg));
    if (any(isnan(resolved)) || any(isinf(resolved)))
        resolved = curr.rgb;

    outTex[px] = float4(resolved, curr.a);
}
