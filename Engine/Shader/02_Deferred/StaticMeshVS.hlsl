
// CPU端采用行向量，不转置，shader端也采用行向量写法, 矩阵使用row_major,都用左乘
//
//
//

#include "../00_Common/Common.hlsli"

cbuffer MeshConstants : register(b0)
{
	float4x4 WorldMatrix; // Object to world
	float4x4 WorldIT; // Object normal to world normal
};

cbuffer GlobalConstants : register(b1)
{
	float4x4 ViewProjMatrix;
	float3 ViewerPos;
}



struct MeshVertex
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 UV0 : TEXCOORD0; // 通常用于 反照率贴图 (Albedo/BaseColor)。它允许 重叠 (Overlap) 或 平铺 (Tiling)，以实现高精度纹理。
};

struct VSOutput
{
    float4 PositionCS : SV_POSITION; // Clip Space
    float3 PositionWS : POSITION_WS; // World Space
    float3 NormalWS : NORMAL_WS; // World Space
    float4 TangentWS : TANGENT_WS; // World Space
    float2 UV0 : TEXCOORD0;
};




// 属性语法。[Name(Argument)]。 提供关于xx的额外信息，此为根签名绑定该函数
    [RootSignature(Renderer_RootSig)]
VSOutput MainVS(MeshVertex vsInput)
{
    VSOutput vsOutput;

    float3 Normal = vsInput.Normal; // 法线向量处理
    float4 Tangent = vsInput.Tangent;

    vsOutput.PositionWS = mul(float4(vsInput.Position, 1.0), WorldMatrix).xyz;
    vsOutput.PositionCS = mul(float4(vsOutput.PositionWS, 1.0), ViewProjMatrix);

    vsOutput.NormalWS = normalize(mul(Normal, (float3x3) WorldIT));
    vsOutput.TangentWS = float4(normalize(mul(vsInput.Tangent.xyz, (float3x3) WorldIT)), vsInput.Tangent.w);
    vsOutput.UV0 = vsInput.UV0;


    return vsOutput;
}
