#pragma once

// ============================================================================
//  RaytracingScene.h —— R5 / D1：BLAS / TLAS / SBT 的持有者
// ----------------------------------------------------------------------------
//  职责边界（刻意的单一职责拆分）：
//    * RayTracingRenderer 只管「怎么画」（根签名、RTPSO、DispatchRays）。
//    * RaytracingScene 只管「场景的加速结构」（几何聚合、BLAS、TLAS、SBT）。
//  这样将来做 D3（阴影/反射）时，只需要往 Renderer 里加射线，不必碰 AS 逻辑。
//
//  几何聚合策略（为什么必须有这一步）：
//    HLSL 里无法解引用一个「运行时才知道的 GPU 虚拟地址」，而 ClosestHit
//    需要按命中三角形反查顶点。DXR 只给 InstanceID/PrimitiveIndex/Barycentrics。
//    因此所有实例的 VB/IB 必须先聚合进**一块** ByteAddressBuffer，
//    每个实例在其中的偏移记录在 StructuredBuffer 里，Hit shader 才能索引。
//    （另一种做法是非一致资源索引 + 每实例一个描述符，那要 Tier 1.1 + SM6.5，
//      与「最小闭环」目标不符。）
//
//  为什么一个实例一个 BLAS（而不是一个模型一个 BLAS）：
//    ClosestHit 里只能拿到 InstanceID() 与 PrimitiveIndex()。
//    GeometryIndex() 是 Tier 1.1 才有的 —— 若一个 BLAS 塞多个 geometry，
//    Tier 1.0 设备就无法判断命中属于哪个 geometry。
//    因此这里**每个 (实例, mesh, draw) 建一个单几何 BLAS**，
//    TLAS 实例与 BLAS 一一对应，InstanceID 直接索引每实例数据。
//    代价是实例数变多，但对本项目的场景规模完全可接受。
//
//  生命周期与状态（路线图 D1 要求「明确 AS 输入/输出 buffer 的状态、UAV barrier」）：
//    * BLAS/TLAS 结果缓冲：DEFAULT 堆，**创建时即为**
//        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE，
//      且整个生命周期内绝不发 ResourceBarrier —— 见 CreateAccelerationStructure。
//      （D3D12 不允许把普通资源"转换进" AS 状态，Debug Layer #526。）
//    * Scratch 缓冲：DEFAULT 堆，状态 UNORDERED_ACCESS，仅在构建期使用。
//      每个 BLAS / TLAS 在 scratch 里各占一段**互不重叠**的区域，
//      这样同一条命令列表里连续发多个 Build 也无需在中间插 UAV barrier
//      （重叠才是问题；不重叠就没有写后写冲突）。
//    * InstanceDesc / SBT：UPLOAD 堆（GENERIC_READ 恒定），
//      GPU 只读、CPU 每帧可能改写 —— 用 UPLOAD 堆可以完全避免对它们做状态
//      转换（对 UPLOAD 堆发 Transition = Debug Layer #741，进程终止）。
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <d3d12.h>

#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/Heap/UploadBuffer.h"

class GraphicsContext;
class RootSignature;

namespace Scene::Model
{
	class ModelInstance;
}

namespace Renderer::RayTracing
{
	// ============================================================
	//  与 HLSL 严格同构的每实例数据（stride = 48 字节）
	//  改动任何字段都必须同步 Engine/Shader/07_RayTracing/RayTracing.hlsl
	//  的 RaytracingInstanceData，否则命中着色器读到的是垃圾。
	//
	//  为什么**不**存 ObjectToWorld：
	//    DXR 的 Hit shader 里可以直接调用 ObjectToWorld3x4()（Tier 1.0 起可用），
	//    它返回的正是 TLAS instance desc 里那份 3x4 变换 —— 数据已经在
	//    instance desc 里了，再存一份纯属冗余，而且容易两边不一致。
	//    因此这里只放几何寻址信息。
	// ============================================================
	struct RaytracingInstanceData
	{
		uint32_t VertexOffset;       // 顶点数据在合并缓冲内的字节偏移
		uint32_t IndexOffset;        // 索引数据在合并缓冲内的字节偏移（已烘焙 startIndex）
		uint32_t VertexStride;       // 顶点步长（字节）
		uint32_t IndexFormat;        // 0 = R16_UINT, 1 = R32_UINT
		uint32_t StartIndex;         // 恒为 0（已烘焙进 IndexOffset）
		uint32_t BaseVertex;         // 顶点元素偏移（保留，来自 Mesh::Draw::baseVertex）
		uint32_t PrimitiveCount;     // 三角形数
		uint32_t InstanceId;
	};
	static_assert(sizeof(RaytracingInstanceData) == 32, "必须与 HLSL 的 RaytracingInstanceData 布局一致");

	// ============================================================
	//  纯资源持有型 buffer：不创建 SRV/UAV。
	//  GpuBuffer 的派生类必须实现 CreateDerivedViews，这里留空 ——
	//  加速结构缓冲由驱动直接读写，不需要任何描述符。
	//
	//  R5 修正：加速结构**必须**走 CreateAccelerationStructure，
	//  不能复用 GpuBuffer::Create。原因见该函数声明处的注释。
	// ============================================================
	class RaytracingBuffer : public GpuBuffer
	{
	public:
		virtual ~RaytracingBuffer() { Destroy(); }

