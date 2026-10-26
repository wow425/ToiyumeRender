#include "EnvironmentCommon.hlsli"
#include "../02_Deferred/ColorPipeline.hlsli"

// skybox是无限远环境辐射radiance， 只跟方向有关，因此通过UV反推得到世界空间方向向量，进行sample即可
// direction -> color

TextureCube<float4> gSkybox : register(t0);
SamplerState gLinearSampler : register(s0);

cbuffer CameraCB : register(b0)
{
    float4x4 gInvView;
    float4x4 gInvProj;

    // R4：显示编码参数。**必须与 C++ 的 SkyboxCameraCB 逐字段一致**。
    // x = Exposure
    // y = EnableTonemap (0/1)
    // z = EnableSRGBEncode (0/1)
    // w = unused
    float4 gDisplayParams;
}

struct VSOutput
{
    float4 PositionH : SV_POSITION;
    float2 UV : TEXCOORD0;
};

float4 MainPS(VSOutput input) : SV_Target
{
    // UV -> NDC
    float2 ndc = input.UV * 2.0 - 1.0;
    ndc.y *= -1.0;

    // clip space
    float4 clipPos = float4(ndc, 1.0, 1.0);

    // view space
    float4 viewPos = mul(clipPos, gInvProj);

    // world direction
    float3 worldDir = normalize(mul(viewPos.xyz, (float3x3)gInvView));

    // R4：HDR 采样值 → 与 Lighting 完全相同的曝光/ACES/sRGB 链。
    // 旧实现直接 return float4(color, 1.0)，把远大于 1.0 的 HDR radiance
    // 硬塞进 R8G8B8A8_UNORM，实测天空有 42.5% 的像素被截断成纯白 ——
    // 那不是"天空很亮"，而是"曝光链漏掉了天空"。
    float3 hdrColor = gSkybox.Sample(gLinearSampler, worldDir).rgb;

    float3 displayColor = EncodeDisplayColor(
        hdrColor,
        gDisplayParams.x,
        gDisplayParams.y > 0.5,
        gDisplayParams.z > 0.5);

    return float4(displayColor, 1.0f);
}
