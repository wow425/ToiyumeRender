// ============================================================================
// 模块：SSAOComputeCS.hlsl（SM 6.6 Bindless 半球屏幕空间环境光遮蔽计算着色器）
// 架构层：Engine/Shader/04_PostProcess（WP-3C · 契约 7.1）
// 约定与契约：
//   - 坐标系：右手系（视空间前向为 -Z：物体越靠近相机，viewPos.z 越大 / 越不负）；
//   - 根签名：RenderServices::GetBindlessComputeRootSignature()（20/64 DWORD）：
//       RootParam[0] = b0 : 4 uint 根常量（DepthSRV / NormalSRV / OutputUAV / SampleCount）
//       RootParam[2] = b2 : SSAOPass::ComputeConstants（224B，16B 对齐）
//   - 算法：基于低差异 Fibonacci 半球采样 + Interleaved Gradient Noise (IGN) 每像素旋转
//           （零额外噪声纹理，高频蓝噪特性极易被下游 5x5 双边模糊抹平）+ 距离范围衰减防黑边。
// ============================================================================
#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"

cbuffer BindlessSsaoIndices : register(b0)
{
    uint g_DepthSrvIdx;  // DWORD 0
    uint g_NormalSrvIdx; // DWORD 1
    uint g_OutputUavIdx; // DWORD 2
    uint g_SampleCount;  // DWORD 3
};

cbuffer SsaoComputeCB : register(b2)
{
    float4x4 g_InvViewProj;   // offset   0
    float4x4 g_ViewMatrix;    // offset  64
    float4x4 g_ProjMatrix;    // offset 128
    float2   g_ScreenSize;    // offset 192
    float2   g_InvScreenSize; // offset 200
    float    g_Radius;        // offset 208
    float    g_Bias;          // offset 212
    float    g_Power;         // offset 216
    uint     g_FrameIndex;    // offset 220
};

// Jorge Jimenez Interleaved Gradient Noise：屏幕空间高频均匀分布，无周期网格伪影
float InterleavedGradientNoise(float2 pixelCoord)
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(pixelCoord, magic.xy)));
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dtid : SV_DispatchThreadID)
{
    uint width  = (uint)(g_ScreenSize.x + 0.5f);
    uint height = (uint)(g_ScreenSize.y + 0.5f);
    if (dtid.x >= width || dtid.y >= height)
        return;

    Texture2D<float>    depthTex  = LoadBindless<Texture2D<float> >(g_DepthSrvIdx);
    Texture2D<float2>   normalTex = LoadBindless<Texture2D<float2> >(g_NormalSrvIdx);
    RWTexture2D<float>  outAoTex  = LoadBindless<RWTexture2D<float> >(g_OutputUavIdx);

    float depth = depthTex.Load(int3(dtid.xy, 0));
    // 天空盒/背景像素不做 SSAO，直接输出无遮蔽 1.0
    if (depth >= 0.99999f)
    {
        outAoTex[dtid.xy] = 1.0f;
        return;
    }

    float2 uv    = (float2(dtid.xy) + 0.5f) * g_InvScreenSize;
    float3 posWS = ReconstructWorldPosition(uv, depth, g_InvViewProj);
    float3 posVS = mul(float4(posWS, 1.0f), g_ViewMatrix).xyz;

    float2 encNormal = normalTex.Load(int3(dtid.xy, 0));
    float3 normalWS  = DecodeOctNormal(encNormal);
    float3 normalVS  = normalize(mul(normalWS, (float3x3)g_ViewMatrix));

    // 围绕 normalVS 构造随机旋转切线基（IGN 产生每像素旋转角）
    float rotAngle = InterleavedGradientNoise(float2(dtid.xy)) * (2.0f * kPBR_PI);
    float3 upRef   = (abs(normalVS.z) < 0.99f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 baseT   = normalize(cross(upRef, normalVS));
    float3 baseB   = cross(normalVS, baseT);

    float c = cos(rotAngle);
    float s = sin(rotAngle);
    float3 T = baseT * c + baseB * s;
    float3 B = -baseT * s + baseB * c;
    float3x3 TBN = float3x3(T, B, normalVS);

    uint sampleCount = clamp(g_SampleCount, 4u, 32u);
    float occlusion = 0.0f;
    float validSamples = 0.0f;

    // Golden-angle Fibonacci 半球采样（径向二次加速聚集于近处以突出接触阴影）
    const float kGoldenAngle = 2.39996323f;
    for (uint i = 0u; i < sampleCount; ++i)
    {
        float t = ((float)i + 0.5f) / (float)sampleCount;
        float phi = (float)i * kGoldenAngle;
        float cosTheta = sqrt(1.0f - t * 0.92f); // 保持在法线切平面以上，避免掠射自遮挡
        float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

        // 径向分布：近处更密集，远处稀疏
        float radialScale = lerp(0.12f, 1.0f, t * t);
        float3 hemiLocal = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta) * radialScale;
        float3 samplePosVS = posVS + mul(hemiLocal, TBN) * g_Radius;

        // 将视空间采样点投影回屏幕 UV
        float4 sampleClip = mul(float4(samplePosVS, 1.0f), g_ProjMatrix);
        if (sampleClip.w <= 1e-5f)
            continue;
        float2 sampleNDC = sampleClip.xy / sampleClip.w;
        float2 sampleUV  = float2(sampleNDC.x * 0.5f + 0.5f, 0.5f - sampleNDC.y * 0.5f);
        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
            continue;

        float sampleDepth = depthTex.SampleLevel(g_SampPointClamp, sampleUV, 0);
        if (sampleDepth >= 0.99999f)
        {
            validSamples += 1.0f;
            continue;
        }

        float3 occPosWS = ReconstructWorldPosition(sampleUV, sampleDepth, g_InvViewProj);
        float3 occPosVS = mul(float4(occPosWS, 1.0f), g_ViewMatrix).xyz;

        // [右手系视空间 Z 不变量] 相机朝向 -Z，故更靠近相机的几何体具有更大的 z（更不负）：
        // 若深度缓冲中重建出的表面 occPosVS.z >= samplePosVS.z + g_Bias，说明该采样点被前景遮挡！
        float depthDiff  = abs(posVS.z - occPosVS.z);
        float rangeCheck = smoothstep(0.0f, 1.0f, g_Radius / max(depthDiff, 1e-4f));
        float isOccluded = (occPosVS.z >= samplePosVS.z + g_Bias) ? 1.0f : 0.0f;

        occlusion += isOccluded * rangeCheck;
        validSamples += 1.0f;
    }

    float normOcclusion = (validSamples > 0.0f) ? (occlusion / validSamples) : 0.0f;
    float ao = pow(saturate(1.0f - normOcclusion), max(g_Power, 0.1f));
    outAoTex[dtid.xy] = ao;
}
