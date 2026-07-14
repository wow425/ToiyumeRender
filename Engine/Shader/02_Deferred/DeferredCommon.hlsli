#pragma once

#ifndef __DEFERRED_COMMON_HLSLI__
#define __DEFERRED_COMMON_HLSLI__

#pragma pack_matrix(row_major)

// ============================================
// Octahedral Encoding 八面体编码技术
// ============================================
float2 EncodeOctNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);

    float2 enc = n.xy;

    if (n.z < 0.0)
    {
        enc = (1.0 - abs(enc.yx)) * sign(enc.xy);
    }

    return enc * 0.5 + 0.5;
}
float3 DecodeOctNormal(float2 enc)
{
    // 1. 从 [0, 1] 映射回 [-1, 1]
    float2 f = enc * 2.0 - 1.0;
    // 2. 还原 Z，基于 L1 归一化公式: |x| + |y| + |z| = 1 -> z = 1 - |x| - |y|
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    // 3. 如果原本在下半球 (n.z 为负)，通过反转对角线还原 XY
    float t = saturate(-n.z);
    n.xy -= sign(n.xy) * t;
    // 4. 标准的 L2 归一化 (Normalize / 正規化) 变回单位向量
    return normalize(n);
} // Octahedral Encoding 八面体编码技术
// ============================================
//  PBR光照模型所需
// ============================================
float3 ReconstructWorldPosition(float2 uv, float depth, float4x4 invViewProj)
{
    // 通过屏幕uv空间构建NDC以获取裁剪空间坐标，再使用逆视图投影矩阵来获取世界空间坐标下的位置, 最后透视除法
    
    float2 ndc; // x,y[-1, 1] z[0, 1]

    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0; // y反轉。

    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, invViewProj);
        
    worldPos.xyz /= worldPos.w;

    return worldPos.xyz;
}

static const float PI = 3.14159265359;

float FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;
    return NdotV / max(denom, 0.0001);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));

    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// CookTorrance BRDF
float3 EvaluateCookTorranceLight(
    float3 N,
    float3 V,
    float3 L,
    float3 baseColor,
    float metallic,
    float roughness,
    float3 lightRadiance)
{
    float3 H = normalize(V + L);

    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, baseColor, metallic);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    denominator = max(denominator, 0.0001);

    float3 specularBRDF = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);

    float3 diffuseBRDF = kD * baseColor / PI;

    float NdotL = saturate(dot(N, L));
    return (diffuseBRDF + specularBRDF) * lightRadiance * NdotL;
}

float ComputePointAttenuation(float distance, float range)
{
    float att = saturate(1.0 - distance / range);
    return att * att;
}









#endif // __DEFERRED_COMMON_HLSLI__
