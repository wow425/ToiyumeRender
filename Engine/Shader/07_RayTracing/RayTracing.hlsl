// ============================================================================
//  RayTracing.hlsl —— R5 / D2：DXR 最小闭环（RayGen / Miss / ClosestHit）
// ----------------------------------------------------------------------------
//  编译方式：dxc -T lib_6_4（见 Engine/20_cmake/ShaderCompiler.cmake 的
//            compile_raytracing_library）。三个入口一次性编进同一份 DXIL 库，
//            再由 State Object 的 DXIL_LIBRARY 子对象按导出名挑选。
//
//  设计约束（必须与光栅路径一致，否则两条路径的画面无法对比）：
//   1) 矩阵约定：本项目是**行向量**（v * M）、右手系、z∈[0,1]。
//      射线生成用 InvViewProj 把 NDC 反投影到世界空间，
//      与 DeferredCommon.hlsli::ReconstructWorldPosition 完全同构。
//   2) 顶点格式：POSITION 是 R32G32B32_FLOAT，位于顶点步长的偏移 0 处
//      （见 MeshConvert.cpp 的 VBWriter 写入顺序）。
//      本阶段只用位置算**几何面法线**，不依赖法线/切线的 UNORM 打包，
//      因此诊断画面的含义是「三角形朝向」，无歧义。
//   3) 显示编码：输出前走 ColorPipeline.hlsli 的 EncodeDisplayColor，
//      与光栅路径共用同一条 tonemap + sRGB 链。
//
//  本阶段（D2）只做**诊断画面**：命中返回世界空间面法线色，未命中返回背景色。
//  不含阴影射线、反射、材质采样 —— 那些是 D3 的范围。
// ============================================================================

#include "02_Deferred/ColorPipeline.hlsli"

// 【必须】与 DeferredCommon.hlsli / Common.hlsli 保持一致：本项目 CPU 侧矩阵是
// 行主序、行向量约定（v * M），上传时不做转置。HLSL 的默认矩阵打包是
// column_major，如果不显式声明 row_major，cbuffer 里的 float4x4 会被按列读取，
// 等价于对矩阵做了一次转置 —— InvViewProj 一旦被转置，反投影出来的射线方向全错，
// 结果是**每一条射线都打空**。
//
// 这个 bug 的隐蔽性极高：不报错、不崩溃、Debug Layer 干净、DispatchRays 照常提交，
// 画面是一整片「未命中背景色」。因为 miss 着色器把 (0.02,0.03,0.06) 写进 UAV，
// 再经 ACES + sRGB 编码后正好是 (26,38,69) —— 实测像素值完全吻合，
// 这本身就是「射线在跑但全打空」的铁证（真正没派发/没写 UAV 时是纯黑 0,0,0）。
//
// 注意 ColorPipeline.hlsli 里没有这个 pragma（它只放纯函数），所以必须在本文件加。
#pragma pack_matrix(row_major)

// ---------------------------------------------------------------------------
//  资源绑定
// ---------------------------------------------------------------------------
//  TLAS：DXR 里加速结构是以「SRV（RAYTRACING_ACCELERATION_STRUCTURE 维度）」
//  的形式绑定的，HLSL 侧声明成 RaytracingAccelerationStructure 后
//  直接作为 TraceRay 的第一个实参传入。
RaytracingAccelerationStructure g_Scene : register(t0);

// 场景几何：所有模型的 VB/IB 在构建 BLAS 时被聚合进这一块大缓冲，
// 记录每个实例在其中的偏移（见 RaytracingScene.cpp 的 SceneGeometry 聚合）。
// 用 ByteAddressBuffer 而不是 StructuredBuffer，因为顶点步长在不同模型间不同。
ByteAddressBuffer g_SceneData : register(t1);

// 每实例数据（stride = 32 字节 = 8 个 DWORD）
//
// 为什么用 ByteAddressBuffer 而不是 StructuredBuffer：
//   这里是通过**根级 SRV**（D3D12_ROOT_PARAMETER_TYPE_SRV）绑定的。
//   根 SRV 描述符不携带 StructureByteStride（它等价于一个 raw buffer view），
//   在 HLSL 里声明成 StructuredBuffer<T> 会读到错误的跨距。
//   因此统一按 ByteAddressBuffer + 手算偏移读取，与 g_SceneData 的做法一致。
//
// 为什么不存 ObjectToWorld：
//   DXR 内置的 ObjectToWorld3x4() 直接返回 TLAS instance desc 里那份变换
//   （Tier 1.0 起可用），数据本来就存在，没必要再冗余一份。
ByteAddressBuffer g_InstanceData : register(t2);

