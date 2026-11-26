// ============================================================================
// 模块：ShadowDepthAlphaTestPS.hlsl（Shadow Map glTF MASK 镂空材质 AlphaTest 着色器）
// 架构层：Engine/Shader/03_Shadow（WP-3C）
// 约定与契约：
//   - 仅绑定给 kMaterial_AlphaTest（glTF alphaMode == MASK）网格，避免镂空树叶/蕾丝
//     投射实心方块阴影；普通不透明网格使用 PS=nullptr 纯深度路径，零像素着色开销。
//   - flags 高 16 位为 half 精度 alphaRef，与 ConstantBuffers.h / GBufferPS.hlsl 严格一致。
// ============================================================================
#pragma pack_matrix(row_major)

Texture2D<float4> baseColorTexture : register(t0);
SamplerState      baseColorSampler : register(s0);

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor;    // offset  0
    float3 emissiveFactor;     // offset 16
    float  normalTextureScale; // offset 28
    float  metallicFactor;     // offset 32
    float  roughnessFactor;    // offset 36
    uint   flags;              // offset 40 (bit 6 = alphaTest, bit 16..31 = half alphaRef)
};

#define MATFLAG_ALPHA_TEST (1u << 6)

struct ShadowVSOutput
{
    float4 PositionCS : SV_POSITION;
    float2 UV0        : TEXCOORD0;
};

void MainPS(ShadowVSOutput input)
{
    if ((flags & MATFLAG_ALPHA_TEST) != 0u)
    {
        float alpha = baseColorFactor.a * baseColorTexture.Sample(baseColorSampler, input.UV0).a;
        float alphaRef = f16tof32((flags >> 16) & 0xFFFFu);
        clip(alpha - alphaRef);
    }
}
