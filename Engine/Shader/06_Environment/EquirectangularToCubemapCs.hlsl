#include "EnvironmentCommon.hlsli"

Texture2D<float4> gEquirectangularMap : register(t0);
RWTexture2DArray<float4> gEnvironmentCube : register(u0);

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

    float3 dir = CubeFaceTexelToDirection(dispatchThreadID.z, dispatchThreadID.xy, gSize);
    float2 uv = DirectionToLatLongUV(dir);
    float3 color = gEquirectangularMap.SampleLevel(gLinearSampler, uv, 0).rgb;
   gEnvironmentCube[dispatchThreadID] = float4(color, 1.0);
}