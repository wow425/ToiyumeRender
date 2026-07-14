#include "EnvironmentCommon.hlsli"

RWTexture2D<float2> gBRDFLUT : register(u0); // UAV0,可读写,因并发性不保障写入顺序,故为unordered


cbuffer EnvironmentBakeCB : register(b0)
{
    uint gSize;
    uint gMipLevel;
    float gRoughness;
    uint gSampleCount;
}


float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V;
   V.x = sqrt(max(1.0 - NdotV * NdotV, 0.0));
   V.y = 0.0;
    V.z = NdotV;
    float A = 0.0;
   float B = 0.0;
   float3 N = float3(0.0, 0.0, 1.0);
   uint sampleCount = max(gSampleCount, 1u);

   for (uint i = 0; i < sampleCount; ++i)
   {
       float2 Xi = Hammersley(i, sampleCount);
       float3 H = ImportanceSampleGGX(Xi, N, roughness);
       float3 L = normalize(2.0 * dot(V, H) * H - V);
       float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
       float VdotH = saturate(dot(V, H));
       if (NdotL > 0.0)
       {
           float G = GeometrySmith_IBL(N, V, L, roughness);
           float GVis = (G * VdotH) / max(NdotH * NdotV, 0.0001);
           float Fc = pow(1.0 - VdotH, 5.0);
           A += (1.0 - Fc) * GVis;
           B += Fc * GVis;
       }
    }
    return float2(A, B) / float(sampleCount);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gSize || dispatchThreadID.y >= gSize) return;

   float2 uv = (float2(dispatchThreadID.xy) + 0.5) / float(gSize);
   gBRDFLUT[dispatchThreadID.xy] = IntegrateBRDF(uv.x, uv.y);
}