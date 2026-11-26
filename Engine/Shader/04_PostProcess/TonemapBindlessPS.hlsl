// ============================================================================
// 模块：TonemapBindlessPS.hlsl（SM 6.6 Bindless 版全局共享 Tonemap 着色器）
// 架构层：Engine/Shader/04_PostProcess（契约 5 + 契约 7.1 / WP-3A）
// 作用：
//   在 SupportsBindlessSM66==true 的设备上由 TonemapPass 自动启用，直接消费全局共享
//   BindlessGraphicsRootSignature（20/64 DWORD，零描述符表根参数！）：
//     - RootParam[0] (b0, 16 DWORD 根常量) : g_LinearHdrSrvIdx（4 分区单可见堆全局槽下标）
//     - RootParam[2] (b2, 2 DWORD 动态 CBV): TonemapConstants（16B 对齐）
//   色彩数学与 SM 6.4 传统版 TonemapPS.hlsl 100% 逐位一致，保证 49/49 Golden RMSE == 0。
// ============================================================================
#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/ColorPipeline.hlsli"

// RootParam[0]：16 DWORD 根常量中的首个 uint 直接携带 LinearHDRColor 在单可见堆中的全局下标
cbuffer BindlessTonemapRootConstants : register(b0)
{
    uint g_LinearHdrSrvIdx;
};

// RootParam[2]：Per-Pass 动态 CBV（与 TonemapPass::Constants 逐字节对齐）
cbuffer TonemapConstants : register(b2)
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
    // SM 6.6 Bindless 直索引：g_LinearHdrSrvIdx 来自 b0 根常量（波前一致），直接走 LoadBindless
    // 注意 HLSL 2021 词法规则：嵌套模板尖括号必须写为 "> >"（避免被词法器解析为右移运算符 ">>"）
    Texture2D<float4> linearHdrTex = LoadBindless<Texture2D<float4> >(g_LinearHdrSrvIdx);
    const float4 src = linearHdrTex.Load(int3(input.PositionH.xy, 0));

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
