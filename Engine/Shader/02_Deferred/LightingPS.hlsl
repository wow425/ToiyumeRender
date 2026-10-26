// #pragma hlsl profile ps_6_6
// #pragma hlsl entry main

#include "../02_Deferred/DeferredCommon.hlsli"
#include "../02_Deferred/ColorPipeline.hlsli"


Texture2D<float4> GBuffer_BaseColor : register(t0);
Texture2D<float2> GBuffer_Normal : register(t1);
Texture2D<float4> GBuffer_Material : register(t2);
Texture2D<float4> GBuffer_Emission : register(t3);
Texture2D<float> DepthTexture : register(t4);

TextureCube<float4> IrradianceMap : register(t5);
TextureCube<float4> PrefilterMap : register(t6);
Texture2D<float2> BRDFLUT : register(t7);
SamplerState LinearClampSampler : register(s0);

cbuffer Light : register(b0)
{
    // xyz = Direction
    // w   = Directional Intensity
    float4 DirectionIntensity;

    // rgb = Directional Color
    // a   = unused
    float4 DirectionalColor;

    // xyz = Point Light Position
    // w   = Point Light Range
    float4 PointLightPositionRange;

    // rgb = Point Light Color
    // a   = Point Light Intensity
    float4 PointLightColorIntensity;

    // R3：Renderer::RenderParams 的 GPU 镜像（唯一 CPU 真源）
    // x = Exposure
    // y = EnableSunLight   (0/1)
    // z = EnablePointLight (0/1)
    // w = EnableTonemap    (0/1)
    float4 PostParams;

    // x = DebugView 通道（0=Final 1=BaseColor 2=Normal 3=Metallic 4=Roughness 5=AO 6=Emission 7=Depth）
    // y = EnableIBL (0/1)
    // z = IBLIntensity
    // w = unused
    float4 DebugParams;

    // R4：显示编码链参数（Renderer::RenderParams 的 GPU 镜像 #3）
    // x = EnableSRGBEncode (0/1)
    // y = PrefilterMaxMip   （= PrefilterCube 真实 mip 数 - 1，由 C++ 回填）
    // z/w = unused
    float4 DisplayParams;
};

