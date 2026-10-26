#include "../02_Deferred/DeferredCommon.hlsli"

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

// R1.7：与 ConstantBuffers.h 的 GlobalConstants 逐字段一致。
// 原先这里叫 ViewProj（C++ 叫 ViewProjMatrix），且缺 SunDirection/SunIntensity，
// 两侧契约已经悄悄分叉。现统一命名与布局。
cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;   // offset  0
    float4   CameraPos;        // offset 64
    float4   SunDirection;     // offset 80
    float4   SunIntensity;     // offset 96
}

// ============================================================
// R4：材质标志位解码
// ------------------------------------------------------------
// flags 是一个 32 位位域（定义见 ConstantBuffers.h::MaterialConstants）：
//   bit 0..4 : 各纹理使用 UV0/UV1
//   bit 5    : twoSided
//   bit 6    : alphaTest        ← glTF alphaMode == "MASK"
//   bit 7    : alphaBlend       ← glTF alphaMode == "BLEND"（透明混合，不属于 R4 范围）
//   bit 16..31: alphaRef，**half 精度**存储的 glTF alphaCutoff
// ModelConvert 原样搬运该位域，因此这里必须按同一布局解码。
// 历史状态：GBufferPS 从来不做 alpha 测试，MASK 材质（如 toneriko1）
// 的镂空边缘被画成实心方块 —— 不是"材质错"，是这条分支根本不存在。
// ============================================================
#define MATFLAG_ALPHA_TEST  (1u << 6)
#define MATFLAG_ALPHA_BLEND (1u << 7)

// alphaRef 以 half 存在高 16 位。用 HLSL 内建 f16tof32 解码，
// 不要自己写位运算近似：half 的指数/尾数布局与 float 不同，
// 手写移位会得到完全错误的阈值（0.5 -> 6.1e-5 这种量级）。
float DecodeAlphaRef(uint flags)
{
    return f16tof32((flags >> 16) & 0xFFFFu);
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
// TBN Construction 
// ============================================
float3x3 ComputeTBN(float3 normalWS, float4 tangentWS)
{
    float3 T = normalize(tangentWS.xyz);

    float3 N = normalize(normalWS);

    float3 B = cross(N, T) * tangentWS.w;

    return float3x3(T, B, N);
}


// ============================================
// Normal Mapping 
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


GBufferOutput MainPS(VSOutput input)
{
    GBufferOutput output;
    
    float4 basecolor = baseColorFactor * baseColorTexture.Sample(baseColorSampler, input.UV0);

    // ============================================
    // R4：Alpha Test（glTF MASK）
    // ============================================
    // 放在最前面：被裁掉的片元不应产生任何后续采样与 GBuffer 写入。
    // clip 会终止整条像素着色器（对 MRT 的所有目标一致生效），
    // 因此 GBuffer 的四个 RT 不会出现"部分目标被写"的不一致状态。
    //
    // 该 PSO 同时用于 Depth Pass（BlendNoColorWrite），所以 Z-PrePass
    // 也会得到同样的镂空深度，不会出现"深度实心、颜色镂空"的错配。
    if ((flags & MATFLAG_ALPHA_TEST) != 0)
    {
        clip(basecolor.a - DecodeAlphaRef(flags));
    }
    
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
    float2 encodedNormal = EncodeOctNormal(normalWS);

    // ============================================
    // GBuffer Write
    // ============================================
    output.BaseColor = float4(basecolor.rgb, ao); // RT0。RGB = BaseColor。 A   = AO
    // 读取法线贴图->构建 TBN->得到最终 normalWS-> 写入 GBuffer
    output.Normal = float4(encodedNormal, 0.0, 0.0); // RT1。 RG = Oct Normal ？？
    output.Material = float4(metallic, roughness, 0.5, 0.0); // RT2。B = Specular。A = Material ID
    output.Emission = float4(emissive, 0.0); // RT3

    return output;
}
