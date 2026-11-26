// ============================================================================
// 模块：LightingBindlessPS.hlsl（SM 6.6 Bindless 延迟光照着色器 · 集成 Shadow Map / CSM + SSAO）
// 架构层：Engine/Shader/02_Deferred（WP-3C · 契约 4 + 契约 7.1）
// 约定与契约：
//   - 坐标系：右手系、行向量左乘（#pragma pack_matrix(row_major)）；
//   - 色彩空间：输出 Linear HDR（R16G16B16A16_FLOAT），统一由下游 TonemapPass 编码；
//   - 根签名：全局共享 RenderServices::GetBindlessGraphicsRootSignature()（20/64 DWORD）：
//       RootParam[0] = b0 : 12 uint 根常量（GBuffer 0..3 / Depth / IBL 3 贴图 / ShadowMap / SSAOMap 下标）
//       RootParam[1] = b1 : Scene::LightingSystem::m_LightGPUBuffer（112B，与 LightingPS 100% 同构）
//       RootParam[2] = b2 : LightingPass::ExtraConstants（464B：相机 + View + 4 级联 LightViewProj + 参数）
//       StaticSamplers    : s1 = LinearClamp, s4 = ShadowComparison (LESS_EQUAL, Linear PCF)
//   - 回归不变量：当 CascadeCount==0 且 SSAOEnabled==0 时，所有 PBR + IBL 表达式与
//                 LightingPS.hlsl 逐位一致，保证关闭态 Golden RMSE == 0.000000。
// ============================================================================
#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"
#include "02_Deferred/ColorPipeline.hlsli"

// RootParam[0]：波前一致的 Bindless 资源下标（全部经 Zone B 瞬态暂存后压入 b0 根常量）
cbuffer BindlessLightingIndices : register(b0)
{
    uint g_GBufferBaseColorIdx; // DWORD 0
    uint g_GBufferNormalIdx;    // DWORD 1
    uint g_GBufferMaterialIdx;  // DWORD 2
    uint g_GBufferEmissionIdx;  // DWORD 3
    uint g_DepthIdx;            // DWORD 4
    uint g_IrradianceIdx;       // DWORD 5
    uint g_PrefilterIdx;        // DWORD 6
    uint g_BrdfLutIdx;          // DWORD 7
    uint g_ShadowMapIdx;        // DWORD 8 (kInvalidBindlessIndex = Shadow 关闭)
    uint g_SsaoMapIdx;          // DWORD 9 (kInvalidBindlessIndex = SSAO 关闭)
    uint g_PadIdx0;             // DWORD 10
    uint g_PadIdx1;             // DWORD 11
};

// RootParam[1]：与 Scene::LightingSystem::LightData（112B）严格逐字段同构
cbuffer Light : register(b1)
{
    float4 DirectionIntensity;       // offset  0
    float4 DirectionalColor;         // offset 16
    float4 PointLightPositionRange;  // offset 32
    float4 PointLightColorIntensity; // offset 48
    float4 PostParams;               // offset 64: x=Exposure y=EnableSun z=EnablePoint w=EnableTonemap
    float4 DebugParams;              // offset 80: x=DebugView y=EnableIBL z=IBLIntensity w=unused
    float4 DisplayParams;            // offset 96: x=EnableSRGBEncode y=PrefilterMaxMip z/w=unused
};