// R1.5 / R1.7：必须与 C++ 的 Scene::Camera::CameraData 逐字段一致。
//   offset  0 : float4   cameraPos
//   offset 16 : float4x4 InViewProj
//   offset 80 : float2   RenderTargetSize
//   offset 88 : float2   InvRenderTargetSize
// 任何字段增删/改序都要同步 Camera.h，否则 UBO 会整体错位。
cbuffer Camera : register(b1)
{
    float4 cameraPos;
    float4x4 InViewProj;
    float2 RenderTargetSize;      // 当前渲染目标像素尺寸 (w, h)
    float2 InvRenderTargetSize;   // 1/w, 1/h
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
   return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// R4：ACES / sRGB 编码已统一到 ColorPipeline.hlsli。
// 本文件不再保留自己的副本 —— 一份实现才能保证 Lighting 与 Skybox 的输出在同一色彩空间。

float4 MainPS(float4 position : SV_POSITION) : SV_TARGET0
{ 
    uint2 pixel = uint2(position.xy);

    //-----------------------------------
    // GBuffer
    //-----------------------------------
    float4 baseColorAO = GBuffer_BaseColor.Load(int3(pixel, 0));
    float3 baseColor = baseColorAO.rgb;
    float ao = baseColorAO.a;

    float4 material = GBuffer_Material.Load(int3(pixel, 0));
	float metallic = saturate(material.r);
    float roughness = max(material.g, 0.04);
    float specular = material.b;

    float3 emission = GBuffer_Emission.Load(int3(pixel, 0)).rgb;

    float depth = DepthTexture.Load(int3(pixel, 0));

    float2 encNormal = GBuffer_Normal.Load(int3(pixel, 0));
    float3 normalWS = DecodeOctNormal(encNormal);

    //-----------------------------------
    // Position Reconstruction
    //-----------------------------------
    // R1.5：像素中心 → UV 必须用**真实**渲染目标尺寸。
    // 原先写死 float2(1920,1080)，只要窗口不是这个尺寸，
    // 反投影出的世界坐标就会整体偏移，直接导致光照与视线方向全错。
    float2 uv = (position.xy + 0.5) * InvRenderTargetSize;
    float3 positionWS = ReconstructWorldPosition(uv, depth, InViewProj);

    float3 V = normalize(cameraPos.xyz - positionWS);
    float3 N = normalize(normalWS);
    float3 F0 = 0.16 * specular * specular;
    F0 = lerp(F0, baseColor, metallic);
    //-----------------------------------
    // Directional Light
    //-----------------------------------
    float3 dirL = normalize(-DirectionIntensity.xyz);
    float3 dirRadiance = DirectionalColor.rgb * DirectionIntensity.w;

    float3 directionalLighting = EvaluateCookTorranceLight(
        N,
        V,
        dirL,
        baseColor,
        metallic,
        roughness,
        dirRadiance
    ) * PostParams.y; // R3：EnableSunLight（关掉时强度已为 0，这里再乘一次保证 UI 开关是硬开关）

    //-----------------------------------
    // Point Light
    //-----------------------------------
    float3 pointToLight = PointLightPositionRange.xyz - positionWS;
    float pointDistance = length(pointToLight);
    float3 pointL = pointToLight / max(pointDistance, 0.0001);

    float pointRange = PointLightPositionRange.w;
    float pointAttenuation = ComputePointAttenuation(pointDistance, pointRange);

    float3 pointRadiance = PointLightColorIntensity.rgb * PointLightColorIntensity.a * pointAttenuation;
        
    float3 pointLighting = EvaluateCookTorranceLight(
        N,
        V,
        pointL,
        baseColor,
        metallic,
        roughness,
        pointRadiance
    ) * PostParams.z; // R3：EnablePointLight

    //-----------------------------------
    // PBR IBL Ambient
    //-----------------------------------
    //float3 ambient = baseColor * 0.2 * ao; // 添加IBL后改为0.03
    // 尚未看
   float NdotV = saturate(dot(N, V));
   float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float3 irradiance = IrradianceMap.Sample(LinearClampSampler, N).rgb;
   float3 diffuseIBL = irradiance * baseColor;
    float3 R = reflect(-V, N);
   // R4：mip 上界来自 C++ 回填的真实值（PrefilterCube 有 5 个 mip -> 4）。
   // 原先写死 8.0，粗糙度 > 0.6 之后全部被硬件 clamp 到同一 mip，
   // 表现为"高光粗糙度响应在小 roughness 之外不再变化"。
   // max(...,1.0) 保证即使纹理只有 1 个 mip，采样也停在 mip 0 而不是退化成负值。
   float maxPrefilterMip = max(DisplayParams.y, 1.0);
  float3 prefilteredColor = PrefilterMap.SampleLevel(LinearClampSampler, R, roughness * maxPrefilterMip).rgb;
   float2 envBRDF = BRDFLUT.Sample(LinearClampSampler, float2(NdotV, roughness)).rg;
   float3 specularIBL = prefilteredColor * (F * envBRDF.x + envBRDF.y);
   // R3：EnableIBL / IBLIntensity 来自 RenderParams（DebugParams.yz）
   float3 ambient = (kD * diffuseIBL + specularIBL) * ao * DebugParams.y * DebugParams.z;


    //-----------------------------------
    // Final Color
    //-----------------------------------
    float3 hdrColor = ambient + directionalLighting + pointLighting + emission;

    //-----------------------------------
    // Debug View（R3，走 RenderParams.Debug，来源 ImGui 面板）
    //-----------------------------------
    // 放在曝光之前、色调映射之前：调试通道要显示的是缓冲区的原始数值，
    // 若先过曝光/ACES，数值会被压扁，看不出 GBuffer 的真实内容。
    // 这里刻意用 if 链而不是查找表，编译期即可完全展开成常量分支。
    uint debugView = (uint)(DebugParams.x + 0.5);
    if (debugView == 1)
    {
        return float4(baseColor, 1.0);          // BaseColor
    }
    if (debugView == 2)
    {
        return float4(normalWS * 0.5 + 0.5, 1.0); // Normal (世界空间)
    }
    if (debugView == 3)
    {
        return float4(metallic.xxx, 1.0);       // Metallic
    }
    if (debugView == 4)
    {
        return float4(roughness.xxx, 1.0);      // Roughness
    }
    if (debugView == 5)
    {
        return float4(ao.xxx, 1.0);             // AO
    }
    if (debugView == 6)
    {
        return float4(emission, 1.0);           // Emission
    }
    if (debugView == 7)
    {
        return float4(depth.xxx, 1.0);          // Depth
    }

    //-----------------------------------
    // Display 编码链（R4）
    //-----------------------------------
    // Exposure -> ToneMap -> sRGB 三步在 ColorPipeline.hlsli 中统一实现，
    // Skybox 走同一个函数。这样天空与模型不会各自处在一个"别的曝光/伽马空间"里。
    //   PostParams.x = Exposure, PostParams.w = EnableTonemap
    //   DisplayParams.x = EnableSRGBEncode
    float3 finalColor = EncodeDisplayColor(
        hdrColor,
        PostParams.x,
        PostParams.w > 0.5,
        DisplayParams.x > 0.5);

    return float4(finalColor, 1.0);
}