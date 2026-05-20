#include "../00_Common/Common.hlsli"
#include "../00_Common/DeferredCommon.hlsli"

Texture2D<float4> baseColorTexture : register(t0);
Texture2D<float3> metallicRoughnessTexture : register(t1);
Texture2D<float1> occlusionTexture : register(t2);
Texture2D<float3> emissiveTexture : register(t3);
Texture2D<float3> normalTexture : register(t4);

SamplerState baseColorSampler : register(s0);
SamplerState metallicRoughnessSampler : register(s1);
SamplerState occlusionSampler : register(s2);
SamplerState emissiveSampler : register(s3);
SamplerState normalSampler : register(s4);

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor; // default=[1,1,1,1
    float3 emissiveFactor; // default=[0,0,0]
    float normalTextureScale; // default=1
    
    float metallicFactor; // default=1
    float roughnessFactor; // default=1
    
    uint flags;
}

cbuffer GlobalConstants : register(b1) // 已核对cpp端结构体定义顺序
{
    float4x4 ViewProj;
    float3 ViewerPos;
}

struct VSOutput
{
    float4 PositionCS : SV_POSITION; // Clip Space
    
    float3 PositionWS : POSITION_WS; // World Space
    float3 NormalWS : NORMAL_WS; // World Space
    float4 TangentWS : TANGENT_WS; // World Space
    
    float2 UV0 : TEXCOORD0;
};

struct GBufferOutput
{
    float4 BaseColor : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 Emission : SV_Target3;
};

// ============================================
// Octahedral Encoding ??????
// ============================================

float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));

    float2 enc = n.xy;

    if (n.z < 0.0)
    {
        enc = (1.0 - abs(enc.yx)) * sign(enc.xy);
    }

    return enc * 0.5 + 0.5;
}


// ============================================
// TBN Construction ???????
// ============================================

float3x3 ComputeTBN(float3 normalWS, float4 tangentWS)
{
    float3 T = normalize(tangentWS.xyz);

    float3 N = normalize(normalWS);

    float3 B = cross(N, T) * tangentWS.w;

    return float3x3(T, B, N);
}


// ============================================
// Normal Mapping ????????
// ============================================

float3 GetNormalWS(VSOutput input)
{
    float3 tangentNormal =
        normalTexture.Sample(normalSampler, input.UV0).xyz;

    tangentNormal = tangentNormal * 2.0 - 1.0;

    tangentNormal.xy *= normalTextureScale;

    float3x3 TBN =
        ComputeTBN(input.NormalWS, input.TangentWS);

    return normalize(
        mul(tangentNormal, TBN)
    );
}


[RootSignature(Renderer_RootSig)]
GBufferOutput MainPS(VSOutput input)
{
    GBufferOutput output;
    
    float4 basecolor = baseColorFactor * baseColorTexture.Sample(baseColorSampler, input.UV0);
    
    // ============================================
    // Metallic Roughness
    // glTF:
    // B = Metallic
    // G = Roughness
    // ============================================
    float3 mrSample = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.UV0);
    float metallic = mrSample.b * metallicFactor;
    float roughness = mrSample.g * roughnessFactor;
    
    float ao = occlusionTexture.Sample(occlusionSampler, input.UV0);
    
    float3 emissive = emissiveFactor * emissiveTexture.Sample(emissiveSampler, input.UV0);
    
    // ============================================
    // Normal
    // ============================================
    float3 normalWS = GetNormalWS(input);
    float2 encodedNormal = OctEncode(normalWS);

    // ============================================
    // GBuffer Write
    // ============================================
    output.BaseColor = float4(basecolor.rgb, ao);    // RT0。RGB = BaseColor。 A   = AO
    output.Normal = float4(encodedNormal, 0.0, 0.0); // RT1。 RG = Oct Normal ？？
    output.Material = float4(metallic, roughness, 0.5, 0.0); // RT2。B = Specular / Reserved。A = Material ID
    output.Emission = float4(emissive, 0.0);                 // RT3

    return output;
}
