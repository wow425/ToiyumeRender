
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




// 属性语法。[Name(Argument)]。 提供关于xx的额外信息，此为根签名绑定该函数
[RootSignature(Renderer_RootSig)]
VSOutput MainVS(VSInput vsInput)
{
    VSOutput vsOutput;

    float3 normal = vsInput.normal; // 法线向量处理
#ifndef NO_TANGENT_FRAME
    float4 tangent = vsInput.tangent * 2 - 1;
#endif

    vsOutput.worldPos = mul(float4(vsInput.position, 1.0), WorldMatrix).xyz;
    vsOutput.position = mul(float4(vsOutput.worldPos, 1.0), ViewProjMatrix);

    vsOutput.normal = mul(normal, (float3x3) WorldIT);
#ifndef NO_TANGENT_FRAME
    vsOutput.tangent = float4(mul(vsInput.tangent.xyz, (float3x3) WorldIT), vsInput.tangent.w);
#endif
    vsOutput.uv0 = vsInput.uv0;


    return vsOutput;
}