// 每实例记录的字段偏移（与 C++ 侧 RaytracingInstanceData 严格对应）
#define INSTANCE_VERTEX_OFFSET   0    // uint VertexOffset
#define INSTANCE_INDEX_OFFSET    4    // uint IndexOffset
#define INSTANCE_VERTEX_STRIDE   8    // uint VertexStride
#define INSTANCE_INDEX_FORMAT   12    // uint IndexFormat
#define INSTANCE_START_INDEX    16    // uint StartIndex
#define INSTANCE_BASE_VERTEX    20    // uint BaseVertex
#define INSTANCE_PRIM_COUNT     24    // uint PrimitiveCount
#define INSTANCE_ID             28    // uint InstanceId
#define INSTANCE_STRIDE_BYTES   32

// 输出
RWTexture2D<float4> g_OutputColor : register(u0);

// ---------------------------------------------------------------------------
//  根常量（b0）：32 个 DWORD
//  必须与 C++ 侧 RootParameter::InitAsConstants(0, 32) 逐字段对齐。
// ---------------------------------------------------------------------------
cbuffer RaytracingConstants : register(b0)
{
    float4x4 g_InvViewProj;      // 16 float：世界 <- 裁剪
    float4   g_CameraPosition;   // 4 float
    float4   g_OutputSize;       // 4 float：xy = 分辨率, zw = 1/分辨率
    float4   g_DisplayParams;    // 4 float：x=exposure, y=tonemap, z=srgb, w=备用
    float4   g_DebugParams;      // 4 float：x=调试模式（保留）
};                               // 共 32 个 DWORD

// ---------------------------------------------------------------------------
//  载荷与命中属性
//  MaxPayloadSizeInBytes / MaxAttributeSizeInBytes 必须与 C++ 侧
//  RaytracingStateObjectDesc 一致（16 / 8）。
// ---------------------------------------------------------------------------
struct RayPayload
{
    float3 Color;         // 已做显示编码前的 HDR 颜色
    float  HitDistance;   // 未命中时为 -1
};

struct HitAttributes
{
    float2 Barycentrics;
};

// ---------------------------------------------------------------------------
//  工具：从 g_SceneData 读一个顶点位置
// ---------------------------------------------------------------------------
float3 LoadVertexPosition(uint vertexOffsetInScene, uint vertexIndex, uint stride)
{
    const uint addr = vertexOffsetInScene + vertexIndex * stride;
    return float3(
        g_SceneData.Load<float>(addr + 0),
        g_SceneData.Load<float>(addr + 4),
        g_SceneData.Load<float>(addr + 8));
}

