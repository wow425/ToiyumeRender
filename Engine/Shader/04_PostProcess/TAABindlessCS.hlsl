// ============================================================================
// 模块：TAABindlessCS.hlsl（SM 6.6 Bindless 时域抗锯齿 Resolve 计算着色器）
// 架构层：Engine/Shader/04_PostProcess（WP-3C · 契约 7.1）
// 约定与契约：
//   - 色彩空间：输入与输出均为场景线性 HDR（R16G16B16A16_FLOAT），运行于 SkyboxPass 之后、
//               TonemapPass 之前；内部采用 Karis 逆亮度压缩 + YCoCg 方差裁剪防止高光闪烁与鬼影；
//   - 根签名：RenderServices::GetBindlessComputeRootSignature()（20/64 DWORD）：
//       RootParam[0] = b0 : 4 uint 根常量（CurrentHdrSRV / HistoryHdrSRV / DepthSRV / OutputHdrUAV）
//       RootParam[2] = b2 : TAAPass::Constants（224B，16B 对齐）
//   - 核心四步流水线：
//       1) 3x3 最近深度膨胀（Closest-Depth Dilation）消除前景轮廓边缘运动矢量错配；
//       2) 未抖动历史重投影（InvJitteredViewProj -> Cur/Prev UnjitteredViewProj），静止帧速度严格为 0；
//       3) 5-tap Catmull-Rom 双三次历史重采样，缓解双线性滤波带来的画面发糊；
//       4) Karis 亮度压缩 + YCoCg 3x3 均值/标准差方差裁剪盒钳制历史颜色，再指数移动平均融合。
// ============================================================================
#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"

cbuffer BindlessTaaIndices : register(b0)
{
    uint g_CurrentHdrSrvIdx; // DWORD 0
    uint g_HistoryHdrSrvIdx; // DWORD 1
    uint g_DepthSrvIdx;      // DWORD 2
    uint g_OutputHdrUavIdx;  // DWORD 3
};

cbuffer TaaConstants : register(b2)
{
    float4x4 g_InvJitteredViewProj;   // offset   0：当前帧带抖动逆视图投影矩阵
    float4x4 g_CurUnjitteredViewProj; // offset  64：当前帧无抖动视图投影矩阵
    float4x4 g_PrevUnjitteredViewProj;// offset 128：上一帧无抖动视图投影矩阵
    float2   g_ScreenSize;            // offset 192
    float2   g_InvScreenSize;         // offset 200
    float    g_BlendFactor;           // offset 208：当前帧权重 alpha ∈ [0.02, 1.0]
    float    g_VarianceGamma;         // offset 212：3x3 方差盒缩放系数（默认 1.25）
    uint     g_HistoryValid;          // offset 216：0=首帧/Resize/重置直接拷贝，1=正常时域融合
    uint     g_Pad;                   // offset 220
};

