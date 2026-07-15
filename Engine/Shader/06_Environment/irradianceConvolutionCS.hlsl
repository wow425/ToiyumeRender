#include "EnvironmentCommon.hlsli"

TextureCube<float4> gEnvironmentCube : register(t0);
RWTexture2DArray<float4> gIrradianceCube : register(u0);
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
   if (dispatchThreadID.x >= gSize || dispatchThreadID.y >= gSize || dispatchThreadID.z >= 6) return;

   float3 N = CubeFaceTexelToDirection(dispatchThreadID.z, dispatchThreadID.xy, gSize);
   float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
   float3 irradiance = 0.0;
    uint sampleCount = max(gSampleCount, 1u);
    for (uint i = 0; i < sampleCount; ++i)
    {
       float2 Xi = Hammersley(i, sampleCount);
       float phi = TWO_PI * Xi.x;
       float cosTheta = sqrt(1.0 - Xi.y);
       float sinTheta = sqrt(Xi.y);
       float3 tangentSample = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
       float3 sampleDir = normalize(right * tangentSample.x + up * tangentSample.y + N * tangentSample.z);
        // 采样后限制最大 Radiance，防止个别 HDR 像素导致积分爆炸
        float3 sampleColor = gEnvironmentCube.SampleLevel(gLinearSampler, sampleDir, 0).rgb;
        irradiance += min(sampleColor, float3(10.0, 10.0, 10.0)); // 这里的 10.0 可根据 HDR 曝光度调整
    }

   irradiance /= float(sampleCount);
   gIrradianceCube[dispatchThreadID] = float4(irradiance, 1.0);
}
