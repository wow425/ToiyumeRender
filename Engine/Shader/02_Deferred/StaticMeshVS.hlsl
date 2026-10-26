
// CPU端采用行向量，不转置，shader端也采用行向量写法, 矩阵使用row_major,都用左乘
//
//
//

#include "../02_Deferred/DeferredCommon.hlsli"

cbuffer MeshConstants : register(b0)
{
    float4x4 WorldMatrix; // Object to world
    float4x4 WorldIT; // Object normal to world normal
};

// R1.7：必须与 C++ 的 GlobalConstants 逐字段一致（见 ConstantBuffers.h）。
// 原先这里写成 float3 ViewerPos，而 C++ 侧是 Matrix4 + 3 个 Vector3，
// 名称与布局都对不上；虽然当时只用到 ViewProjMatrix（偏移 0）所以没爆，
// 但一旦要新增字段就会立刻错位。现统一为 16 字节对齐的全量布局。
cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;   // offset  0
    float4   CameraPos;        // offset 64
    float4   SunDirection;     // offset 80
    float4   SunIntensity;     // offset 96
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

VSOutput MainVS(MeshVertex vsInput)
{
    VSOutput vsOutput;

    // ============================================
    // UNORM 解码（R1.4）
    // --------------------------------------------
    // 烘焙端用 VBWriter 的 x2bias=true 把 [-1,1] 映射到 [0,1]，
    // 再存进 R10G10B10A2_UNORM（MeshConvert.cpp 里 Write(..., true)）。
    // 因此运行时必须先 *2-1 还原，才能得到真正的向量。
    //
    // 原实现有两个缺陷：
    //  1) TangentWS 直接用未还原的 vsInput.Tangent.xyz（仍是 [0,1]），
    //     只有 Normal 做了还原 —— TBN 的切线基完全错误，
    //     法线贴图方向随之错乱。
    //  2) 还原后的局部变量 Tangent 算了却没被使用（死代码）。
    // ============================================
    float3 Normal      = vsInput.Normal * 2.0 - 1.0;       // UNORM -> [-1,1]
    float3 Tangent     = vsInput.Tangent.xyz * 2.0 - 1.0;  // UNORM → [-1,1]
    float  TangentSign = vsInput.Tangent.w * 2.0 - 1.0;    // 手性：0/1 → -1/+1

    vsOutput.PositionWS = mul(float4(vsInput.Position, 1.0), WorldMatrix).xyz;
    vsOutput.PositionCS = mul(float4(vsOutput.PositionWS, 1.0), ViewProjMatrix);

    // 法线与切线都用 WorldIT（逆转置）变换，非均匀缩放下才正确
    vsOutput.NormalWS  = normalize(mul(Normal,  (float3x3) WorldIT));
    vsOutput.TangentWS = float4(normalize(mul(Tangent, (float3x3) WorldIT)), TangentSign);
    vsOutput.UV0 = vsInput.UV0;


    return vsOutput;
}
