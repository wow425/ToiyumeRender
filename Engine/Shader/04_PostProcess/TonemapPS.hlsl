// ============================================================================
// 模块：TonemapPS.hlsl（全局唯一共享线性 HDR -> 显示编码 LDR 后处理着色器）
// 架构层：Engine/Shader/04_PostProcess（WP-2C / 契约 5）
// 色彩空间契约：
//   输入 g_LinearHDR (t0)：R16G16B16A16_FLOAT 场景线性辐射度（Scene-Referred Linear HDR）
//                          alpha < 0.5 表示该像素携带逐像素调试原始值，需旁路直通。
//   输出 SV_Target0       ：R8G8B8A8_UNORM 显示编码色（Exposure -> ACES -> sRGB IEC 61966-2-1）
// 根签名契约：
//   RootParam[0] = SRV t0 (g_LinearHDR)
//   RootParam[1] = CBV b0 (TonemapConstants, 16B 对齐)
//   StaticSampler s0 = PointClamp
// ============================================================================
#include "02_Deferred/ColorPipeline.hlsli"

Texture2D<float4> g_LinearHDR  : register(t0);
SamplerState      g_PointClamp : register(s0);

cbuffer TonemapConstants : register(b0)
{
    float g_Exposure;            // offset  0：线性曝光乘子（ACES 之前应用）
    uint  g_EnableTonemap;       // offset  4：0=线性 saturate，1=ACES Filmic
    uint  g_EnableSRGBEncode;    // offset  8：0=线性直出（A/B 取证），1=sRGB 编码
    uint  g_BypassDisplayEncode; // offset 12：1=整帧旁路直通（Deferred DebugView 1..7 / Forward）
};

// 与 02_Deferred/FullScreenVS.hlsl 输出签名严格 1:1 匹配（仅含 SV_POSITION，不含 TEXCOORD0）
struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

float4 MainPS(VSOutput input) : SV_Target0
{
    // 1:1 逐纹素 Load 读取，避免不必要的滤波模糊，精确对齐上游像素中心
    const float4 src = g_LinearHDR.Load(int3(input.PositionH.xy, 0));

    // 整帧旁路（g_BypassDisplayEncode!=0）或逐像素调试旁路（src.a < 0.5，如 DXR RTDebugView 命中像素）
    const bool bypass = (g_BypassDisplayEncode != 0u) || (src.a < 0.5f);

    const float3 displayColor = bypass
        ? src.rgb
        : EncodeDisplayColor(
            src.rgb,
            g_Exposure,
            g_EnableTonemap != 0u,
            g_EnableSRGBEncode != 0u);

    return float4(displayColor, 1.0f);
}
