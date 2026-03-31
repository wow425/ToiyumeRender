#define PI 3.14159265359
#define NUM_DIR_LIGHTS 3
// 定义支持的最大贴图数量（根据你的 DX12 根签名/描述符表设置）
#define MAX_TEXTURES 10

#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "BRDFHelp.hlsl"

// ==========================================
// 数据结构与资源绑定
// ==========================================
struct Light
{
    float3 Strength; // 光源辐射度 (Radiance)
    float3 Direction; // 光源照射方向
};


struct MaterialData
{
    float Metallic; // 金属度
    float Roughness; // 粗糙度
    uint BaseColorIndex; // 基础色贴图在数组中的索引
    uint NormalMapIndex; // 预留：法线贴图索引
    float3 Emissive; // 自发光
    uint MatPad; // 内存对齐填充
};



// --- 资源定义 ---
// 1. 材质数组 (Structured Buffer)
StructuredBuffer<MaterialData> MaterialBuffer : register(t0);
// 2. 贴图数组 (Array of Textures)
Texture2D BaseColorMaps[MAX_TEXTURES] : register(t0, space1);
// 3. IBL 环境贴图
TextureCube IrradianceMap : register(t1);
TextureCube PrefilterMap : register(t2, space1);
Texture2D brdfLUT : register(t3);
// 4. 灯光
StructuredBuffer<Light> Lights : register(t3, space1);

// 采样器
SamplerState gsamLinearClamp : register(s0);
SamplerState brdfSampler : register(s1);




// 传递当前 DrawCall 或 Instance 的索引
cbuffer cbPerObject : register(b0)
{
    float4x4 World;
    float4x4 TexTransform;
    uint MaterialIndex; // 当前物体使用的材质索引
    uint gObjPad0;
    uint gObjPad1;
    uint gObjPad2;
}

// 每帧常量数据
cbuffer cbPass : register(b1)
{
    float4x4 View;
    float4x4 InvView;
    float4x4 Proj;
    float4x4 InvProj;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 ViewProjTex;
    float4x4 ShadowTransform;
    float3 EyePosW;
    float cbPerObjectPad1;
    float2 RenderTargetSize;
    float2 InvRenderTargetSize;
    float NearZ;
    float FarZ;
    float TotalTime;
    float DeltaTime;
    float4 AmbientLight;

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float3 TangentU : TAGENT;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD; // 采集贴图所需的 UV
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut) 0.0f;
    

    // 变换到世界空间
    float4 posW = mul(float4(vin.PosL, 1.0f), World);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3) World);
    vout.TangentW = mul(vin.TangentU, (float3x3) World);
    // 变换到齐次空间
    vout.PosH = mul(posW, ViewProj);
    // 纹理插值
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), TexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
   // --- 1. 获取动态材质数据 ---
    MaterialData mat = MaterialBuffer[MaterialIndex];
    float metallic = mat.Metallic;
    float roughness = max(mat.Roughness, 0.04);
    uint texIndex = mat.BaseColorIndex;
    
    // --- 2. 从贴图数组中动态采样 ---
    // [极其重要] 在 DX12 (Shader Model 5.1+) 中，如果 texIndex 在不同的像素/波前中不一样，
    // 必须套上 NonUniformResourceIndex()，否则会导致 GPU 硬件级别的采样错误。 放弃标量优化
    float4 sampledColor = BaseColorMaps[NonUniformResourceIndex(texIndex)].Sample(gsamLinearClamp, pin.TexC);

    // 线性化(sRGB到Linear) Gamma 编码，也就是 sRGB 空间,非线性。Gamma 解码
    float3 albedo = pow(sampledColor.rgb, 2.2);
    
    // --- 3. 基础 PBR 向量与能量分配 ---
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);
    
    float3 N = normalize(pin.NormalW); // 物体法线
    float3 V = normalize(EyePosW - pin.PosW); // 视线向量 (从表面指向眼睛)
    float3 DirectLightAccum = float3(0.0, 0.0, 0.0); // 光照累加
    float NdotV = max(dot(N, V), 0.0001); // 法线与视线的夹角余弦
    
    // ==========================================
    // 第一部分：直接光照 (Direct Lighting)
    // ==========================================
    for (int i = 0; i < NUM_DIR_LIGHTS; i++)
    {
        float3 L = normalize(-Lights[i].Direction); // 朝向光线的向量
        float3 H = normalize(V + L); // 半程向量
        float NdotL = max(dot(N, L), 0.0); // 法线与光线的夹角余弦
        
        if (NdotL > 0.0) // 正面才计算
        {
            float D = DistributionGGX(N, H, roughness); 
            float G = GeometrySmith(N, V, L, roughness, false);
            float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
            
            // 镜面反射BRDF
            float3 specularBRDF = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);
            
            // 漫反射BRDF
            
            // F 是菲涅尔项，代表有多少比例的光被"直接弹走"了 (Specular)
            // 1.0 - F 就是剩下的光。
            // (1.0 - metallic) 是因为：纯金属的自由电子会瞬间吸收所有进入内部的光子并转化为热能。
            float3 kD = (1.0 - F) * (1.0 - metallic); 
            float3 diffuseBRDF = kD * albedo / PI;
            
            // 累加这盏灯的贡献
            DirectLightAccum += (diffuseBRDF + specularBRDF) * Lights[i].Strength * NdotL;
        }
    }
    
    // ==========================================
    // 第二部分：间接光照 (IBL) 看不懂，先搁置
    // ==========================================
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = (1.0 - kS_IBL) * (1.0 - metallic);
    
    // 漫反射 IBL
    float3 irradiance = IrradianceMap.Sample(gsamLinearClamp, N).rgb;
    float3 indirectDiffuse = irradiance * albedo * kD_IBL;

    // 镜面反射 IBL
    float3 R = reflect(-V, N);
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = PrefilterMap.SampleLevel(gsamLinearClamp, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 envBRDF = brdfLUT.Sample(brdfSampler, float2(NdotV, roughness)).rg;
    float3 indirectSpecular = prefilteredColor * (F_IBL * envBRDF.x + envBRDF.y);

    float3 AmbientLight = indirectDiffuse + indirectSpecular;
    
    // ==========================================
    // 第三部分：总和与输出
    // ==========================================
    float3 finalColor = mat.Emissive + DirectLightAccum + AmbientLight;

    return float4(finalColor, sampledColor.a);

}

