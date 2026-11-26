// ============================================================================
// 模块：PrimaryRayCS.hlsl（DXR 1.1 Inline RayQuery 主可见性与表面几何写入 Pass）
// 架构层：Engine/Shader/07_RayTracing（契约 4 / 统一 PrimarySurfaceDesc DXR 生产端）
// 编译配置：compile_shader_bindless (cs_6_6, -HV 2021 -enable-16bit-types)
// 依赖根签名：RenderServices::GetBindlessComputeRootSignature()（20/64 DWORD）：
//   - b0 (12/16 DWORD 根常量)：TLAS / SceneData / InstanceData SRV + 5 张 Surface UAV 下标 + 宽高开关
//   - b1 (Per-Frame CBV)      ：InvViewProj / ViewProj / CameraPos / OutputSize
//   - StaticSamplers s0..s5   ：材质采样使用 s3 (g_SampAnisoWrap)
// 数学与色彩约定：
//   - 右手系、行向量 mul(pos, Matrix)（#pragma pack_matrix(row_major)）；
//   - 深度输出与光栅 GBuffer 完全同构的 NDC z ∈ [0,1]（未命中写 1.0）；
//   - 法线使用 DeferredCommon.hlsli::EncodeOctNormal 写入 R16G16_FLOAT；
//   - 反照率与自发光均为线性场景空间，材质槽位采样按波前发散规律包裹 NonUniformResourceIndex。
// ============================================================================
#pragma pack_matrix(row_major)

#include "00_Common/BindlessCommon.hlsli"
#include "02_Deferred/DeferredCommon.hlsli"

// 每实例记录字段偏移（与 C++ RaytracingScene.h::RaytracingInstanceData 96B 布局严格一致）
#define INSTANCE_VERTEX_OFFSET       0u
#define INSTANCE_INDEX_OFFSET        4u
#define INSTANCE_VERTEX_STRIDE       8u
#define INSTANCE_INDEX_FORMAT       12u
#define INSTANCE_START_INDEX        16u
#define INSTANCE_BASE_VERTEX        20u
#define INSTANCE_PRIM_COUNT         24u
#define INSTANCE_ID                 28u
#define INSTANCE_UV_OFFSET          32u
#define INSTANCE_VERTEX_FLAGS       36u
#define INSTANCE_MATERIAL_INDEX     40u
#define INSTANCE_SRV_TABLE_OFFSET   44u
#define INSTANCE_BASECOLOR_FACTOR   48u
#define INSTANCE_EMISSIVE_FACTOR    64u
#define INSTANCE_NORMAL_SCALE       76u
#define INSTANCE_METALLIC           80u
#define INSTANCE_ROUGHNESS          84u
#define INSTANCE_MATERIAL_FLAGS     88u
#define INSTANCE_STRIDE_BYTES       96u

// 材质 5 槽贴图偏移（与 ConstantBuffers.h / ModelLoader.cpp 一致）
#define TEX_SLOT_BASECOLOR           0u
#define TEX_SLOT_METALLIC_ROUGHNESS  1u
#define TEX_SLOT_OCCLUSION           2u
#define TEX_SLOT_EMISSIVE            3u

#define VERTEX_FLAG_UV0              8u
#define UV_OFFSET_NONE               0xFFFFFFFFu
#define ZONE_A_FIRST_DYNAMIC_SLOT    4u

