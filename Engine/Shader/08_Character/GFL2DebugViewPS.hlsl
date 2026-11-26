// ============================================================================
// 着色器：GFL2DebugViewPS.hlsl（M2 全屏 X 光机覆盖 Pass：真实硬件 8-bit Stencil 等可视化）
// 编译配置：compile_shader_bindless (ps_6_6, MainPS)
// ============================================================================
#include "08_Character/CharacterCommon.hlsli"

float4 MainPS(float4 position : SV_POSITION) : SV_TARGET0
{
    const uint2 pixel = uint2(position.xy);

    // g_AuxTex0SrvIdx = SceneDepth Stencil SRV (DXGI_FORMAT_X32_TYPELESS_G8X24_UINT -> 通道 .g)
    // g_AuxTex1SrvIdx = SceneDepth Depth SRV   (DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS -> 通道 .r)
    Texture2D<uint2> stencilTex = ResourceDescriptorHeap[g_AuxTex0SrvIdx];
    Texture2D<float> depthTex   = ResourceDescriptorHeap[g_AuxTex1SrvIdx];

    const uint  stencil = stencilTex.Load(int3(pixel, 0)).g;
    const float depth   = depthTex.Load(int3(pixel, 0)).r;

    if (g_PassSubMode == 9u) // DebugView::StencilMask
    {
        if (stencil == 0u)   return float4(0.00f, 0.00f, 0.00f, 1.0f); // 背景
        if (stencil == 200u) return float4(0.14f, 0.85f, 0.28f, 1.0f); // Body / Cloth / Weapon / Face
        if (stencil == 201u) return float4(0.96f, 0.86f, 0.14f, 1.0f); // Bangs Shadow (IncSat M4)
        if (stencil == 203u) return float4(0.14f, 0.85f, 0.92f, 1.0f); // HairBack
        if (stencil == 204u) return float4(0.92f, 0.22f, 0.88f, 1.0f); // Eyebrow / EyeBase / Bangs
        return float4(0.95f, 0.95f, 0.95f, 1.0f);
    }

    // Fallback: Reverse-Z Depth 对比可视化
    if (depth <= 1e-5f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float linearZ = 0.10f / max(depth, 1e-5f);
    const float vis = saturate(1.0f - (linearZ - 0.5f) * 0.25f);
    return float4(vis, vis, vis, 1.0f);
}
