#include "EnvironmentCommon.hlsli"

TextureCube<float4> gEnvironmentCube : register(t0);
RWTexture2DArray<float4> gPrefilterCube : register(u0);
SamplerState gLinearSampler : register(s0);

cbuffer EnvironmentBakeCB : register(b0)
{
   uint gSize;
   uint gMipLevel;
   float gRoughness;
   uint gSampleCount;
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
   if (dispatchThreadID.x >= gSize || dispatchThreadID.y >= gSize || dispatchThreadID.z >= 6)  return;
  float3 N = CubeFaceTexelToDirection(dispatchThreadID.z, dispatchThreadID.xy, gSize);
  float3 R = N;
   float3 V = R;
   float3 prefilteredColor = 0.0;
   float totalWeight = 0.0;
    uint sampleCount = max(gSampleCount, 1u);
   for (uint i = 0; i < sampleCount; ++i)
   {
       float2 Xi = Hammersley(i, sampleCount);
       float3 H = ImportanceSampleGGX(Xi, N, max(gRoughness, 0.001));
       float3 L = normalize(2.0 * dot(V, H) * H - V);
      float NdotL = saturate(dot(N, L));
       if (NdotL > 0.0)
       {
           prefilteredColor += gEnvironmentCube.SampleLevel(gLinearSampler, L, 0).rgb * NdotL;
           totalWeight += NdotL;
       }
    }
   prefilteredColor /= max(totalWeight, 0.0001);
   gPrefilterCube[dispatchThreadID] = float4(prefilteredColor, 1.0);
}