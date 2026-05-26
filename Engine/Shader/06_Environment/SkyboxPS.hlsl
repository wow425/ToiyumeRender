TextureCube gEnvironmentMap : register(t0);

SamplerState gLinearSampler : register(s0);

struct PSInput
{
    float4 PositionH : SV_POSITION;
    float3 Direction : TEXCOORD;
};

float4 MainPS(PSInput input) : SV_Target
{
    float3 dir = normalize(input.Direction);
    
    float3 color = gEnvironmentMap.Sample(gLinearSampler, dir).rgb;
    
    return float4(color, 1.0f);
}
