// ============================================================================
// 模块：PrimarySurfaceCommon.hlsli（统一主可见性与表面几何 HLSL 解包契约）
// 架构层：Engine/Shader/00_Common（契约 4 / SM 6.6 Bindless）
// 坐标系与色彩约定：
//   - 右手系、行向量 mul(pos, Matrix)（#pragma pack_matrix(row_major)）；
//   - BaseColor / Emission 均为线性场景空间（Linear Scene-Referred RGB）；
//   - Normal 为世界空间单位法线，底层以 Octahedral [0,1] 编码存储于 R16G16_FLOAT；
//   - Depth 为 NDC z ∈ [0,1]，1.0（>= 0.999999）统一代表未命中/天空背景。
// 依赖根签名：
//   - RenderServices 全局共享 BindlessRootSignature（SM 6.6 + Tier 3）。
// 屏蔽上游差异：
//   - 无论上游是 Deferred GBufferPass 还是 DXR PrimaryRayPass，下游着色器仅需调用
//     LoadPrimarySurface(...) 即可用完全一致的公式解包表面属性与世界坐标。
// ============================================================================
#ifndef TOOIYUME_PRIMARY_SURFACE_COMMON_HLSLI
#define TOOIYUME_PRIMARY_SURFACE_COMMON_HLSLI

#pragma pack_matrix(row_major)

#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"

// 与 C++ 侧 Renderer::PrimarySurfaceBindlessIndices 逐字段对齐（32 字节 = 8 DWORD）
struct PrimarySurfaceBindlessIndices
{
    uint DepthSrvIdx;    // offset  0: Texture2D<float>
    uint AlbedoAoSrvIdx; // offset  4: Texture2D<float4>
    uint NormalSrvIdx;   // offset  8: Texture2D<float2>
    uint MaterialSrvIdx; // offset 12: Texture2D<float4>
    uint EmissionSrvIdx; // offset 16: Texture2D<float4> (可选，无效时为 kInvalidBindlessIndex)
    uint VelocitySrvIdx; // offset 20: Texture2D<float2> (可选，无效时为 kInvalidBindlessIndex)
    uint Width;          // offset 24
    uint Height;         // offset 28
};

// 下游着色器统一消费的主表面采样结果
struct PrimarySurfaceSample
{
    float  Depth;      // NDC z ∈ [0, 1]；>= 0.999999 代表背景/未命中
    float3 BaseColor;  // 线性 RGB 反照率
    float  AO;         // 材质环境光遮蔽 ∈ [0, 1]
    float3 NormalWS;   // 世界空间单位法线
    float  Metallic;   // 金属度 ∈ [0, 1]
    float  Roughness;  // 粗糙度 ∈ [0, 1]
    float  Specular;   // 高光默认 0.5
    float  MaterialID; // 材质槽位 ID
    float3 Emission;   // 线性自发光 RGB（无该通道时为 0）
    float2 Velocity;   // 屏幕空间运动矢量（无该通道时为 0）
    bool   ValidHit;   // true = 命中场景表面，false = 天空/背景
};

// 按整像素坐标零滤波精确读取统一主表面全部属性（Deferred / DXR 完全同构）
PrimarySurfaceSample LoadPrimarySurface(uint2 pixel, PrimarySurfaceBindlessIndices idx)
{
    PrimarySurfaceSample s;
    s.Depth      = 1.0f;
    s.BaseColor  = float3(0.0f, 0.0f, 0.0f);
    s.AO         = 1.0f;
    s.NormalWS   = float3(0.0f, 1.0f, 0.0f);
    s.Metallic   = 0.0f;
    s.Roughness  = 1.0f;
    s.Specular   = 0.5f;
    s.MaterialID = 0.0f;
    s.Emission   = float3(0.0f, 0.0f, 0.0f);
    s.Velocity   = float2(0.0f, 0.0f);
    s.ValidHit   = false;

    if (idx.DepthSrvIdx == kInvalidBindlessIndex ||
        idx.AlbedoAoSrvIdx == kInvalidBindlessIndex ||
        idx.NormalSrvIdx == kInvalidBindlessIndex ||
        idx.MaterialSrvIdx == kInvalidBindlessIndex)
    {
        return s;
    }

    const int3 coord = int3(pixel, 0);

    Texture2D<float>  depthTex    = LoadBindless<Texture2D<float> >(idx.DepthSrvIdx);
    const float depth = depthTex.Load(coord);
    s.Depth    = depth;
    s.ValidHit = (depth < 0.999999f);

    if (!s.ValidHit)
    {
        return s;
    }

    Texture2D<float4> albedoTex   = LoadBindless<Texture2D<float4> >(idx.AlbedoAoSrvIdx);
    Texture2D<float2> normalTex   = LoadBindless<Texture2D<float2> >(idx.NormalSrvIdx);
    Texture2D<float4> materialTex = LoadBindless<Texture2D<float4> >(idx.MaterialSrvIdx);

    const float4 albedoAo = albedoTex.Load(coord);
    const float2 octNorm  = normalTex.Load(coord);
    const float4 material = materialTex.Load(coord);

    s.BaseColor  = albedoAo.rgb;
    s.AO         = albedoAo.a;
    s.NormalWS   = DecodeOctNormal(octNorm);
    s.Metallic   = material.r;
    s.Roughness  = material.g;
    s.Specular   = material.b;
    s.MaterialID = material.a;

    if (idx.EmissionSrvIdx != kInvalidBindlessIndex)
    {
        Texture2D<float4> emissionTex = LoadBindless<Texture2D<float4> >(idx.EmissionSrvIdx);
        s.Emission = emissionTex.Load(coord).rgb;
    }

    if (idx.VelocitySrvIdx != kInvalidBindlessIndex)
    {
        Texture2D<float2> velocityTex = LoadBindless<Texture2D<float2> >(idx.VelocitySrvIdx);
        s.Velocity = velocityTex.Load(coord);
    }

    return s;
}

// 从统一主表面深度反投影世界空间位置（与 DeferredCommon.hlsli::ReconstructWorldPosition 完全同构）
float3 ReconstructPrimarySurfaceWorldPos(uint2 pixel, float depth, uint2 surfaceSize, float4x4 invViewProj)
{
    const float2 uv = (float2(pixel) + 0.5f) / float2(max(surfaceSize, uint2(1u, 1u)));
    return ReconstructWorldPosition(uv, depth, invViewProj);
}

#endif // TOOIYUME_PRIMARY_SURFACE_COMMON_HLSLI