// RootParam[0] = b0（12/16 DWORD 根常量，来自 PrimaryRayPass::Execute）
cbuffer PrimaryRayIndices : register(b0)
{
    uint g_TlasSrvIdx;             // DWORD 0: Zone B RaytracingAccelerationStructure SRV
    uint g_SceneDataSrvIdx;        // DWORD 1: Zone B 聚合几何 ByteAddressBuffer SRV
    uint g_InstanceDataSrvIdx;     // DWORD 2: Zone B 每实例数据 ByteAddressBuffer SRV
    uint g_DepthUavIdx;            // DWORD 3: PrimarySurface Depth (R32_FLOAT) UAV
    uint g_AlbedoAoUavIdx;         // DWORD 4: PrimarySurface AlbedoAO (R8G8B8A8_UNORM) UAV
    uint g_NormalUavIdx;           // DWORD 5: PrimarySurface Normal (R16G16_FLOAT) UAV
    uint g_MaterialUavIdx;         // DWORD 6: PrimarySurface Material (R16G16B16A16_FLOAT) UAV
    uint g_EmissionUavIdx;         // DWORD 7: PrimarySurface Emission (R8G8B8A8_UNORM) UAV
    uint g_EnableMaterialTextures; // DWORD 8: 0/1，同步 RenderParams::EnableMaterialTextures
    uint g_Width;                  // DWORD 9: 目标宽度
    uint g_Height;                 // DWORD 10: 目标高度
    uint g_Pad0;                   // DWORD 11
};

// RootParam[1] = b1（Per-Frame CBV，与 C++ PrimaryRayPass::FrameConstants 160B 严格对齐）
cbuffer PrimaryRayFrameCB : register(b1)
{
    float4x4 g_InvViewProj; // offset   0 (64 B)
    float4x4 g_ViewProj;    // offset  64 (64 B)
    float4   g_CameraPos;   // offset 128 (16 B)
    float4   g_OutputSize;  // offset 144 (16 B): xy=W,H, zw=1/W,1/H
};

float3 LoadVertexPosition(ByteAddressBuffer sceneData, uint vertexOffsetInScene, uint vertexIndex, uint stride)
{
    const uint addr = vertexOffsetInScene + vertexIndex * stride;
    return float3(
        asfloat(sceneData.Load(addr + 0u)),
        asfloat(sceneData.Load(addr + 4u)),
        asfloat(sceneData.Load(addr + 8u)));
}

