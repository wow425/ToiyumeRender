// ============================================================================
//  RayTracing.hlsl —— R6 / D3：光追算法（硬阴影射线 + 单次反射）
// ----------------------------------------------------------------------------
//  编译方式：dxc -T lib_6_4（见 Engine/20_cmake/ShaderCompiler.cmake 的
//            compile_raytracing_library）。所有入口一次性编进同一份 DXIL 库，
//            再由 State Object 的 DXIL_LIBRARY 子对象按导出名挑选。
//
//  演进历史：
//    R5 / D2 —— 最小闭环：RayGen / Miss / ClosestHit，命中输出面法线色。
//               那只是"证明 DXR 通了"，不是算法。
//    R6 / D3 —— 本文件：主射线 → 命中 → (a) 硬阴影射线 (b) 单次反射射线。
//               两条二级射线都从 ClosestHit 里发出，递归深度 2。
//
//  设计约束（必须与光栅路径一致，否则两条路径的画面无法对比）：
//   1) 矩阵约定：本项目是**行向量**（v * M）、右手系、z∈[0,1]。
//      射线生成用 InvViewProj 把 NDC 反投影到世界空间，
//      与 DeferredCommon.hlsli::ReconstructWorldPosition 完全同构。
//   2) 顶点格式：POSITION 是 R32G32B32_FLOAT，位于顶点步长的偏移 0 处。
//      本阶段用**几何面法线**（cross(p1-p0, p2-p0)），不读顶点法线 ——
//      因为不同模型的 VB 布局（是否有 TANGENT/UV、步长多大）各异，
//      而 RaytracingInstanceData 只记录了位置偏移。代价是画面呈
//      "低多边形刻面"外观；路线图 D3 的后续项（DXR 接材质）会补上。
//   3) 显示编码：输出前走 ColorPipeline.hlsli 的 EncodeDisplayColor，
//      与光栅路径共用同一条 tonemap + sRGB 链。
//   4) 光照模型：Lambert 漫反射 + 天空环境项 + 硬阴影 + 屏幕无关反射。
//      刻意**不**实现 Cook-Torrance —— 没有材质/法线贴图时它只是把
//      同一份错误数据算得更贵。D3 的目标是"有明确物理含义的光追效果"，
//      不是"看起来像 PBR"。
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
//  根常量（b0）：**48** 个 DWORD
//  必须与 C++ 侧 RootParameter::InitAsConstants(0, 48) 与
//  RaytracingConstants 逐字段对齐（见 RayTracingRenderer.h 的 static_assert）。
//
//  R5 时是 32 个 DWORD；R6 追加 g_LightDirection / g_LightColor /
//  g_RTParams / g_RTParams2 四个 float4（+16 DWORD）用于阴影与反射。
//  追加只能放在**末尾**，否则前面所有字段整体错位。
// ---------------------------------------------------------------------------
cbuffer RaytracingConstants : register(b0)
{
    float4x4 g_InvViewProj;      // 16 float：世界 <- 裁剪
    float4   g_CameraPosition;   // 4 float
    float4   g_OutputSize;       // 4 float：xy = 分辨率, zw = 1/分辨率
    float4   g_DisplayParams;    // 4 float：x=exposure, y=tonemap, z=srgb, w=备用
    float4   g_DebugParams;      // 4 float：x=调试模式（保留）

    // ---- R6 / D3 追加 ----
    // 方向光。语义与 LightingPS.hlsl 的 cbuffer Light 完全一致：
    //   g_LightDirection.xyz 是**光的传播方向**（不是指向光源的方向），
    //   着色时要取反：dirL = normalize(-g_LightDirection.xyz)。
    //   这一点必须与光栅路径一致，否则两条路径的主光会一上一下。
    float4   g_LightDirection;   // xyz = 传播方向, w = 强度
    float4   g_LightColor;       // rgb = 颜色, a = 备用
    // x = 阴影射线沿法线的抬起距离（米，偏差）
    // y = 阴影射线最大距离（米）
    // z = 反射射线最大距离（米）
    // w = 反射强度倍率
    float4   g_RTParams;
    // x = EnableRTShadow (0/1)
    // y = EnableRTReflection (0/1)
    // z = RayTracingDebugView 通道编号
    // w = 备用
    float4   g_RTParams2;
};                               // 共 48 个 DWORD