float LuminanceHDR(float3 rgb)
{
    return max(dot(rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

// Karis 逆亮度压缩：抑制 HDR 高光噪点把 3x3 邻域方差盒撑得过大
float3 CompressHDR(float3 c)
{
    c = max(c, 0.0f);
    return c / (1.0f + LuminanceHDR(c));
}

float3 DecompressHDR(float3 c)
{
    return max(c, 0.0f) / max(1.0f - LuminanceHDR(c), 1e-4f);
}

float3 RGBToYCoCg(float3 rgb)
{
    float Y  =  0.25f * rgb.r + 0.50f * rgb.g + 0.25f * rgb.b;
    float Co =  0.50f * rgb.r                 - 0.50f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.50f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float t = ycocg.x - ycocg.z;
    float r = t + ycocg.y;
    float g = ycocg.x + ycocg.z;
    float b = t - ycocg.y;
    return max(float3(r, g, b), 0.0f);
}

// 5-tap Catmull-Rom 双三次近似采样（利用硬件双线性采样将 16 tap 折叠为 5 tap）
float3 SampleHistoryCatmullRom(Texture2D<float4> historyTex, float2 uv)
{
    float2 samplePos = uv * g_ScreenSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    float2 f         = samplePos - texPos1;

    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12      = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5f);

    float2 texPos0  = (texPos1 - 1.0f) * g_InvScreenSize;
    float2 texPos3  = (texPos1 + 2.0f) * g_InvScreenSize;
    float2 texPos12 = (texPos1 + offset12) * g_InvScreenSize;

    float3 result = 0.0f;
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos0.y),  0).rgb * (w12.x * w0.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos0.x,  texPos12.y), 0).rgb * (w0.x  * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos12.y), 0).rgb * (w12.x * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos3.x,  texPos12.y), 0).rgb * (w3.x  * w12.y);
    result += historyTex.SampleLevel(g_SampLinearClamp, float2(texPos12.x, texPos3.y),  0).rgb * (w12.x * w3.y);

    float totalWeight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / max(totalWeight, 1e-5f), 0.0f);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    int width  = (int)(g_ScreenSize.x + 0.5f);
    int height = (int)(g_ScreenSize.y + 0.5f);
    if ((int)dtid.x >= width || (int)dtid.y >= height)
        return;

    Texture2D<float4>   currentTex = LoadBindless<Texture2D<float4> >(g_CurrentHdrSrvIdx);
    Texture2D<float4>   historyTex = LoadBindless<Texture2D<float4> >(g_HistoryHdrSrvIdx);
    Texture2D<float>    depthTex   = LoadBindless<Texture2D<float> >(g_DepthSrvIdx);
    RWTexture2D<float4> outputTex  = LoadBindless<RWTexture2D<float4> >(g_OutputHdrUavIdx);

    int2   centerPx = int2(dtid.xy);
    float4 currSrc  = currentTex.Load(int3(centerPx, 0));

    // 冷启动 / Resize / 开关首次开启 / Reset：直接将当前帧写入历史缓冲，零残影污染
    if (g_HistoryValid == 0u || g_BlendFactor >= 0.999f)
    {
        outputTex[dtid.xy] = currSrc;
        return;
    }

    // 1. 3x3 最近深度膨胀 + 同步累加压缩 YCoCg 一阶/二阶矩
    float  closestDepth = 1.0f;
    int2   closestPx    = centerPx;
    float3 m1           = 0.0f;
    float3 m2           = 0.0f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 tapPx = clamp(centerPx + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));

            float tapDepth = depthTex.Load(int3(tapPx, 0));
            if (tapDepth < closestDepth)
            {
                closestDepth = tapDepth;
                closestPx    = tapPx;
            }

            float3 tapColor = currentTex.Load(int3(tapPx, 0)).rgb;
            float3 tapYCoCg = RGBToYCoCg(CompressHDR(tapColor));
            m1 += tapYCoCg;
            m2 += tapYCoCg * tapYCoCg;
        }
    }

    // 2. 无抖动相机重投影：静止帧 curUV == prevUV => velocity == 0，确保亚像素抖动稳定收敛
    float2 centerUV  = (float2(centerPx)  + 0.5f) * g_InvScreenSize;
    float2 closestUV = (float2(closestPx) + 0.5f) * g_InvScreenSize;

    float  reprojDepth = min(closestDepth, 0.99999f);
    float3 worldPos    = ReconstructWorldPosition(closestUV, reprojDepth, g_InvJitteredViewProj);

    float4 curClip  = mul(float4(worldPos, 1.0f), g_CurUnjitteredViewProj);
    float4 prevClip = mul(float4(worldPos, 1.0f), g_PrevUnjitteredViewProj);

    if (curClip.w <= 1e-5f || prevClip.w <= 1e-5f)
    {
        outputTex[dtid.xy] = currSrc;
        return;
    }

    float2 curNDC  = curClip.xy / curClip.w;
    float2 prevNDC = prevClip.xy / prevClip.w;
    float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);

    float2 velocityUV = curUV - prevUV;
    float2 historyUV  = centerUV - velocityUV;

    // 越出屏幕历史边界（新露出区域）放弃历史直接采用当前帧
    if (any(historyUV < 0.0f) || any(historyUV > 1.0f))
    {
        outputTex[dtid.xy] = currSrc;
        return;
    }

    // 3. 5-tap Catmull-Rom 历史采样 + 3x3 YCoCg 方差盒裁剪防鬼影
    float3 mean   = m1 * (1.0f / 9.0f);
    float3 stddev = sqrt(max(0.0f, m2 * (1.0f / 9.0f) - mean * mean));
    float  gamma  = max(g_VarianceGamma, 0.25f);
    float3 boxMin = mean - gamma * stddev;
    float3 boxMax = mean + gamma * stddev;

    float3 histRGB     = SampleHistoryCatmullRom(historyTex, historyUV);
    float3 histYCoCg   = clamp(RGBToYCoCg(CompressHDR(histRGB)), boxMin, boxMax);
    float3 currYCoCg   = RGBToYCoCg(CompressHDR(currSrc.rgb));

    // 4. 在压缩 YCoCg 空间线性融合后逆映射回 Linear HDR
    float  alpha       = clamp(g_BlendFactor, 0.01f, 1.0f);
    float3 blendedYCoCg = lerp(histYCoCg, currYCoCg, alpha);
    float3 resolvedRGB = DecompressHDR(YCoCgToRGB(blendedYCoCg));

    // 防护极端数值导致 NaN/Inf 扩散进历史缓冲
    if (any(isnan(resolvedRGB)) || any(isinf(resolvedRGB)))
        resolvedRGB = currSrc.rgb;

    outputTex[dtid.xy] = float4(resolvedRGB, currSrc.a);
}
