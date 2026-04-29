#include "Common.hlsli"

// 已校对cpp端结构体定义顺序
cbuffer MeshConstants : register(b0)
{
    float4x4 WorldMatrix; // Object to world
    float3x3 WorldIT; // Object normal to world normal
};

cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;
    float3 ViewerPos;
}



struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD; // 通常用于 反照率贴图 (Albedo/BaseColor)。它允许 重叠 (Overlap) 或 平铺 (Tiling)，以实现高精度纹理。

};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD0;

    float3 worldPos : TEXCOORD2;
};

// HLSL采用行向量，从左往右顺序，v * matrix

// 属性语法。[Name(Argument)]。 提供关于xx的额外信息，此为根签名绑定该函数
[RootSignature(Renderer_RootSig)]
VSOutput main(VSInput vsInput)
{
    VSOutput vsOutput;

    float3 normal = vsInput.normal; // [0,1] -> [-1,1]纹理采样的UV坐标通常在[0,1]范围内，而法线向量需要在[-1,1]范围内表示
#ifndef NO_TANGENT_FRAME
    float4 tangent = vsInput.tangent * 2 - 1;
#endif
    // object space -> world space
    vsOutput.worldPos = mul(float4(vsInput.position, 1.0), WorldMatrix).xyz;
    // world space -> clip space
    vsOutput.position = mul(float4(vsOutput.worldPos, 1.0), ViewProjMatrix);
    
    vsOutput.normal = mul(normal, WorldIT);
#ifndef NO_TANGENT_FRAME
    vsOutput.tangent = float4(mul(vsInput.tangent.xyz, WorldIT), vsInput.tangent.w);
#endif
    vsOutput.uv0 = vsInput.uv0;


    return vsOutput;
}
