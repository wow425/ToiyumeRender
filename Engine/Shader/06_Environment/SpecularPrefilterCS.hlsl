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

// GGX NDF
float DistributionGGX( float3 N, float3 H,float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float NdotH = max(dot(N, H), 0.0);

    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
       
    return  a2 /   (PI * denom * denom);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gSize || dispatchThreadID.y >= gSize ||  dispatchThreadID.z >= 6) return;

    float3 N = CubeFaceTexelToDirection(    dispatchThreadID.z,  dispatchThreadID.xy,   gSize);
       
    float3 R = N;
    float3 V = R;

    float3 prefilteredColor = 0.0;

    float totalWeight = 0.0;

    uint sampleCount = max(gSampleCount, 1u);
       
    for (uint i = 0; i < sampleCount; i++)
    {

        float2 Xi =  Hammersley(i, sampleCount);
          
        float3 H =  ImportanceSampleGGX(  Xi, N,  max(gRoughness, 0.001));
          
        float3 L = normalize(2.0 * dot(V, H) *   H  - V);
           
                
        float NdotL =saturate(dot(N, L));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
           
        if (NdotL > 0.0)
        {
            float D = DistributionGGX(N, H,gRoughness);
               
            // GGX importance sampling PDF
            float pdf = (D * NdotH)/ max( 4.0 * VdotH, 0.001);

            /*
              根据solid angle计算mip
              防止roughness高时采到过亮区域
            */
            float resolution =(float) gSize;
            
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
               
            float saSample =1.0 /(sampleCount * pdf);
                
            float mip = 0.5 * log2(saSample /saTexel);
               
            mip = clamp(mip, 0.0, gMipLevel - 1);
                
            float3 color =gEnvironmentCube .SampleLevel(gLinearSampler,L,mip).rgb;
                
            float weight = NdotL;
            prefilteredColor += color * weight;
            totalWeight += weight;
        }
    }

    prefilteredColor /=max(totalWeight, 0.0001);
        
    gPrefilterCube[dispatchThreadID] = float4( prefilteredColor,1.0);
}