// ---------------------------------------------------------------------------
//  载荷与命中属性
//  MaxPayloadSizeInBytes / MaxAttributeSizeInBytes 必须与 C++ 侧
//  RaytracingStateObjectDesc 一致（32 / 8）。
// ---------------------------------------------------------------------------
struct RayPayload
{
    float3 Color;         // HDR 颜色（显示编码前）
    float  HitDistance;   // 未命中时为 -1
    uint   Depth;         // 0 = 主射线，1 = 反射射线（用于阻止二次递归）
    uint   _pad0;
    float2 _pad1;         // 补齐到 32 字节（与 C++ 的 MaxPayloadSizeInBytes 对齐）
};

// 阴影射线的载荷。只有"完全未命中"时 ShadowMiss 会把它设成 1，
// 命中时**任何着色器都不会被调用**（见下面 TraceShadowRay 的 flag 说明），
// 因此初值 0 就代表"被遮挡"。
struct ShadowPayload
{
    float Visible;        // 1 = 受光，0 = 被遮挡
    float _pad0;
};

struct HitAttributes
{
    float2 Barycentrics;
};

// 天空色：未命中时返回的 HDR 值。反射射线打空时也用同一个函数，
// 保证"背景"和"反射里看到的天空"是同一片天，画面才有连续性。
float3 SkyColor(float3 direction)
{
    // 极简天穹：与 Miss 里 R5 沿用的深蓝灰同色系，按 y 分量做一点梯度，
    // 让"反射里能看到地平线"这件事在画面上可辨认（纯常数色时反射几乎不可见）。
    const float t = saturate(direction.y * 0.5f + 0.5f);
    const float3 horizon = float3(0.02f, 0.03f, 0.06f);
    const float3 zenith  = float3(0.05f, 0.08f, 0.16f);
    return lerp(horizon, zenith, t);
}

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
//  D3-1：硬阴影射线
// ---------------------------------------------------------------------------
//  为什么用 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH：
//    阴影是布尔量（挡 / 不挡），一旦命中就没有继续遍历的必要。
//    这个 flag 让遍历在第一个命中处终止，是最便宜的做法。
//
//  为什么用 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER（关键）：
//    DXR 的命中着色器签名是按**载荷类型**绑定的。主射线的 ClosestHit 收
//    RayPayload，阴影射线收 ShadowPayload —— 如果阴影射线也走到主射线的
//    hit group，那个 ClosestHit 会把 ShadowPayload 当 RayPayload 解释，
//    写坏 32 字节里的字段，属于静默的类型混淆（编译期不报错）。
//    加上 skip 之后：命中时不调用任何着色器，载荷保持调用前的初值 0；
//    只有**完全未命中**才调用 ShadowMiss 把它置 1。
//    「初值 0 = 被遮挡」这个约定因此成立，而且不需要第二个 hit group 记录。
float TraceShadowRay(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = tMax;

    ShadowPayload payload;
    payload.Visible = 0.0f;
    payload._pad0 = 0.0f;

    TraceRay(g_Scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF,   // InstanceInclusionMask
        0,      // RayContributionToHitGroupIndex
        1,      // MultiplierForGeometryContributionToHitGroupIndex
        1,      // MissShaderIndex = 1 → SBT miss 表的第二条记录（ShadowMiss）
        ray,
        payload);

    return payload.Visible;
}

