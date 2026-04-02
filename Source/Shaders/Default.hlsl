

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
    float4x4 gjitteredViewProj;
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
    // TAA
    float4x4 gCleanViewProj; // 纯净vp，用于计算运动矢量
    float4x4 gPrevViewProj; // 纯净上一帧vp，用于计算运动矢量
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

struct PixelOut
{
    float4 Color : SV_Target0; // 输出到 gCurrentColor 纹理
    float2 Velocity : SV_Target1; // 输出到 gVelocity 纹理
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;
	MaterialData matData = gMaterialData[gMaterialIndex];
	
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3) gWorld);
    vout.TangentW = mul(vin.TangentU, (float3x3) gWorld);
    vout.PosH = mul(posW, gjitteredViewProj);
    
    vout.CurrPosH = mul(posW, gCleanViewProj); // 计算运动矢量必须用纯净的VP，避免抖动带来误差
    float4 prevPosW = mul(float4(vin.PosL, 1.0f), gPrevWorld);
    vout.PrevPosH = mul(prevPosW, gPrevViewProj);

    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    return vout;
}

PixelOut PS(VertexOut pin)
{
    PixelOut pout;

    MaterialData matData = gMaterialData[gMaterialIndex];
    float4 diffuseAlbedo = matData.DiffuseAlbedo;
    diffuseAlbedo *= gTextureMaps[matData.DiffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    pout.Color = diffuseAlbedo;

    // ==========================================
    // 2. 运动矢量计算 (Velocity)
    // ==========================================
    // 透视除法，转换到 NDC (Normalized Device Coordinates) 空间 [-1, 1]
    float2 currNDC = pin.CurrPosH.xy / pin.CurrPosH.w;
    float2 prevNDC = pin.PrevPosH.xy / pin.PrevPosH.w;
    
    // Velocity = CurrUV - PrevUV 计算 NDC 差值并缩放
    pout.Velocity.x = (currNDC.x - prevNDC.x) * 0.5f;
    pout.Velocity.y = -(currNDC.y - prevNDC.y) * 0.5f; // 注意负号翻转 Y 轴

    return pout;
}


