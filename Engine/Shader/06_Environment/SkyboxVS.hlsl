#include "../06_Environment/EnvironmentCommon.hlsli"

cbuffer CameraCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
}

struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 PositionH : SV_Position;
    float3 Direction : TEXCOORD;
};

VSOutput MainVS(VSInput input)
{
    VSOutput output;
    
    float4 pos = float4(input.Position, 1.0f);
    
    // 去掉view translation ??
    float4x4 viewNoTranslation = gView;
    viewNoTranslation[3][0] = 0.0f;
    viewNoTranslation[3][1] = 0.0f;
    viewNoTranslation[3][2] = 0.0f;
    
    float4 clipPos = mul(mul(pos, viewNoTranslation), gProj);
    
    output.PositionH = clipPos.xyzw;
    output.Direction = input.Position;

    return output;
}