uint LoadIndex(ByteAddressBuffer sceneData, uint indexOffsetInScene, uint indexElement, uint indexFormat)
{
    const uint addr = indexOffsetInScene + indexElement * (indexFormat == 1u ? 4u : 2u);
    if (indexFormat == 1u)
        return sceneData.Load(addr);

    // ByteAddressBuffer::Load 要求 4 字节对齐，16-bit 索引先按字对齐读取再拆半字
    const uint word = sceneData.Load(addr & ~3u);
    return ((addr & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

float2 LoadVertexUv(ByteAddressBuffer sceneData, uint vertexOffsetInScene, uint vertexIndex, uint vertexStride, uint uvByteOffset)
{
    const uint addr = vertexOffsetInScene + vertexIndex * vertexStride + uvByteOffset;
    const uint packed = sceneData.Load(addr & ~3u);
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= g_Width || pixel.y >= g_Height)
        return;

    RWTexture2D<float>  outDepth    = LoadBindless<RWTexture2D<float> >(g_DepthUavIdx);
    RWTexture2D<float4> outAlbedoAO = LoadBindless<RWTexture2D<float4> >(g_AlbedoAoUavIdx);
    RWTexture2D<float2> outNormal   = LoadBindless<RWTexture2D<float2> >(g_NormalUavIdx);
    RWTexture2D<float4> outMaterial = LoadBindless<RWTexture2D<float4> >(g_MaterialUavIdx);
    RWTexture2D<float4> outEmission = LoadBindless<RWTexture2D<float4> >(g_EmissionUavIdx);

    // 与 DeferredCommon.hlsli::ReconstructWorldPosition / RayTracing.hlsl::RayGen 完全同构的主射线生成
    const float2 uv  = (float2(pixel) + 0.5f) * g_OutputSize.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    const float4 nearH = mul(float4(ndc, 0.0f, 1.0f), g_InvViewProj);
    const float4 farH  = mul(float4(ndc, 1.0f, 1.0f), g_InvViewProj);
    const float3 nearW = nearH.xyz / nearH.w;
    const float3 farW  = farH.xyz / farH.w;

    RayDesc ray;
    ray.Origin    = g_CameraPos.xyz;
    ray.Direction = normalize(farW - nearW);
    ray.TMin      = 0.0f;
    ray.TMax      = 1.0e4f;

    // DXR 1.1 Inline RayQuery：复用主场景 TLAS（零第二套 SBT），只解析主射线最近不透明命中
    RaytracingAccelerationStructure scene = LoadBindless<RaytracingAccelerationStructure>(g_TlasSrvIdx);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0xFFu, ray);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // 未命中约定：Depth=1.0（与光栅 ClearDepth(1.0) 一致），其余通道写干净哨兵值
        outDepth[pixel]    = 1.0f;
        outAlbedoAO[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        outNormal[pixel]   = float2(0.5f, 0.5f);
        outMaterial[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        outEmission[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    ByteAddressBuffer sceneData    = LoadBindless<ByteAddressBuffer>(g_SceneDataSrvIdx);
    ByteAddressBuffer instanceData = LoadBindless<ByteAddressBuffer>(g_InstanceDataSrvIdx);

    const uint   instanceId = q.CommittedInstanceID();
    const uint   tri        = q.CommittedPrimitiveIndex();
    const float2 bary       = q.CommittedTriangleBarycentrics();
    const float  hitT       = q.CommittedRayT();
    const float3x4 o2w      = q.CommittedObjectToWorld3x4();

    const uint base         = instanceId * INSTANCE_STRIDE_BYTES;
    const uint vertexOffset = instanceData.Load(base + INSTANCE_VERTEX_OFFSET);
    const uint indexOffset  = instanceData.Load(base + INSTANCE_INDEX_OFFSET);
    const uint vertexStride = instanceData.Load(base + INSTANCE_VERTEX_STRIDE);
    const uint indexFormat  = instanceData.Load(base + INSTANCE_INDEX_FORMAT);
    const uint startIndex   = instanceData.Load(base + INSTANCE_START_INDEX);
    const uint baseVertex   = instanceData.Load(base + INSTANCE_BASE_VERTEX);

    const uint i0 = LoadIndex(sceneData, indexOffset, startIndex + tri * 3u + 0u, indexFormat);
    const uint i1 = LoadIndex(sceneData, indexOffset, startIndex + tri * 3u + 1u, indexFormat);
    const uint i2 = LoadIndex(sceneData, indexOffset, startIndex + tri * 3u + 2u, indexFormat);

    const float3 p0 = LoadVertexPosition(sceneData, vertexOffset, baseVertex + i0, vertexStride);
    const float3 p1 = LoadVertexPosition(sceneData, vertexOffset, baseVertex + i1, vertexStride);
    const float3 p2 = LoadVertexPosition(sceneData, vertexOffset, baseVertex + i2, vertexStride);

    // 物体空间面法线 -> 世界空间：
    // RaytracingScene::UpdateInstances 将行向量世界矩阵 M 转置写入 DXR 3x4 Transform（平移在第 3 列），
    // 故行向量 v * M 等价于 dot(o2w[row].xyz, v)，显式点积对 pack_matrix 布局 100% 免疫。
    float3 nObj = cross(p1 - p0, p2 - p0);
    const float nObjLenSq = dot(nObj, nObj);
    nObj = (nObjLenSq > 1e-16f) ? (nObj * rsqrt(nObjLenSq)) : float3(0.0f, 1.0f, 0.0f);

    float3 nWorld = float3(
        dot(o2w[0].xyz, nObj),
        dot(o2w[1].xyz, nObj),
        dot(o2w[2].xyz, nObj));
    const float nWorldLenSq = dot(nWorld, nWorld);
    nWorld = (nWorldLenSq > 1e-16f) ? (nWorld * rsqrt(nWorldLenSq)) : float3(0.0f, 1.0f, 0.0f);

    // 双面一致性：法线始终朝向入射主射线一侧
    if (dot(nWorld, ray.Direction) > 0.0f)
        nWorld = -nWorld;

    // 投影命中点至裁剪空间，产出与光栅 D32_FLOAT 严格同构的 NDC Depth ∈ [0, 1]
    const float3 hitPos  = ray.Origin + ray.Direction * hitT;
    const float4 clipPos = mul(float4(hitPos, 1.0f), g_ViewProj);
    const float  ndcDepth = (clipPos.w > 1e-6f) ? saturate(clipPos.z / clipPos.w) : 1.0f;

    // 读取材质标量因子
    const uint   materialIdx = instanceData.Load(base + INSTANCE_MATERIAL_INDEX);
    const uint   srvBase     = instanceData.Load(base + INSTANCE_SRV_TABLE_OFFSET);
    const uint   vertexFlags = instanceData.Load(base + INSTANCE_VERTEX_FLAGS);
    const uint   uvOffset    = instanceData.Load(base + INSTANCE_UV_OFFSET);

    float3 albedo    = asfloat(instanceData.Load3(base + INSTANCE_BASECOLOR_FACTOR));
    float3 emissive  = asfloat(instanceData.Load3(base + INSTANCE_EMISSIVE_FACTOR));
    float  metallic  = saturate(asfloat(instanceData.Load(base + INSTANCE_METALLIC)));
    float  roughness = saturate(asfloat(instanceData.Load(base + INSTANCE_ROUGHNESS)));
    float  ao        = 1.0f;

    if (g_EnableMaterialTextures == 0u)
    {
        albedo = float3(0.8f, 0.8f, 0.8f);
    }
    else if ((vertexFlags & VERTEX_FLAG_UV0) != 0u && uvOffset != UV_OFFSET_NONE)
    {
        const float2 uv0 = LoadVertexUv(sceneData, vertexOffset, baseVertex + i0, vertexStride, uvOffset);
        const float2 uv1 = LoadVertexUv(sceneData, vertexOffset, baseVertex + i1, vertexStride, uvOffset);
        const float2 uv2 = LoadVertexUv(sceneData, vertexOffset, baseVertex + i2, vertexStride, uvOffset);
        const float  b0  = 1.0f - bary.x - bary.y;
        const float2 texUv = uv0 * b0 + uv1 * bary.x + uv2 * bary.y;

        // [SM 6.6 Bindless 发散索引铁律]
        // 同一 8x8 线程组内不同像素会命中不同材质实例，srvBase 跨 Lane 发散，
        // 必须用 NonUniformResourceIndex 包裹防止驱动标量化错采！
        Texture2D<float4> baseColorTex = LOAD_BINDLESS_NONUNIFORM(Texture2D<float4>, srvBase + TEX_SLOT_BASECOLOR);
        albedo *= baseColorTex.SampleLevel(g_SampAnisoWrap, texUv, 0.0f).rgb;

        // 仅当 srvBase 指向真实 5 槽 Zone A 材质块（>= 4）时才读取其余槽位；
        // 避免无效材质哨兵 srvBase==0 时将槽 3（ZeroByteBuffer）误当 Texture2D 采样。
        if (srvBase >= ZONE_A_FIRST_DYNAMIC_SLOT)
        {
            Texture2D<float4> mrTex  = LOAD_BINDLESS_NONUNIFORM(Texture2D<float4>, srvBase + TEX_SLOT_METALLIC_ROUGHNESS);
            Texture2D<float4> occTex = LOAD_BINDLESS_NONUNIFORM(Texture2D<float4>, srvBase + TEX_SLOT_OCCLUSION);
            Texture2D<float4> emTex  = LOAD_BINDLESS_NONUNIFORM(Texture2D<float4>, srvBase + TEX_SLOT_EMISSIVE);

            const float4 mrSample = mrTex.SampleLevel(g_SampAnisoWrap, texUv, 0.0f);
            metallic  = saturate(metallic * mrSample.b);
            roughness = saturate(roughness * mrSample.g);
            ao        = saturate(occTex.SampleLevel(g_SampAnisoWrap, texUv, 0.0f).r);
            emissive *= emTex.SampleLevel(g_SampAnisoWrap, texUv, 0.0f).rgb;
        }
    }

    outDepth[pixel]    = ndcDepth;
    outAlbedoAO[pixel] = float4(saturate(albedo), ao);
    outNormal[pixel]   = EncodeOctNormal(nWorld);
    outMaterial[pixel] = float4(metallic, roughness, 0.5f, (float)materialIdx);
    outEmission[pixel] = float4(saturate(emissive), 0.0f);
}