// RootParam[2]：与 LightingPass::ExtraConstants（464B，16B 对齐）逐字节对齐
cbuffer LightingExtraCB : register(b2)
{
    float4   cameraPos;            // offset   0
    float4x4 InViewProj;           // offset  16（开启 TAA 时为当前帧抖动逆矩阵）
    float2   RenderTargetSize;     // offset  80
    float2   InvRenderTargetSize;  // offset  88
    float4x4 ViewMatrix;           // offset  96（World -> View，用于 CSM 视空间深度分级）
    float4x4 LightViewProj[4];     // offset 160（最多 4 级联光源投影矩阵）
    float4   CascadeSplits;        // offset 416（每级联视空间远平面距离 > 0）
    float4   ShadowParams0;        // offset 432: x=CascadeCount, y=NormalBias, z=DepthBias, w=PCFRadius
    float4   ShadowParams1;        // offset 448: x=ShadowInvRes, y=ShadowStrength, z=SSAOEnabled, w=Unused
};

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ----------------------------------------------------------------------------
// 方向光 Shadow Map / CSM 3x3 硬件比较 PCF 采样
// - SimpleShadowMap (CascadeCount == 1) 与 CSM (CascadeCount == 2..4) 共用同一条
//   Texture2DArray<float> + SampleCmpLevelZero(g_SampShadowCmp) 代码路径。
// ----------------------------------------------------------------------------
float EvaluateDirectionalShadow(float3 positionWS, float3 normalWS)
{
    uint cascadeCount = (uint)(ShadowParams0.x + 0.5f);
    if (cascadeCount == 0u || g_ShadowMapIdx == kInvalidBindlessIndex)
        return 1.0f;

    // 1. 选择级联：右手系视空间朝向 -Z，故视空间正深度为 -viewPos.z
    uint cascadeIdx = 0u;
    if (cascadeCount > 1u)
    {
        float3 posVS = mul(float4(positionWS, 1.0f), ViewMatrix).xyz;
        float viewDepth = -posVS.z;
        cascadeIdx = cascadeCount - 1u;
        [unroll]
        for (uint c = 0u; c < 4u; ++c)
        {
            if (c < cascadeCount && viewDepth <= CascadeSplits[c])
            {
                cascadeIdx = c;
                break;
            }
        }
    }

    // 2. 沿世界空间法线抬升采样点（远级联 texel 更大，按级联序号温和放大法线偏移抑制 Acne）
    float normalBias = ShadowParams0.y * (1.0f + 0.35f * (float)cascadeIdx);
    float3 biasedPos = positionWS + normalWS * normalBias;

    float4 lightClip = mul(float4(biasedPos, 1.0f), LightViewProj[cascadeIdx]);
    if (abs(lightClip.w) < 1e-6f)
        return 1.0f;

    float3 lightNDC = lightClip.xyz / lightClip.w;
    // NDC [-1,1] -> D3D12 Texture UV [0,1]（Y 轴翻转：NDC +1 对应贴图顶边 v=0）
    float2 shadowUV = float2(lightNDC.x * 0.5f + 0.5f, 0.5f - lightNDC.y * 0.5f);

    // 超出阴影图视锥范围的区域默认不受遮挡（避免边缘拉丝）
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f) || lightNDC.z <= 0.0f || lightNDC.z >= 1.0f)
        return 1.0f;

    float compareDepth = saturate(lightNDC.z - ShadowParams0.z);
    float texelStep    = ShadowParams1.x * ShadowParams0.w;

    // SM 6.6 Bindless 直索引：g_ShadowMapIdx 来自 b0 根常量（波前一致）
    Texture2DArray<float> shadowArray = LoadBindless<Texture2DArray<float> >(g_ShadowMapIdx);

    float visibilitySum = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            float2 sampleUV = shadowUV + float2((float)dx, (float)dy) * texelStep;
            visibilitySum += shadowArray.SampleCmpLevelZero(
                g_SampShadowCmp,
                float3(sampleUV, (float)cascadeIdx),
                compareDepth);
        }
    }

    float visibility = visibilitySum * (1.0f / 9.0f);
    return lerp(1.0f, visibility, saturate(ShadowParams1.y));
}

