#include "../06_Environment/EnvironmentCommon.hlsli"

cbuffer CameraCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
}

struct VSOutput
{
    float4 PositionH : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput MainVS(uint vertexID : SV_VertexID)
{
    VSOutput o;
    
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
	// [0,1] -> [-1,1] DX12 中的 NDC 规定：X 轴向右为正 $[-1, 1]$，Y 轴向上为正 $[-1, 1]$。（注意：纹理 UV 的 V 轴是向下的）。
    o.PositionH = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    
    return o;
}
