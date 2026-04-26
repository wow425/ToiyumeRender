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
#ifndef NO_SECOND_UV
    float2 uv1 : TEXCOORD1; // 通常用于 光照贴图 (Lightmap / ライトマップ) 或 烘培遮蔽 (Baked AO)。它要求每个三角形在 UV 空间中必须是 唯一且不重叠的 (Unique & Non-overlapping)。
#endif
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD0;
#ifndef NO_SECOND_UV
    float2 uv1 : TEXCOORD1;
#endif
    float3 worldPos : TEXCOORD2;
};

// 属性语法。[Name(Argument)]。 提供关于xx的额外信息，此为根签名绑定该函数
[RootSignature(Renderer_RootSig)]
VSOutput main(VSInput vsInput)
{
    VSOutput vsOutput;

    float4 position = float4(vsInput.position, 1.0);
    float3 normal = vsInput.normal * 2 - 1; // [0,1] -> [-1,1]
#ifndef NO_TANGENT_FRAME
    float4 tangent = vsInput.tangent * 2 - 1;
#endif

    vsOutput.worldPos = mul(WorldMatrix, position).xyz;
    vsOutput.position = mul(ViewProjMatrix, float4(vsOutput.worldPos, 1.0));
    vsOutput.normal = mul(WorldIT, normal);
#ifndef NO_TANGENT_FRAME
    vsOutput.tangent = float4(mul(WorldIT, tangent.xyz), tagent.w);
#endif
    vsOutput.uv0 = vsInput.uv0;
#ifndef NO_SECOND_UV
    vsOutput.uv1 = vsInput.uv1;
#endif

    return vsOutput;
}
