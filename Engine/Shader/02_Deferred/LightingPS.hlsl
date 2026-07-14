// #pragma hlsl profile ps_6_6
// #pragma hlsl entry main

#include "../02_Deferred/DeferredCommon.hlsli"


Texture2D<float4> GBuffer_BaseColor : register(t0);
Texture2D<float2> GBuffer_Normal : register(t1);
Texture2D<float4> GBuffer_Material : register(t2);
Texture2D<float4> GBuffer_Emission : register(t3);
Texture2D<float> DepthTexture : register(t4);

TextureCube<float4> IrradianceMap : register(t5);
TextureCube<float4> PrefilterMap : register(t6);
Texture2D<float2> BRDFLUT : register(t7);
SamplerState LinearClampSampler : register(s0);

cbuffer Light : register(b0)
{
    // xyz = Direction
    // w   = Directional Intensity
    float4 DirectionIntensity;

    // rgb = Directional Color
    // a   = unused
    float4 DirectionalColor;

    // xyz = Point Light Position
    // w   = Point Light Range
    float4 PointLightPositionRange;

    // rgb = Point Light Color
    // a   = Point Light Intensity
    float4 PointLightColorIntensity;
};

cbuffer Camera : register(b1)
{
    float4 cameraPos;
    float4x4 InViewProj;
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
   return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}


float4 MainPS(float4 position : SV_POSITION) : SV_TARGET0
{
    uint2 pixel = uint2(position.xy);

    //-----------------------------------
    // GBuffer
    //-----------------------------------
    float4 baseColorAO = GBuffer_BaseColor.Load(int3(pixel, 0));
    float3 baseColor = baseColorAO.rgb;
    float ao = baseColorAO.a;

    float4 material = GBuffer_Material.Load(int3(pixel, 0));
	float metallic = saturate(material.r);
    float roughness = max(material.g, 0.04);
    float specular = material.b;

    float3 emission = GBuffer_Emission.Load(int3(pixel, 0)).rgb;

    float depth = DepthTexture.Load(int3(pixel, 0));

    float2 encNormal = GBuffer_Normal.Load(int3(pixel, 0));
    float3 normalWS = DecodeOctNormal(encNormal);

    //-----------------------------------
    // Position Reconstruction
    //-----------------------------------
    float2 renderResolution = float2(1920, 1080);
    float2 uv = (position.xy + 0.5) / renderResolution;
    float3 positionWS = ReconstructWorldPosition(uv, depth, InViewProj);

    float3 V = normalize(cameraPos.xyz - positionWS);
    float3 N = normalize(normalWS);
    float3 F0 = float3(0.04, 0.04, 0.04) * max(specular, 0.001); // ?
    F0 = lerp(F0, baseColor, metallic);
    //-----------------------------------
    // Directional Light
    //-----------------------------------
    float3 dirL = normalize(-DirectionIntensity.xyz);
    float3 dirRadiance = DirectionalColor.rgb * DirectionIntensity.w;

    float3 directionalLighting = EvaluateCookTorranceLight(
        N,
        V,
        dirL,
        baseColor,
        metallic,
        roughness,
        dirRadiance
    );

    //-----------------------------------
    // Point Light
    //-----------------------------------
    float3 pointToLight = PointLightPositionRange.xyz - positionWS;
    float pointDistance = length(pointToLight);
    float3 pointL = pointToLight / max(pointDistance, 0.0001);

    float pointRange = PointLightPositionRange.w;
    float pointAttenuation = ComputePointAttenuation(pointDistance, pointRange);

    float3 pointRadiance = PointLightColorIntensity.rgb * PointLightColorIntensity.a * pointAttenuation;
        
    float3 pointLighting = EvaluateCookTorranceLight(
        N,
        V,
        pointL,
        baseColor,
        metallic,
        roughness,
        pointRadiance
    );

    //-----------------------------------
    // PBR IBL Ambient
    //-----------------------------------
    //float3 ambient = baseColor * 0.2 * ao; // 添加IBL后改为0.03
    // 尚未看
   float NdotV = saturate(dot(N, V));
   float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float3 irradiance = IrradianceMap.Sample(LinearClampSampler, N).rgb;
   float3 diffuseIBL = irradiance * baseColor;
    float3 R = reflect(-V, N);
   const float maxPrefilterMip = 4.0;
  float3 prefilteredColor = PrefilterMap.SampleLevel(LinearClampSampler, R, roughness * maxPrefilterMip).rgb;
   float2 envBRDF = BRDFLUT.Sample(LinearClampSampler, float2(NdotV, roughness)).rg;
   float3 specularIBL = prefilteredColor * (F * envBRDF.x + envBRDF.y);
   float3 ambient = (kD * diffuseIBL + specularIBL) * ao;


    //-----------------------------------
    // Final Color
    //-----------------------------------
    float3 finalColor = ambient + directionalLighting + pointLighting + emission;

    finalColor = finalColor / (finalColor + 1.0); // ToneMapping
    finalColor = pow(saturate(finalColor), 1.0 / 2.2); // 伽马校正

    return float4(finalColor, 1.0);
    //return float4(abs(positionWS) * 0.1, 1);
    //return float4(positionWS * 0.01, 1);
    // return float4(normalWS * 0.5 + 0.5, 1);
    //return float4(depth.xxx, 1);
}
