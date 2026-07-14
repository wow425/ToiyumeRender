#pragma once

// 行主序，左乘

#ifndef _ENVIRONMENT_COMMON_HLSLI_
#define _ENVIRONMENT_COMMON_HLSLI_

#pragma pack_matrix(row_major)

static const float PI = 3.14159265359f;
static const float TWO_PI = 6.28318530718f;

float2 DirectionToLatLongUV(float3 dir)
{
   dir = normalize(dir);
   float phi = atan2(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / TWO_PI + 0.5, 0.5 - theta / PI);
}

float3 CubeFaceTexelToDirection(uint face, float2 pixel, float size)
{
   float2 uv = (pixel + 0.5) / size;
   float2 xy = uv * 2.0 - 1.0;
    xy.y = -xy.y;
   float3 dir = 0.0.xxx;
    if (face == 0) dir = float3(1.0, xy.y, -xy.x);
    else if (face == 1) dir = float3(-1.0, xy.y, xy.x);
   else if (face == 2) dir = float3(xy.x, 1.0, -xy.y);
   else if (face == 3) dir = float3(xy.x, -1.0, xy.y);
    else if (face == 4) dir = float3(xy.x, xy.y, 1.0);
   else dir = float3(-xy.x, xy.y, -1.0);
    return normalize(dir);
}

float RadicalInverse_VdC(uint bits)
{
   bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint sampleCount)
{
    return float2(float(i) / float(sampleCount), RadicalInverse_VdC(i));
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
   float a = roughness * roughness;
   float phi = TWO_PI * Xi.x;
  float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
   float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
   float3 H;
   H.x = cos(phi) * sinTheta;
   H.y = sin(phi) * sinTheta;
 H.z = cosTheta;
   float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
   return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
   float a = roughness;
   float k = (a * a) / 2.0;
   return NdotV / max(NdotV * (1.0 - k) + k, 0.0001);
}

float GeometrySmith_IBL(float3 N, float3 V, float3 L, float roughness)
{
  float NdotV = saturate(dot(N, V));
  float NdotL = saturate(dot(N, L));
  return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}



#endif
