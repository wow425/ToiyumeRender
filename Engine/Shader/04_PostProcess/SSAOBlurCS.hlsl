// ============================================================================
// 模块：SSAOBlurCS.hlsl（SM 6.6 Bindless 5x5 双边深度/法线感知 SSAO 模糊着色器）
// 架构层：Engine/Shader/04_PostProcess（WP-3C · 契约 7.1）
// 约定与契约：
//   - 根签名：RenderServices::GetBindlessComputeRootSignature()（20/64 DWORD）：
//       RootParam[0] = b0 : 4 uint 根常量（RawSsaoSRV / DepthSRV / NormalSRV / BlurUAV）
//       RootParam[2] = b2 : SSAOPass::BlurConstants（32B，16B 对齐）
//   - 滤波核：5x5 双边核，联合空间高斯权重 + 法线点积权重 + 线性深度差权重，
//             在抹平 IGN 采样噪点的同时保持物体轮廓与法线折边锐利不渗色。
// ============================================================================
#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"

cbuffer BindlessSsaoBlurIndices : register(b0)
{
    uint g_RawSsaoSrvIdx; // DWORD 0
    uint g_DepthSrvIdx;   // DWORD 1
    uint g_NormalSrvIdx;  // DWORD 2
    uint g_OutputUavIdx;  // DWORD 3
};

cbuffer SsaoBlurCB : register(b2)
{
    float2 g_ScreenSize;    // offset  0
    float2 g_InvScreenSize; // offset  8
    float  g_NearClip;      // offset 16
    float  g_FarClip;       // offset 20
    float  g_DepthSharpness;// offset 24
    float  g_Pad;           // offset 28
};

// 从非反转透视深度 [0,1] 还原正线性视空间距离（与 Camera.cpp:99-100 投影公式严格互逆）
float LinearizeDepth(float d)
{
    float q1 = g_FarClip / (g_NearClip - g_FarClip);
    float q2 = q1 * g_NearClip;
    // d = -q1 - q2 / z_pos  =>  z_pos = -q2 / (d + q1)
    return abs(q2 / max(abs(d + q1), 1e-6f));
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    int width  = (int)(g_ScreenSize.x + 0.5f);
    int height = (int)(g_ScreenSize.y + 0.5f);
    if ((int)dtid.x >= width || (int)dtid.y >= height)
        return;

    Texture2D<float>   rawAoTex  = LoadBindless<Texture2D<float> >(g_RawSsaoSrvIdx);
    Texture2D<float>   depthTex  = LoadBindless<Texture2D<float> >(g_DepthSrvIdx);
    Texture2D<float2>  normalTex = LoadBindless<Texture2D<float2> >(g_NormalSrvIdx);
    RWTexture2D<float> outAoTex  = LoadBindless<RWTexture2D<float> >(g_OutputUavIdx);

    int2 centerPx = int2(dtid.xy);
    float centerDepth = depthTex.Load(int3(centerPx, 0));
    if (centerDepth >= 0.99999f)
    {
        outAoTex[dtid.xy] = 1.0f;
        return;
    }

    float  centerLinZ = LinearizeDepth(centerDepth);
    float3 centerN    = DecodeOctNormal(normalTex.Load(int3(centerPx, 0)));

    float sumAO = 0.0f;
    float sumW  = 0.0f;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 tapPx = clamp(centerPx + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));
            float tapDepth = depthTex.Load(int3(tapPx, 0));
            if (tapDepth >= 0.99999f)
                continue;

            float  tapAO   = rawAoTex.Load(int3(tapPx, 0));
            float  tapLinZ = LinearizeDepth(tapDepth);
            float3 tapN    = DecodeOctNormal(normalTex.Load(int3(tapPx, 0)));

            float spatialW = exp(-float(dx * dx + dy * dy) * 0.25f);
            float normalW  = pow(max(dot(centerN, tapN), 0.0f), 8.0f);
            float depthW   = exp(-abs(centerLinZ - tapLinZ) * g_DepthSharpness);

            float w = spatialW * normalW * depthW;
            sumAO += tapAO * w;
            sumW  += w;
        }
    }

    outAoTex[dtid.xy] = (sumW > 1e-5f) ? saturate(sumAO / sumW) : rawAoTex.Load(int3(centerPx, 0));
}