// ---------------------------------------------------------------------------
//  D3-2：单次反射射线
// ---------------------------------------------------------------------------
//  反射的递归控制在 payload.Depth 上做，而不是靠更深的 SRT：
//  主射线（Depth=0）的 ClosestHit 允许发反射；反射射线把自己的 Depth 置 1，
//  其 ClosestHit 看到 Depth>=1 就只做本地面着色、不再发二级射线。
//  这样无论场景多复杂，最大递归深度恒为 2 —— 与 State Object 的
//  MaxTraceRecursionDepth=2 严格一致。写错会在运行时被设备直接移除
//  （DEVICE_REMOVED / 0x887A0005），不会有 Debug Layer 提示。
void TraceReflectionRay(float3 origin, float3 direction, float tMax, float tMin,
    out float3 outColor, out float outDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayPayload payload;
    payload.Color = float3(0.0f, 0.0f, 0.0f);
    payload.HitDistance = -1.0f;
    payload.Depth = 1;          // 反射射线：禁止再发二级射线
    payload._pad0 = 0;
    payload._pad1 = float2(0.0f, 0.0f);

    TraceRay(g_Scene,
        RAY_FLAG_NONE,
        0xFF,
        0,
        1,
        0,      // MissShaderIndex = 0 → 主射线用的 Miss
        ray,
        payload);

    outColor = payload.Color;
    outDistance = payload.HitDistance;
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
    payload.Depth       = 0;    // 主射线：允许 ClosestHit 发二级射线
    payload._pad0       = 0;
    payload._pad1       = float2(0.0f, 0.0f);

    TraceRay(
        g_Scene,
        RAY_FLAG_NONE,
        0xFF,   // InstanceInclusionMask：全开
        0,      // RayContributionToHitGroupIndex
        1,      // MultiplierForGeometryContributionToHitGroupIndex
        0,      // MissShaderIndex
        ray,
        payload);

    // 未命中 → 天空色（一眼区分"没打到"与"打到但全黑"）。
    const float3 hdr = payload.HitDistance < 0.0f
        ? SkyColor(ray.Direction)
        : payload.Color;

    const float3 display = EncodeDisplayColor(hdr,
        g_DisplayParams.x,
        g_DisplayParams.y > 0.5f,
        g_DisplayParams.z > 0.5f);

    // SceneColorBuffer 的 UAV 格式由 ColorBuffer::GetUAVFormat() 推导
    // （R8G8B8A8_UNORM → R8G8B8A8_UNORM），因此这里直接写 float4。
    g_OutputColor[pixel] = float4(display, 1.0f);
}

// ---------------------------------------------------------------------------
//  Miss #0：主射线 / 反射射线未命中任何几何
// ---------------------------------------------------------------------------
[shader("miss")]
void Miss(inout RayPayload payload)
{
    // 用 WorldRayDirection() 而不是外部变量：miss 着色器能拿到本射线的方向，
    // 反射射线打空时因此能得到"反射里看到的天"。
    payload.Color       = SkyColor(normalize(WorldRayDirection()));
    payload.HitDistance = -1.0f;
}

// ---------------------------------------------------------------------------
//  Miss #1：阴影射线未命中 → 受光
// ---------------------------------------------------------------------------
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.Visible = 1.0f;
}

