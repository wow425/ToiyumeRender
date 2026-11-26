// ============================================================================
// 着色器：SpecularPrefilterCS.hlsl（IBL 镜面预滤波环境立方体贴图烘焙）
// 坐标系：右手系、行主序矩阵；输入/输出色彩空间：Linear HDR (RGBA16F)
// 根签名：EnvironmentLightingManager::m_PrefilterRootSig（SM 6.4 传统描述符表）
//   - t0 : TextureCube<float4> gEnvironmentCube（源环境立方体贴图）
//   - u0 : RWTexture2DArray<float4> gPrefilterCube（目标预滤波贴图当前 mip 的 6 个面）
//   - b0 : SpecularPrefilterCB（分离源/目标尺寸，严格 32 字节 = 16B 对齐）
//   - s0 : LinearClampSampler
// ============================================================================
#include "EnvironmentCommon.hlsli"

TextureCube<float4> gEnvironmentCube : register(t0);
RWTexture2DArray<float4> gPrefilterCube : register(u0);
SamplerState gLinearSampler : register(s0);

// [TD-06b] 与 EnvironmentLightingManager.cpp::SpecularPrefilterCB 逐字段严格对齐（32 B）
cbuffer SpecularPrefilterCB : register(b0)
{
    uint   gDstSize;      // offset  0: 当前目标预滤波 mip 的单面像素边长
    uint   gSrcSize;      // offset  4: 源环境立方体贴图（mip 0）的单面像素边长
    float  gRoughness;    // offset  8: 目标粗糙度 [0, 1]
    uint   gSampleCount;  // offset 12: GGX 重要性采样数
    float  gSrcMaxMip;    // offset 16: 源环境贴图最高有效 mip 下标（仅写 mip0 时为 0.0）
    float3 _pad0;         // offset 20..31: 16 字节对齐填充
};

// GGX NDF（含分母保护，防止 roughness 极小且 NdotH==1 时产生 0/0 NaN）
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;

    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = max(PI * denom * denom, 1e-6);

    return a2 / denom;
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= gDstSize || dispatchThreadID.y >= gDstSize || dispatchThreadID.z >= 6)
        return;

    float3 N = CubeFaceTexelToDirection(dispatchThreadID.z, dispatchThreadID.xy, gDstSize);
    float3 R = N;
    float3 V = R;

    float3 prefilteredColor = 0.0;
    float totalWeight = 0.0;

    uint sampleCount = max(gSampleCount, 1u);
    // [TD-06b] 粗糙度下限保护：mip 0 传入 gRoughness==0 时，若直接送入 DistributionGGX 会算出
    // D=0 -> pdf=0 -> saSample=+INF -> log2(+INF)=+INF，引发 NaN/黑斑。
    float safeRoughness = max(gRoughness, 0.04);
    float srcResolution = max((float)gSrcSize, 1.0);
    float saTexel = 4.0 * PI / (6.0 * srcResolution * srcResolution);

    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 Xi = Hammersley(i, sampleCount);
        float3 H = ImportanceSampleGGX(Xi, N, safeRoughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = saturate(dot(N, L));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0)
        {
            float D = DistributionGGX(N, H, safeRoughness);
            float pdf = max((D * NdotH) / max(4.0 * VdotH, 1e-4), 1e-4);

            // [TD-06b / 消除无符号下溢与黑 mip 采样]
            // 原代码写了 `clamp(mip, 0.0, gMipLevel - 1)` 且使用目标 gSize 计算 saTexel：
            //   1) mip 0 时 uint(0) - 1 下溢为 0xFFFFFFFF，采到源 Cube 未写入的黑 mip；
            //   2) 源 EnvironmentCubeMap 仅烘焙了 mip 0，此时 gSrcMaxMip == 0.0 固定采 mip 0；
            //   3) 若未来为源 Cube 生成完整 mip 链，则 gSrcMaxMip 传入 (envMip - 1) 即可无缝启用滤波采样。
            float mip = 0.0;
            if (gRoughness > 0.001 && gSrcMaxMip > 0.0)
            {
                float saSample = 1.0 / (float(sampleCount) * pdf);
                mip = 0.5 * log2(max(saSample / saTexel, 1e-6));
                mip = clamp(mip, 0.0, gSrcMaxMip);
            }

            float3 color = gEnvironmentCube.SampleLevel(gLinearSampler, L, mip).rgb;
            // 防御性滤除源 HDR 中可能存在的极端 NaN/Inf 坏纹素
            if (all(isfinite(color)))
            {
                prefilteredColor += color * NdotL;
                totalWeight += NdotL;
            }
        }
    }

    prefilteredColor /= max(totalWeight, 1e-4);
    gPrefilterCube[dispatchThreadID] = float4(prefilteredColor, 1.0);
}
