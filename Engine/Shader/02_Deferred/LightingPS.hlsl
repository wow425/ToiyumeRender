Texture2D<float4> GBuffer_BaseColor : register(t0);
Texture2D<float2> GBuffer_Normal : register(t1);
Texture2D<float4> GBuffer_Material : register(t2);
Texture2D<float4> GBuffer_Emission : register(t3);
Texture2D<float> DepthTexture : register(t4);

cbuffer Light : register(b0)
{
    float3 DirectionalLightDir;
    float DirectionalLightIntensity;
    float3 DirectionalLightColor;
    float _pad0;
};

// cbuffer Camera : register(b1)
//{
//    float3 CameraPosWS;
//    float _pad1;
//};

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
}

float4 MainPS(float4 position : SV_POSITION) : SV_TARGET0
{
    uint2 pixel = uint2(position.xy);

    float3 baseColor =
        GBuffer_BaseColor.Load(int3(pixel, 0)).rgb;

    float2 encNormal =
        GBuffer_Normal.Load(int3(pixel, 0));

    float4 material =
        GBuffer_Material.Load(int3(pixel, 0));

    float3 emission =
        GBuffer_Emission.Load(int3(pixel, 0)).rgb;

    float depth =
        DepthTexture.Load(int3(pixel, 0));

    float3 normalWS = DecodeOctNormal(encNormal);

    float3 L = normalize(-DirectionalLightDir);

    float NdotL = saturate(dot(normalWS, L));

    float3 diffuse =
        baseColor *
        DirectionalLightColor *
        DirectionalLightIntensity *
        NdotL;

    float3 finalColor = diffuse + emission;

    return float4(finalColor, 1.0);
    //return float4(normalWS * 0.5 + 0.5, 1.0);
}
