

struct MaterialData
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Roughness;
    float4x4 MatTransform;
    uint DiffuseMapIndex; // 纹理索引
    uint NormalMapIndex;
    uint MatPad1;
    uint MatPad2;
};


// SRV, t0-t9被用，t0 s1被用。
// t1-t4的s1被TAA的SRV占用(Current, History, Depth, Velocity).
// u0-u1的s1被TAA的UAV占用
Texture2D gTextureMaps[10] : register(t0); // SRV, 纹理数组，对纹理的大小和格式不做要求
StructuredBuffer<MaterialData> gMaterialData : register(t0, space1); // 材质


SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);


// 物体常量数据
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    uint gMaterialIndex;
    uint gObjPad0;
    uint gObjPad1;
    uint gObjPad2;
    // 【新增】物体上一帧的世界矩阵 (用于处理动态物体的运动)
    float4x4 gPrevWorld;
};

// 渲染过程常量数据
cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gViewProjTex;
    float4x4 gShadowTransform;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    // 【新增】上一帧的 ViewProj 矩阵 (用于处理摄像机的运动)
    // 强烈建议：算速度用的矩阵必须是【去除了 Jitter 抖动】的纯净矩阵，否则速度缓冲会被污染！
    float4x4 gPrevViewProj;
};

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
	float3 TangentU : TANGENT;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
	float3 TangentW : TANGENT;
	float2 TexC    : TEXCOORD;
    
    // 【新增】分别记录当前帧和上一帧的裁剪空间坐标
    float4 CurrPosH : POSITION3;
    float4 PrevPosH : POSITION4;
};

// 【新增】MRT 输出结构体
struct PixelOut
{
    float4 Color : SV_Target0; // 输出到 gCurrentColor 纹理
    float2 Velocity : SV_Target1; // 输出到 gVelocity 纹理
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;
	MaterialData matData = gMaterialData[gMaterialIndex];
	
// 1. 常规的当前帧位置计算
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3) gWorld);
    vout.TangentW = mul(vin.TangentU, (float3x3) gWorld);
    vout.PosH = mul(posW, gViewProj);
    
    // 【新增】保存当前帧未被光栅化修改的齐次裁剪坐标
    vout.CurrPosH = vout.PosH;

// 2. 【新增】计算上一帧的裁剪空间位置
    
    float4 prevPosW = mul(float4(vin.PosL, 1.0f), gPrevWorld);
    vout.PrevPosH = mul(prevPosW, gPrevViewProj);

// 3. 纹理 UV
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    return vout;
}

PixelOut PS(VertexOut pin)
{
    PixelOut pout;

    // ==========================================
    // 1. 常规颜色计算 (Color)
    // ==========================================
    MaterialData matData = gMaterialData[gMaterialIndex];
    float4 diffuseAlbedo = matData.DiffuseAlbedo;
    diffuseAlbedo *= gTextureMaps[matData.DiffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    
    // 省略光照计算，直接输出漫反射
    pout.Color = diffuseAlbedo;

// ==========================================
    // 2. 运动矢量计算 (Velocity)
    // ==========================================
    // 透视除法，转换到 NDC (Normalized Device Coordinates) 空间 [-1, 1]
    float2 currNDC = pin.CurrPosH.xy / pin.CurrPosH.w;
    float2 prevNDC = pin.PrevPosH.xy / pin.PrevPosH.w;

    // 将 NDC 坐标映射到 UV 空间 [0, 1]
    // UV 和 NDC 的关系：
    // U = NDC.x * 0.5 + 0.5
    // V = -NDC.y * 0.5 + 0.5 (注意 DirectX 中 V 轴向下，NDC Y轴向上)
    
    // Velocity = CurrUV - PrevUV
    float2 velocity;
    velocity.x = (currNDC.x - prevNDC.x) * 0.5f;
    velocity.y = -(currNDC.y - prevNDC.y) * 0.5f; // 注意负号翻转 Y 轴

    pout.Velocity = velocity;

    return pout;
}


