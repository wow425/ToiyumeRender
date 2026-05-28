#include "../06_Environment/EnvironmentCommon.hlsli"

// skybox是无限远环境辐射radiance， 只跟方向有关，因此通过UV反推得到世界空间方向向量，进行sample即可
// direction -> color


TextureCube<float4> gSkybox : register(t0);
SamplerState gLinearSampler : register(s0);

cbuffer CameraCB : register(b0)
{
    float4x4 gInvView;
    float4x4 gInvProj;
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
    
    float3 color = gSkybox.Sample(gLinearSampler, worldDir).rgb;
    
    return float4(color, 1.0f);
}
