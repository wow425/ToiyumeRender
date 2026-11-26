// ============================================================================
// 模块：ShadowDepthVS.hlsl（方向光 Shadow Map / CSM 深度投射顶点着色器）
// 架构层：Engine/Shader/03_Shadow（WP-3C）
// 约定与契约：
//   - 坐标系：右手系，行向量左乘（#pragma pack_matrix(row_major)）；
//   - 根签名：ShadowPass 专用传统根签名（与 RasterDrawHelper 契约兼容）：
//       RootParam[0] = b0 (MeshConstants : WorldMatrix / WorldIT，由 RasterDrawHelper 自动绑定)
//       RootParam[1] = b1 (ShadowViewConstants : 当前级联 LightViewProj，64B)
//   - 不变量：仅输出裁剪空间位置与 UV0（供 glTF MASK 材质 AlphaTest 使用）；
//             不透明物体直接使用 PS=nullptr 走硬件纯深度光栅化双倍填充率路径。
// ============================================================================
#pragma pack_matrix(row_major)

cbuffer MeshConstants : register(b0)
{
    float4x4 WorldMatrix; // offset  0：Object -> World
    float4x4 WorldIT;     // offset 64：法线逆转置（本 Pass 不使用，保持与 ConstantBuffers.h 对齐）
};

cbuffer ShadowViewConstants : register(b1)
{
    float4x4 LightViewProj; // offset 0：World -> Light Clip Space (NDC z ∈ [0, 1])
};

struct ShadowVertexInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Tangent  : TANGENT;
    float2 UV0      : TEXCOORD0;
};

struct ShadowVSOutput
{
    float4 PositionCS : SV_POSITION;
    float2 UV0        : TEXCOORD0;
};

ShadowVSOutput MainVS(ShadowVertexInput input)
{
    ShadowVSOutput output;
    float4 worldPos   = mul(float4(input.Position, 1.0f), WorldMatrix);
    output.PositionCS = mul(worldPos, LightViewProj);
    output.UV0        = input.UV0;
    return output;
}