// ---------------------------------------------------------------------------
//  ClosestHit：主射线 → 着色（Lambert + 硬阴影 + 反射）
//             反射射线 → 只做本地面着色，不再递归
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
    //
    // 注意：这里**不**用 InverseTranspose —— 本场景的实例变换只有平移+均匀缩放，
    // 法线与顶点用同一个矩阵变换在数学上成立。将来若引入非均匀缩放必须改成
    // InverseTranspose，否则法线方向会偏。
    float3 n = normalize(cross(p1 - p0, p2 - p0));
    n = normalize(mul(n, (float3x3)ObjectToWorld3x4()));

    // 双面一致性：让法线始终朝向入射射线一侧，避免背面一片死黑。
    const float3 incoming = WorldRayDirection();
    if (dot(n, incoming) > 0.0f)
        n = -n;

    const float3 hitPos = WorldRayOrigin() + incoming * RayTCurrent();
    const float  hitDistance = RayTCurrent();

    // ---- 递归闸门（必须最先判断）-----------------------------------------
    // 反射射线的 ClosestHit 也走这个函数。如果这里不早退，它会再发一条反射射线
    // （深度 3），而 State Object 的 MaxTraceRecursionDepth = 2 —— 越界不会报错，
    // 设备会直接把整个进程移除（DEVICE_REMOVED 0x887A0005），且 Debug Layer
    // 不一定留下可读信息。因此 Depth >= 1 时只做本地面着色，立即返回。
    if (payload.Depth >= 1u)
    {
        const float3 toLightSecondary = normalize(-g_LightDirection.xyz);
        const float  nDotLSecondary = saturate(dot(n, toLightSecondary));
        const float3 albedoSecondary = float3(0.8f, 0.8f, 0.8f);
        payload.Color = albedoSecondary * (g_LightColor.rgb * g_LightDirection.w * nDotLSecondary
            + SkyColor(n) * 0.35f);
        return;
    }

    // ---- 着色参数 ---------------------------------------------------------
    const uint   debugView    = (uint)(g_RTParams2.z + 0.5f);
    const bool   enableShadow = g_RTParams2.x > 0.5f;
    const bool   enableRefl   = g_RTParams2.y > 0.5f;

    // 与光栅路径同一条主光：g_LightDirection 是传播方向，取反才是指向光源。
    const float3 toLight = normalize(-g_LightDirection.xyz);
    const float3 lightRadiance = g_LightColor.rgb * g_LightDirection.w;

    // ---- D3-1：硬阴影 -----------------------------------------------------
    // 偏差策略：唯一偏差是沿**法线**抬起 normalBias 米。
    // 为什么必须沿法线而不是沿光线：沿法线抬起保证新起点仍在表面外侧，
    // 沿光线抬起在掠射角会把起点推到表面内侧，反而制造更多自遮挡。
    float visibility = 1.0f;
    if (enableShadow)
    {
        const float3 shadowOrigin = hitPos + n * g_RTParams.x;
        visibility = TraceShadowRay(shadowOrigin, toLight, g_RTParams.y);
    }

    // ---- 基础着色：Lambert + 天空环境 --------------------------------------
    const float  nDotL = saturate(dot(n, toLight));
    const float3 albedo = float3(0.8f, 0.8f, 0.8f);   // D3 未接材质，用中性灰
    const float3 ambient = SkyColor(n) * 0.35f;
    const float3 diffuse = albedo * lightRadiance * nDotL * visibility;

    float3 shaded = diffuse + albedo * ambient;

    // ---- D3-2：单次反射 ---------------------------------------------------
    float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
    const float3 viewDir = -incoming;
    const float  nDotV = saturate(dot(n, viewDir));

    if (enableRefl)
    {
        const float3 reflectDir = normalize(reflect(incoming, n));
        // TMin 用与阴影同一个偏差量：反射射线的起点和阴影射线相同，
        // 若从 0 起算会立刻命中自己所在的三角形（自相交 → 全场条纹）。
        float reflectionDistance = -1.0f;
        TraceReflectionRay(hitPos + n * g_RTParams.x, reflectDir, g_RTParams.z,
            g_RTParams.x, reflectionColor, reflectionDistance);

        // 反射里看到天空时，Miss 已经填好 SkyColor，这里不再单独处理。

        // Fresnel（Schlick，F0 取 0.04 的介电体近似）：
        // 掠射角反射强、正视反射弱 —— 这是反射看起来"像反射"的关键，
        // 常数混合会让人觉得只是加了一层灰色。
        const float fresnel = 0.04f + 0.96f * pow(1.0f - nDotV, 5.0f);
        shaded = lerp(shaded, reflectionColor, saturate(fresnel * g_RTParams.w));
    }

    // ---- 调试通道 ---------------------------------------------------------
    float3 result = shaded;
    if (debugView == 1u)      result = n * 0.5f + 0.5f;
    else if (debugView == 2u) result = float3(visibility, visibility, visibility);
    else if (debugView == 3u) result = reflectionColor;
    else if (debugView == 4u) result = float3(saturate(hitDistance / 32.0f),
                                             saturate(hitDistance / 32.0f),
                                             saturate(hitDistance / 32.0f));

    payload.HitDistance = hitDistance;
    payload.Color       = result;
}
