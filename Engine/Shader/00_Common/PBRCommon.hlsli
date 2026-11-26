// ============================================================================
// 模块：PBRCommon.hlsli（光栅与 DXR 共享 PBR 基础数学函数）
// 架构层：Engine/Shader/00_Common（WP-2B / TD-02c）
// 核心职责：统一光栅延迟着色（DeferredCommon.hlsli）与光追着色（RayTracing.hlsl）共用
//           的 PBR 常量与 FresnelSchlick 公共原语，消除双份拷贝与口径漂移。
// 色彩空间约定：所有输入/输出辐射度与 F0 均为 Linear RGB 空间。
// ============================================================================
#pragma once

#ifndef __PBR_COMMON_HLSLI__
#define __PBR_COMMON_HLSLI__

static const float kPBR_PI = 3.14159265359f;

// [TD-06a / TD-02c] Schlick 菲涅尔近似（向量版，金属 F0 必须保持 RGB 三通道不截断）
// saturate(1.0 - cosTheta) 保护 pow() 底数非负，避免掠射角浮点误差产生 NaN。
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

#endif // __PBR_COMMON_HLSLI__
