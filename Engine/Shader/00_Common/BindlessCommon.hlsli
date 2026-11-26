// ============================================================================
// 模块：BindlessCommon.hlsli（SM 6.6 Bindless 全局根签名与描述符直索引公共契约）
// 架构层：Engine/Shader/00_Common（契约 7.1 / WP-3A）
// 硬件与编译前提：
//   - 仅限 SM 6.6 + ResourceBindingTier >= Tier 3，使用 compile_shader_bindless
//     （-HV 2021 -enable-16bit-types）编译；
//   - 配套 RenderServices::GetBindlessGraphicsRootSignature() / ComputeRootSignature()：
//       RootParam[0] = b0 : 16 DWORD 根常量（Bindless 资源下标与高频小标量）
//       RootParam[1] = b1 : Per-Frame / Per-View 动态 CBV (2 DWORD)
//       RootParam[2] = b2 : Per-Pass 参数动态 CBV (2 DWORD)
//       StaticSamplers s0..s6 : PointClamp / LinearClamp / LinearWrap /
//                               AnisoWrap / ShadowComparison(LE) / PointWrap / ShadowCmpGreater(GE)
// DX12 屏障不变量：
//   启用 Bindless 后，Shader 可直接按下标索引 4 分区统一堆中任意槽位，但 CPU 侧仍
//   必须经 LinearPassList 完整声明 ResourceAccess 状态转换，绝不可省略屏障！
// ============================================================================
#ifndef TOOIYUME_BINDLESS_COMMON_HLSLI
#define TOOIYUME_BINDLESS_COMMON_HLSLI

// 4 分区统一堆常量（与 UnifiedDescriptorHeap.h 严格一致）
static const uint kInvalidBindlessIndex   = 0xFFFFFFFFu;
static const uint kSentinelWhite2D        = 0u;
static const uint kSentinelBlack2D        = 1u;
static const uint kSentinelFlatNormal2D   = 2u;
static const uint kSentinelZeroByteBuffer = 3u;

// 共享静态采样器寄存器映射（与 RenderServices BindlessRootSignature 严格一致）
SamplerState           g_SampPointClamp  : register(s0);
SamplerState           g_SampLinearClamp : register(s1);
SamplerState           g_SampLinearWrap  : register(s2);
SamplerState           g_SampAnisoWrap   : register(s3);
SamplerComparisonState g_SampShadowCmp        : register(s4);
SamplerState           g_SampPointWrap        : register(s5);
SamplerComparisonState g_SampShadowCmpGreater : register(s6);

// 波前一致索引（来自 b0 根常量或 b1/b2 cbuffer）：直接索引统一 Shader-Visible 堆
template<typename T>
T LoadBindless(uint idx)
{
    return ResourceDescriptorHeap[idx];
}

// 【波前发散索引铁律】
// 当 idx 在同一 Wave/Warp 内的不同像素/线程取值不同（如每像素材质贴图 ID、随机样本索引）时，
// 必须包裹 NonUniformResourceIndex()，否则硬件标量执行单元会产生未定义纹理错采或崩溃！
#define LOAD_BINDLESS_NONUNIFORM(Type, idx) ResourceDescriptorHeap[NonUniformResourceIndex(idx)]

#endif // TOOIYUME_BINDLESS_COMMON_HLSLI
