#pragma once

#ifndef _ENVIRONMENT_COMMON_HLSLI_
#define _ENVIRONMENT_COMMON_HLSLI_

#pragma pack_matrix(row_major)

static const float PI = 3.14159265359f;

// ?
float2 SampleSphericalMap(float3 v)
{
    float2 uv =
    {
        atan2(v.z, v.x),
        asin(v.y)
    };

    uv *= float2(0.1591f, 0.3183f);
    uv += 0.5f;

    return uv;
}

// ?
float3 GetCubeDirection(uint face, float2 uv)
{
    uv = uv * 2.0f - 1.0f;

    switch (face)
    {
        case 0:
            return normalize(float3(1.0f, -uv.y, -uv.x)); // +X
        case 1:
            return normalize(float3(-1.0f, -uv.y, uv.x)); // -X
        case 2:
            return normalize(float3(uv.x, 1.0f, uv.y)); // +Y
        case 3:
            return normalize(float3(uv.x, -1.0f, -uv.y)); // -Y
        case 4:
            return normalize(float3(uv.x, -uv.y, 1.0f)); // +Z
        case 5:
            return normalize(float3(-uv.x, -uv.y, -1.0f)); // -Z
    }

    return 0;
}

#endif