float4 MainPS(float4 position : SV_POSITION) : SV_TARGET0
{
    uint2 pixel = uint2(position.xy);

    Texture2D<float4>   GBuffer_BaseColor = LoadBindless<Texture2D<float4> >(g_GBufferBaseColorIdx);
    Texture2D<float2>   GBuffer_Normal    = LoadBindless<Texture2D<float2> >(g_GBufferNormalIdx);
    Texture2D<float4>   GBuffer_Material  = LoadBindless<Texture2D<float4> >(g_GBufferMaterialIdx);
    Texture2D<float4>   GBuffer_Emission  = LoadBindless<Texture2D<float4> >(g_GBufferEmissionIdx);
    Texture2D<float>    DepthTexture      = LoadBindless<Texture2D<float> >(g_DepthIdx);
    TextureCube<float4> IrradianceMap     = LoadBindless<TextureCube<float4> >(g_IrradianceIdx);
    TextureCube<float4> PrefilterMap      = LoadBindless<TextureCube<float4> >(g_PrefilterIdx);
    Texture2D<float2>   BRDFLUT           = LoadBindless<Texture2D<float2> >(g_BrdfLutIdx);

    //-----------------------------------
    // GBuffer
    //-----------------------------------
    float4 baseColorAO = GBuffer_BaseColor.Load(int3(pixel, 0));
    float3 baseColor   = baseColorAO.rgb;
    float  ao          = baseColorAO.a;

    // [WP-3C] SSAO 调制：仅在启用 SSAO 时叠乘屏幕空间环境光遮蔽，关闭时零修改保证 Golden 0 差
    if (ShadowParams1.z > 0.5f && g_SsaoMapIdx != kInvalidBindlessIndex)
    {
        Texture2D<float> ssaoTex = LoadBindless<Texture2D<float> >(g_SsaoMapIdx);
        float ssaoFactor = ssaoTex.Load(int3(pixel, 0));
        ao *= ssaoFactor;
    }

    float4 material  = GBuffer_Material.Load(int3(pixel, 0));
    float  metallic  = saturate(material.r);
    float  roughness = max(material.g, 0.04);
    float  specular  = material.b;

    float3 emission  = GBuffer_Emission.Load(int3(pixel, 0)).rgb;
    float  depth     = DepthTexture.Load(int3(pixel, 0));
    float2 encNormal = GBuffer_Normal.Load(int3(pixel, 0));
    float3 normalWS  = DecodeOctNormal(encNormal);

    //-----------------------------------
    // Position Reconstruction (D3D12 SV_POSITION 像素中心不变量)
    //-----------------------------------
    float2 uv         = position.xy * InvRenderTargetSize;
    float3 positionWS = ReconstructWorldPosition(uv, depth, InViewProj);

    float3 V  = normalize(cameraPos.xyz - positionWS);
    float3 N  = normalize(normalWS);
    float3 F0 = 0.16 * specular * specular;
    F0 = lerp(F0, baseColor, metallic);

    //-----------------------------------
    // Directional Light + Shadow Map / CSM
    //-----------------------------------
    float3 dirL        = normalize(-DirectionIntensity.xyz);
    float3 dirRadiance = DirectionalColor.rgb * DirectionIntensity.w;

    float3 directionalLighting = EvaluateCookTorranceLight(
        N,
        V,
        dirL,
        baseColor,
        metallic,
        roughness,
        dirRadiance
    ) * PostParams.y;

    // [WP-3C] 仅在启用阴影且方向光开启时计算阴影因子（关闭时零乘算开销与舍入差异）
    if (ShadowParams0.x > 0.5f && PostParams.y > 0.5f && depth < 0.99999f)
    {
        float shadowFactor = EvaluateDirectionalShadow(positionWS, N);
        directionalLighting *= shadowFactor;
    }

    //-----------------------------------
    // Point Light
    //-----------------------------------
    float3 pointToLight    = PointLightPositionRange.xyz - positionWS;
    float  pointDistance   = length(pointToLight);
    float3 pointL          = pointToLight / max(pointDistance, 0.0001);
    float  pointRange      = PointLightPositionRange.w;
    float  pointAttenuation = ComputePointAttenuation(pointDistance, pointRange);
    float3 pointRadiance   = PointLightColorIntensity.rgb * PointLightColorIntensity.a * pointAttenuation;

    float3 pointLighting = EvaluateCookTorranceLight(
        N,
        V,
        pointL,
        baseColor,
        metallic,
        roughness,
        pointRadiance
    ) * PostParams.z;

    //-----------------------------------
    // PBR IBL Ambient (modulated by Material AO * SSAO)
    //-----------------------------------
    float  NdotV            = saturate(dot(N, V));
    float3 F                = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS               = F;
    float3 kD               = (1.0 - kS) * (1.0 - metallic);
    float3 irradiance       = IrradianceMap.Sample(g_SampLinearClamp, N).rgb;
    float3 diffuseIBL       = irradiance * baseColor;
    float3 R                = reflect(-V, N);
    float  maxPrefilterMip  = max(DisplayParams.y, 1.0);
    float3 prefilteredColor = PrefilterMap.SampleLevel(g_SampLinearClamp, R, roughness * maxPrefilterMip).rgb;
    float2 envBRDF          = BRDFLUT.Sample(g_SampLinearClamp, float2(NdotV, roughness)).rg;
    float3 specularIBL      = prefilteredColor * (F * envBRDF.x + envBRDF.y);
    float3 ambient          = (kD * diffuseIBL + specularIBL) * ao * DebugParams.y * DebugParams.z;

    //-----------------------------------
    // Final Color & Debug View
    //-----------------------------------
    float3 hdrColor = ambient + directionalLighting + pointLighting + emission;

    uint debugView = (uint)(DebugParams.x + 0.5);
    if (debugView == 1) return float4(baseColor, 1.0);
    if (debugView == 2) return float4(normalWS * 0.5 + 0.5, 1.0);
    if (debugView == 3) return float4(metallic.xxx, 1.0);
    if (debugView == 4) return float4(roughness.xxx, 1.0);
    if (debugView == 5) return float4(ao.xxx, 1.0);
    if (debugView == 6) return float4(emission, 1.0);
    if (debugView == 7) return float4(depth.xxx, 1.0);
    if (debugView >= 8) return float4(0.15, 0.05, 0.25, 1.0);

    return float4(hdrColor, 1.0);
}