uint LoadIndex(uint indexOffsetInScene, uint indexElement, uint indexFormat)
{
    const uint addr = indexOffsetInScene + indexElement * (indexFormat == 1u ? 4u : 2u);
    if (indexFormat == 1u)
        return g_SceneData.Load<uint>(addr);

    // HLSL 没有 uint16_t（那是 C++ 的类型；DXC 的 -enable-16bit-types 才引入
    // uint16_t/min16uint 且会改变 ABI）。这里按「32 位字 + 半字提取」读 16 位索引：
    // ByteAddressBuffer::Load 要求地址 4 字节对齐，因此先对齐到字边界再取半字。
    const uint wordAddr = addr & ~3u;
    const uint word = g_SceneData.Load<uint>(wordAddr);
    return ((addr & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

// ---------------------------------------------------------------------------
//  RayGen：每个像素发一条主射线
// ---------------------------------------------------------------------------
[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 size  = DispatchRaysDimensions().xy;

    // 与 DeferredCommon.hlsli::ReconstructWorldPosition 同构：
    //   ndc.x = uv.x*2-1, ndc.y = 1-uv.y*2, clip = (ndc, depth, 1)
    const float2 uv  = (float2(pixel) + 0.5f) / float2(size);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // 反投影近/远两点求方向（行向量：mul(vector, matrix)）。
    const float4 nearH = mul(float4(ndc, 0.0f, 1.0f), g_InvViewProj);
    const float4 farH  = mul(float4(ndc, 1.0f, 1.0f), g_InvViewProj);
    const float3 nearW = nearH.xyz / nearH.w;
    const float3 farW  = farH.xyz / farH.w;

    RayDesc ray;
    ray.Origin    = g_CameraPosition.xyz;
    ray.Direction = normalize(farW - nearW);
    ray.TMin      = 0.0f;
    ray.TMax      = 1.0e4f;



    RayPayload payload;
    payload.Color       = float3(0.0f, 0.0f, 0.0f);
    payload.HitDistance = -1.0f;

    TraceRay(
        g_Scene,
        RAY_FLAG_NONE,
        0xFF,   // InstanceInclusionMask：全开
        0,      // RayContributionToHitGroupIndex
        1,      // MultiplierForGeometryContributionToHitGroupIndex
        0,      // MissShaderIndex
        ray,
        payload);

    // 未命中 → 背景色（深蓝灰，一眼区分「没打到」与「打到但全黑」）。
    const float3 hdr = payload.HitDistance < 0.0f
        ? float3(0.02f, 0.03f, 0.06f)
        : payload.Color;

#if defined(RT_DEBUG_RAYDIR) && RT_DEBUG_RAYDIR
    // 【临时诊断】把射线方向编码成颜色输出。
    // 相机反投影正确时，画面应是一条从中心向外的平滑彩色渐变
    // （中心附近方向≈(0,0,-1) → 蓝，边缘方向发散 → 各色渐变）。
    // 若矩阵转置错，会得到完全不连续/单一的颜色。
    {
        const float3 d = ray.Direction;
        g_OutputColor[pixel] = float4(d * 0.5f + 0.5f, 1.0f);
        return;
    }
#endif

    const float3 display = EncodeDisplayColor(hdr,
        g_DisplayParams.x,
        g_DisplayParams.y > 0.5f,
        g_DisplayParams.z > 0.5f);

    // SceneColorBuffer 的 UAV 格式由 ColorBuffer::GetUAVFormat() 推导
    // （R8G8B8A8_UNORM → R8G8B8A8_UNORM），因此这里直接写 float4。
    g_OutputColor[pixel] = float4(display, 1.0f);
}

// ---------------------------------------------------------------------------
//  Miss：未命中任何几何
// ---------------------------------------------------------------------------
[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.Color       = float3(0.0f, 0.0f, 0.0f);
    payload.HitDistance = -1.0f;
}

// ---------------------------------------------------------------------------
//  ClosestHit：命中三角形 → 世界空间面法线着色
// ---------------------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in HitAttributes attribs)
{
    // 按 InstanceID 取本实例的几何寻址信息（根 SRV + ByteAddressBuffer）。
    const uint base = InstanceID() * INSTANCE_STRIDE_BYTES;
    const uint vertexOffset   = g_InstanceData.Load<uint>(base + INSTANCE_VERTEX_OFFSET);
    const uint indexOffset    = g_InstanceData.Load<uint>(base + INSTANCE_INDEX_OFFSET);
    const uint vertexStride   = g_InstanceData.Load<uint>(base + INSTANCE_VERTEX_STRIDE);
    const uint indexFormat    = g_InstanceData.Load<uint>(base + INSTANCE_INDEX_FORMAT);
    const uint startIndex     = g_InstanceData.Load<uint>(base + INSTANCE_START_INDEX);
    const uint baseVertex     = g_InstanceData.Load<uint>(base + INSTANCE_BASE_VERTEX);

    // PrimitiveIndex() 是**几何内**的三角形序号（Tier 1.0 可用；
    // GeometryIndex() 才是 Tier 1.1+，本阶段刻意不用，保持兼容）。
    const uint tri = PrimitiveIndex();

    const uint i0 = LoadIndex(indexOffset, startIndex + tri * 3 + 0, indexFormat);
    const uint i1 = LoadIndex(indexOffset, startIndex + tri * 3 + 1, indexFormat);
    const uint i2 = LoadIndex(indexOffset, startIndex + tri * 3 + 2, indexFormat);

    const float3 p0 = LoadVertexPosition(vertexOffset, baseVertex + i0, vertexStride);
    const float3 p1 = LoadVertexPosition(vertexOffset, baseVertex + i1, vertexStride);
    const float3 p2 = LoadVertexPosition(vertexOffset, baseVertex + i2, vertexStride);

    // 物体空间面法线 → 世界空间。
    // 本项目是行向量约定（v * M），而 ObjectToWorld3x4() 返回的是 DXR 规定的
    // 行主序 3x4（平移在最后一列）。对方向向量做 mul(n, M3x3) 即可。
    float3 n = normalize(cross(p1 - p0, p2 - p0));
    n = normalize(mul(n, (float3x3)ObjectToWorld3x4()));

    // 双面一致性：让法线始终朝向入射射线一侧，避免背面一片死黑。
    if (dot(n, WorldRayDirection()) > 0.0f)
        n = -n;

    payload.HitDistance = RayTCurrent();
    payload.Color       = n * 0.5f + 0.5f;   // [-1,1] → [0,1]，直接可视化
}