		// 加速结构专用创建路径。
		// ------------------------------------------------------------
		// 不能复用 GpuBuffer::Create，因为：
		//   1) 它把 m_UsageState 固定成 COMMON，而 AS 资源**不允许**从别的
		//      状态转换进来 —— 必须在 CreateCommittedResource 时就处于
		//      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE。
		//      否则 Debug Layer 直接报
		//        ERROR #526 RESOURCE_BARRIER_INVALID_COMBINATION:
		//        "Resources cannot be transitioned into state
		//         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE"
		//      并 break（实测进程以 0x87a 异常终止，日志停在 RebuildGeometry）。
		//   2) 它把 m_ResourceFlags 固定成 ALLOW_UNORDERED_ACCESS，而 AS 资源
		//      需要 D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE。
		// 创建后**永远不要再对这块资源发 ResourceBarrier**。
		void CreateAccelerationStructure(const std::wstring& name, uint64_t sizeInBytes);

	protected:
		virtual void CreateDerivedViews(void) override {}
	};

	// ============================================================
	//  SBT 记录布局（每条 32 字节 = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES）
	// ============================================================
	struct ShaderBindingTableRecord
	{
		uint8_t ShaderIdentifier[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
	};

	class RaytracingScene
	{
	public:
		RaytracingScene() = default;
		~RaytracingScene() { Destroy(); }

		RaytracingScene(const RaytracingScene&) = delete;
		RaytracingScene& operator=(const RaytracingScene&) = delete;

		// 重建几何（模型集合变化 / 首次初始化时调用）。
		// 返回 false 表示没有可用的三角形几何（例如场景为空）。
		bool RebuildGeometry(GraphicsContext& gfxContext,
			const std::vector<Scene::Model::ModelInstance*>& models);

		// 每帧更新实例变换（只改 TLAS 的 instance desc 与每实例数据，
		// 不重建 BLAS）。这是路线图 D1「静态几何尽量复用 BLAS，
		// 只在实例变换变化时更新/重建 TLAS」的落点。
		void UpdateInstances(GraphicsContext& gfxContext,
			const std::vector<Scene::Model::ModelInstance*>& models);

		// 用当前实例 desc 重建 TLAS（几何未变但实例变换/数量变化时调用）。
		void RebuildTopLevel(GraphicsContext& gfxContext);

		void Destroy();

		bool IsValid() const { return m_TopLevel != nullptr && m_InstanceDataBuffer != nullptr; }

		ID3D12Resource* GetTopLevel() const { return m_TopLevel ? m_TopLevel->GetResource() : nullptr; }
		// 供 CommandContext::SetBufferSRV 使用（它要的是 GpuBuffer&，内部取 GPU 地址）
		const GpuBuffer* GetTopLevelBuffer() const { return m_TopLevel.get(); }
		const GpuBuffer* GetInstanceDataBuffer() const { return m_InstanceDataBuffer.get(); }
		const GpuBuffer* GetSceneGeometryBuffer() const { return m_SceneGeometry.get(); }

		uint32_t GetInstanceCount() const { return m_InstanceCount; }
		uint32_t GetBlasCount() const { return (uint32_t)m_BlasBuffers.size(); }
		uint32_t GetTriangleCount() const { return m_TriangleCount; }

		// ---- SBT ----
		// 需要 State Object 的 properties 来取 32 字节着色器标识符。
		//
		// R6 起 miss 表有**两条记录**（原来只有一条）：
		//   index 0 = missExport      （主射线 / 反射射线打空）
		//   index 1 = shadowMissExport（阴影射线打空 → 受光）
		// TraceRay 的第 6 个实参 MissShaderIndex 直接索引这张表，
		// 所以顺序就是契约：编号与这里写入的顺序必须一致。
		bool BuildShaderBindingTable(ID3D12StateObjectProperties* stateObjectProperties,
			const wchar_t* rayGenExport, const wchar_t* missExport,
			const wchar_t* shadowMissExport, const wchar_t* hitGroupName);

		const D3D12_DISPATCH_RAYS_DESC* GetDispatchRaysDesc() const { return &m_DispatchRaysDesc; }

	private:
		// 单个 (mesh, draw) 在合并缓冲里的几何区段
		struct GeometrySpan
		{
			uint32_t VertexOffset = 0;   // 合并缓冲内的字节偏移
			uint32_t IndexOffset = 0;    // 合并缓冲内的字节偏移（已含 startIndex）
			uint32_t VertexStride = 0;
			uint32_t IndexFormat = 0;    // 0=16bit, 1=32bit
			uint32_t PrimitiveCount = 0; // 三角形数
			uint32_t VertexCount = 0;
			uint32_t IndexCount = 0;
		};

		void DestroyBlasBuffers();
		void DestroySceneBuffers();

		// BLAS：每个 (实例, mesh, draw) 一个单几何 BLAS
		std::vector<std::unique_ptr<RaytracingBuffer>> m_BlasBuffers;

		std::unique_ptr<RaytracingBuffer> m_TopLevel;      // TLAS 结果
		std::unique_ptr<RaytracingBuffer> m_Scratch;       // 构建期 scratch（各段互不重叠）
		std::unique_ptr<GpuBuffer>        m_SceneGeometry; // 合并后的 VB/IB（ByteAddressBuffer）
		std::unique_ptr<GpuBuffer>        m_InstanceDataBuffer; // StructuredBuffer<RaytracingInstanceData>

		UploadBuffer m_InstanceDescsUpload;  // TLAS 的 D3D12_RAYTRACING_INSTANCE_DESC 数组
		UploadBuffer m_InstanceDataUpload;   // 合并几何后每实例数据的上传源（活到 GPU 执行完）
		UploadBuffer m_SbtUpload;            // SBT

		D3D12_DISPATCH_RAYS_DESC m_DispatchRaysDesc = {};

		uint32_t m_InstanceCount = 0;
		uint32_t m_TriangleCount = 0;
		uint64_t m_ScratchSize = 0;
		uint64_t m_TlasScratchOffset = 0;
		std::vector<uint64_t> m_BlasScratchOffsets;
		bool     m_TopLevelBuilt = false;
	};
}
