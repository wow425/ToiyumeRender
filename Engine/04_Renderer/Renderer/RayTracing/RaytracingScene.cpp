#include "00_Core/PCH.h"
#include "04_Renderer/Renderer/RayTracing/RaytracingScene.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "05_Scene/Model/Model.h"

#include <algorithm>

using namespace Graphics;

// ============================================================================
//  R5 / D1：BLAS / TLAS / SBT 实现
// ----------------------------------------------------------------------------
//  本文件只做「加速结构」，不含任何绘制逻辑。
// ============================================================================

namespace
{
	// D3D12 要求 AS 结果与 scratch 的地址按 256 字节对齐
	// （D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT）。
	constexpr uint64_t kASByteAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

	uint64_t AlignUp64(uint64_t value, uint64_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	// SBT 每个 section 的起始地址要按 D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT（64）对齐，
	// 每条记录 32 字节（D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES）。
	constexpr uint32_t kSbtRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
	constexpr uint32_t kSbtSectionAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64
}

// ============================================================================
//  RaytracingBuffer::CreateAccelerationStructure
// ----------------------------------------------------------------------------
//  为什么不能直接用 GpuBuffer::Create —— 两条硬性约束：
//    1) 加速结构资源**不允许**用 ResourceBarrier 转换进
//       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE。
//       它必须在 CreateCommittedResource 的 InitialState 参数里就给出该状态。
//       否则 Debug Layer 报
//         ERROR #526 RESOURCE_BARRIER_INVALID_COMBINATION
//       并 break（实测进程以 0x87a 异常终止，日志刚好停在 RebuildGeometry）。
//    2) 资源描述里必须带 D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE。
//  GpuBuffer::Create 把这两者都写死成 COMMON / ALLOW_UNORDERED_ACCESS，
//  因此这里单独开一条创建路径。
//
//  创建后该资源在整个生命周期内**不再发生任何状态转换** ——
//  BuildRaytracingAccelerationStructure 要求它已是 AS 状态，
//  DispatchRays 以 root SRV 直接读取它（root SRV 不需要状态转换）。
// ============================================================================
void Renderer::RayTracing::RaytracingBuffer::CreateAccelerationStructure(
	const std::wstring& name, uint64_t sizeInBytes)
{
	Destroy();

	m_ElementCount = 1;
	m_ElementSize = (uint32_t)sizeInBytes;
	m_BufferSize = (size_t)sizeInBytes;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = sizeInBytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	// 标志位必须是 ALLOW_UNORDERED_ACCESS —— 两个错误都实测踩过：
	//   (a) 用 D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE(0x100)：
	//       本机运行时是 Windows 10 19041 的 D3D12（10.0.19041.5794），
	//       不认识该位，CreateCommittedResource 报
	//         ERROR #719 CREATERESOURCE_UNRECOGNIZEDLAYOUT
	//         "Flags has unrecognized bits. The value is 0x100"
	//       该标志是 Agility SDK 1.7xx 之后才引入的，不是 DXR 1.0 的必需项。
	//   (b) 用 D3D12_RESOURCE_FLAG_NONE：报
	//         ERROR #524 RESOURCE_BARRIER_MISMATCHING_MISC_FLAGS
	//         "D3D12_RESOURCE_STATES has bits that mismatch support
	//          required from D3D12_RESOURCE_FLAGS"
	//       即「初始状态是 AS」这件事要求资源具备 UAV 能力。
	// 结论：与微软官方 D3D12 Raytracing 样例一致 ——
	//       ALLOW_UNORDERED_ACCESS + 初始状态 RAYTRACING_ACCELERATION_STRUCTURE。
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	// 关键：初始状态直接就是 AS，之后永不转换。
	m_UsageState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

	ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
		&desc, m_UsageState, nullptr, TY_IID_PPV_ARGS(&m_pResource)));

	m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();

#ifdef RELEASE
	(name);
#else
	m_pResource->SetName(name.c_str());
#endif

	// AS 缓冲不需要 SRV/UAV 描述符（root SRV 直接给 GPU 地址）。
	CreateDerivedViews();
}

// ============================================================================
//  几何聚合
// ----------------------------------------------------------------------------
//  为什么要聚合：HLSL 无法解引用「运行时才知道的 GPU 虚拟地址」。
//  ClosestHit 只拿到 InstanceID()/PrimitiveIndex()，必须能按偏移索引到顶点。
//  因此把所有实例用到的模型几何，各复制一份到**一块**大 ByteAddressBuffer，
//  每个实例记录自己那一段的字节偏移。
//
//  为什么用 GPU→GPU 复制而不是回读：
//    Mesh 元数据（vbOffset/vbSize/ibOffset/ibSize/vbStride/ibFormat/
//    draw.startIndex/baseVertex/primCount）已经完整描述了每个区段的位置与长度，
//    我们不需要知道顶点字节内容就能算偏移。所以只要 CopyBufferRegion 就够，
//    省掉一次同步回读（回读必须 Finish(true) 等 GPU，代价大且打断录制）。
//
//  为什么按 Model 指针去重：
//    同一个模型可以被多个 ModelInstance 共享（RendererDemoApp 允许加多个实例）。
//    几何是静态的，复制一份即可；不同实例只在 ObjectToWorld 上不同。
// ============================================================================
bool Renderer::RayTracing::RaytracingScene::RebuildGeometry(
	GraphicsContext& gfxContext,
	const std::vector<Scene::Model::ModelInstance*>& models)
{
	if (g_DeviceDXR == nullptr)
	{
		LOG_INFO(L"[DXR] RebuildGeometry 跳过：g_DeviceDXR 为空（设备不支持 DXR）\n");
		return false;
	}

	DestroyBlasBuffers();
	DestroySceneBuffers();

	// ------------------------------------------------------------------
	//  1. 收集唯一模型，算每块几何在合并缓冲里的基址
	// ------------------------------------------------------------------
	struct ModelPlacement
	{
		const Scene::Model::Model* Model;
		uint64_t BaseOffset;
		uint64_t ByteSize;
	};

	std::vector<ModelPlacement> placements;
	std::vector<Scene::Model::ModelInstance*> liveModels;

	uint64_t totalBytes = 0;
	for (Scene::Model::ModelInstance* instance : models)
	{
		if (instance == nullptr || instance->IsNull())
			continue;

		const Scene::Model::Model* model = instance->GetModel();
		if (model == nullptr || model->m_NumMeshes == 0)
			continue;

		const uint64_t size = model->m_DataBuffer.GetBufferSize();
		if (size == 0)
			continue;

		liveModels.push_back(instance);

		const bool alreadyPlaced = std::any_of(placements.begin(), placements.end(),
			[model](const ModelPlacement& p) { return p.Model == model; });
		if (alreadyPlaced)
			continue;

		// 每块几何按 256 字节对齐，保证 BLAS 的顶点/索引地址天然满足对齐要求。
		ModelPlacement placement;
		placement.Model = model;
		placement.BaseOffset = AlignUp64(totalBytes, kASByteAlignment);
		placement.ByteSize = size;
		placements.push_back(placement);
		totalBytes = placement.BaseOffset + size;
	}

	if (placements.empty())
	{
		LOG_INFO(L"[DXR] RebuildGeometry：场景里没有可用几何（模型数=%zu）\n", models.size());
		return false;
	}

	totalBytes = AlignUp64(totalBytes, kASByteAlignment);

	// ------------------------------------------------------------------
	//  2. 建合并几何缓冲并逐模型复制
	// ------------------------------------------------------------------
	m_SceneGeometry = std::make_unique<RaytracingBuffer>();
	m_SceneGeometry->Create(L"Raytracing Scene Geometry", (uint32_t)totalBytes, 1);

	for (const ModelPlacement& placement : placements)
	{
		// 源缓冲是模型自己的 m_DataBuffer（DEFAULT 堆，创建后为 GENERIC_READ）。
		// CopyBufferRegion 内部会临时把源切到 COPY_SOURCE 再恢复，见 CommandContext.cpp:647-660。
		Scene::Model::Model* mutableModel = const_cast<Scene::Model::Model*>(placement.Model);
		gfxContext.CopyBufferRegion(*m_SceneGeometry, (size_t)placement.BaseOffset,
			mutableModel->m_DataBuffer, 0, (size_t)placement.ByteSize);
	}

	// 合并缓冲被 BLAS 构建与命中着色器读取，统一停在 NON_PIXEL_SHADER_RESOURCE。
	gfxContext.TransitionResource(*m_SceneGeometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// ------------------------------------------------------------------
	//  3. 逐 (实例, mesh, draw) 建单几何 BLAS 描述
	//     同时填充每实例数据（供命中着色器索引顶点）
	// ------------------------------------------------------------------
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
	std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> blasInputs;
	std::vector<Renderer::RayTracing::RaytracingInstanceData> instanceData;

	geometries.reserve(64);
	blasInputs.reserve(64);
	instanceData.reserve(64);

	auto findBaseOffset = [&placements](const Scene::Model::Model* model) -> uint64_t
	{
		for (const ModelPlacement& p : placements)
			if (p.Model == model)
				return p.BaseOffset;
		return 0;
	};

	uint32_t triangleCount = 0;

	for (Scene::Model::ModelInstance* instance : liveModels)
	{
		const Scene::Model::Model* model = instance->GetModel();
		const uint64_t modelBase = findBaseOffset(model);
		const uint64_t geometryVA = m_SceneGeometry->GetGpuVirtualAddress();

		const uint8_t* pMesh = model->m_MeshData.get();
		for (uint32_t meshIndex = 0; meshIndex < model->m_NumMeshes; ++meshIndex)
		{
			const Scene::Model::Mesh& mesh = *(const Scene::Model::Mesh*)pMesh;

			const uint32_t indexStride = (mesh.ibFormat == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
			const uint32_t indexFormatCode = (mesh.ibFormat == DXGI_FORMAT_R32_UINT) ? 1u : 0u;
			const uint32_t vertexCount = mesh.vbStride > 0 ? (mesh.vbSize / mesh.vbStride) : 0u;

			for (uint16_t drawIndex = 0; drawIndex < mesh.numDraws; ++drawIndex)
			{
				const Scene::Model::Mesh::Draw& draw = mesh.draw[drawIndex];
				const uint32_t primitiveCount = draw.primCount / 3;
				if (primitiveCount == 0 || vertexCount == 0)
					continue;

				// 把 startIndex / baseVertex 烘焙进地址，这样每个 BLAS 是自包含的，
				// 命中着色器只需 PrimitiveIndex() 就能定位三角形（Tier 1.0 无 GeometryIndex）。
				const uint64_t indexAddress = geometryVA + modelBase + mesh.ibOffset
					+ (uint64_t)draw.startIndex * indexStride;
				const uint64_t vertexAddress = geometryVA + modelBase + mesh.vbOffset
					+ (uint64_t)draw.baseVertex * mesh.vbStride;

				D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
				geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				geometry.Triangles.Transform3x4 = 0;
				geometry.Triangles.IndexFormat = (DXGI_FORMAT)mesh.ibFormat;
				geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				geometry.Triangles.IndexCount = draw.primCount;
				geometry.Triangles.VertexCount = vertexCount;
				geometry.Triangles.IndexBuffer = indexAddress;
				geometry.Triangles.VertexBuffer.StartAddress = vertexAddress;
				geometry.Triangles.VertexBuffer.StrideInBytes = mesh.vbStride;
				geometries.push_back(geometry);

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
				inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				inputs.NumDescs = 1;
				inputs.pGeometryDescs = &geometries.back();
				blasInputs.push_back(inputs);

				Renderer::RayTracing::RaytracingInstanceData data = {};
				data.VertexOffset = (uint32_t)(modelBase + mesh.vbOffset + (uint64_t)draw.baseVertex * mesh.vbStride);
				data.IndexOffset = (uint32_t)(modelBase + mesh.ibOffset + (uint64_t)draw.startIndex * indexStride);
				data.VertexStride = mesh.vbStride;
				data.IndexFormat = indexFormatCode;
				data.StartIndex = 0;
				data.BaseVertex = 0;
				data.PrimitiveCount = primitiveCount;
				data.InstanceId = (uint32_t)instanceData.size();
				instanceData.push_back(data);

				triangleCount += primitiveCount;
			}

			pMesh += sizeof(Scene::Model::Mesh) + (mesh.numDraws - 1) * sizeof(Scene::Model::Mesh::Draw);
		}
	}

	if (blasInputs.empty())
	{
		LOG_INFO(L"[DXR] RebuildGeometry：模型存在但没有可构建的三角形（三角数=0）\n");
		return false;
	}

	// ------------------------------------------------------------------
	//  4. 分配 BLAS 结果缓冲与 scratch
	//     scratch 各段互不重叠 —— 同一条命令列表里连续 Build 无需中间 UAV barrier
	//     （重叠才有写后写冲突）。
	// ------------------------------------------------------------------
	m_BlasBuffers.resize(blasInputs.size());
	m_BlasScratchOffsets.resize(blasInputs.size());

	uint64_t scratchCursor = 0;
	for (size_t i = 0; i < blasInputs.size(); ++i)
	{
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		g_DeviceDXR->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs[i], &info);

		m_BlasBuffers[i] = std::make_unique<RaytracingBuffer>();
		m_BlasBuffers[i]->CreateAccelerationStructure(L"Raytracing BLAS", info.ResultDataMaxSizeInBytes);

		scratchCursor = AlignUp64(scratchCursor, kASByteAlignment);
		m_BlasScratchOffsets[i] = scratchCursor;
		scratchCursor += AlignUp64(info.ScratchDataSizeInBytes, kASByteAlignment);
	}

	// ------------------------------------------------------------------
	//  5. 提交 BLAS 构建
	// ------------------------------------------------------------------
	//  注意：这里**没有**对 BLAS 结果缓冲做任何 TransitionResource。
	//  AS 资源在 CreateAccelerationStructure 里就已经是
	//  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE，
	//  而 D3D12 禁止用 ResourceBarrier 把资源"转换进"该状态
	//  （Debug Layer ERROR #526）。详见 RaytracingBuffer::CreateAccelerationStructure。
	m_Scratch = std::make_unique<RaytracingBuffer>();
	// scratch 先按临时大小建，后面 TLAS 需要更多时再重建（见下）。
	m_Scratch->Create(L"Raytracing Scratch", (uint32_t)std::max<uint64_t>(scratchCursor, 256), 1);
	gfxContext.TransitionResource(*m_Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 关键：BuildRaytracingAccelerationStructure **不会**像 DrawInstanced 那样
	// 自动提交挂起的屏障。TransitionResource 只是把屏障压进
	// m_ResourceBarrierBuffer[16]，不到 16 个就不 flush。若这里不手动 flush，
	// 合并几何缓冲还停在 COPY_DEST（CopyBufferRegion 留下的），
	// Build 会直接报
	//   ERROR #538 INVALID_SUBRESOURCE_STATE:
	//   "'Raytracing Scene Geometry' ... is invalid for use as a
	//    acceleration structure geometry vertex/index data.
	//    Expected: NON_PIXEL_SHADER_RESOURCE, Actual: COPY_DEST"
	// 并 break（实测进程 0x87a 异常终止）。
	gfxContext.FlushResourceBarriers();

	for (size_t i = 0; i < blasInputs.size(); ++i)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
		buildDesc.Inputs = blasInputs[i];
		buildDesc.ScratchAccelerationStructureData = m_Scratch->GetGpuVirtualAddress() + m_BlasScratchOffsets[i];
		buildDesc.DestAccelerationStructureData = m_BlasBuffers[i]->GetGpuVirtualAddress();
		buildDesc.SourceAccelerationStructureData = 0;
		gfxContext.GetCommandList4()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	}

	// ------------------------------------------------------------------
	//  6. 每实例数据 → GPU（raw buffer，每实例 32 字节 = 8 个 DWORD）
	//     HLSL 侧用 ByteAddressBuffer + 手算偏移读取（根级 SRV 不带
	//     StructureByteStride，声明成 StructuredBuffer<T> 会读错跨距）。
	// ------------------------------------------------------------------
	const uint32_t instanceBytes = (uint32_t)(instanceData.size() * sizeof(Renderer::RayTracing::RaytracingInstanceData));
	m_InstanceDataBuffer = std::make_unique<RaytracingBuffer>();
	m_InstanceDataBuffer->Create(L"Raytracing Instance Data", instanceBytes, 1);

	// 注意：CommandContext::WriteBuffer 内部有 ASSERT(Math::IsAligned(BufferData, 16))，
	// 而 std::vector<T>::data() 只保证 alignof(T)（这里是 4）对齐，不能依赖。
	// 因此走 UploadBuffer + Map 的路径。上传缓冲必须是成员（活到 GPU 执行完），
	// 不能是局部变量 —— 否则命令列表还没提交，源资源就被销毁了。
	m_InstanceDataUpload.Create(L"Raytracing Instance Data Upload", instanceBytes);
	std::memcpy(m_InstanceDataUpload.Map(), instanceData.data(), instanceBytes);
	m_InstanceDataUpload.Unmap();
	gfxContext.CopyBufferRegion(*m_InstanceDataBuffer, 0, m_InstanceDataUpload, 0, instanceBytes);
	gfxContext.TransitionResource(*m_InstanceDataBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_InstanceCount = (uint32_t)instanceData.size();
	m_TriangleCount = triangleCount;
	m_TopLevelBuilt = false;

	LOG_INFO(L"[DXR] RebuildGeometry 完成：模型=%zu 唯一几何=%zu BLAS=%zu 三角=%u scratch=%llu B\n",
		liveModels.size(), placements.size(), m_BlasBuffers.size(), triangleCount,
		(unsigned long long)scratchCursor);

	return true;
}

// ============================================================================
//  每帧更新实例变换 + 重建 TLAS
// ----------------------------------------------------------------------------
//  BLAS 只在几何变化时重建（RebuildGeometry）；
//  这里每帧只改 TLAS 的 instance desc —— 这就是路线图 D1 要求的
//  「静态几何复用 BLAS，只在实例变换变化时更新 TLAS」。
// ============================================================================
void Renderer::RayTracing::RaytracingScene::UpdateInstances(
	GraphicsContext& gfxContext,
	const std::vector<Scene::Model::ModelInstance*>& models)
{
	// 注意：这里**不能**用 IsValid() 当前置条件 —— IsValid() 要求
	// m_TopLevel != nullptr，而 m_TopLevel 正是本函数（经由 RebuildTopLevel）
	// 第一次创建的。用 IsValid() 会导致「首帧永远建不出 TLAS」的鸡生蛋问题，
	// 表现为 Render 里 GetTopLevelBuffer() 返回 nullptr 后解引用崩溃
	// （实测 c0000005 @ GpuResource::GetGpuVirtualAddress）。
	// 唯一真正需要的前置条件是 BLAS 已经建好（每实例数据也随 BLAS 一起建）。
	if (m_BlasBuffers.empty() || m_InstanceDataBuffer == nullptr)
		return;

	// 按「实例 × mesh × draw」的顺序重建 instance desc，
	// 顺序必须与 RebuildGeometry 里填充 instanceData 的顺序完全一致，
	// 否则 InstanceID 会指向错误的几何（表现为法线颜色对不上模型）。
	uint32_t cursor = 0;
	uint32_t instanceIndex = 0;

	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
	descs.reserve(m_InstanceCount);

	for (Scene::Model::ModelInstance* instance : models)
	{
		if (instance == nullptr || instance->IsNull())
			continue;

		const Scene::Model::Model* model = instance->GetModel();
		if (model == nullptr || model->m_NumMeshes == 0)
			continue;

		// 与 RebuildGeometry 相同：空几何的模型在那边被跳过，这里也要跳过。
		if (model->m_DataBuffer.GetBufferSize() == 0)
			continue;

		const Math::Matrix4 localToWorld = instance->GetLocalToWorld();
		XMFLOAT4X4 rows;
		XMStoreFloat4x4(&rows, localToWorld);

		const uint8_t* pMesh = model->m_MeshData.get();
		for (uint32_t meshIndex = 0; meshIndex < model->m_NumMeshes; ++meshIndex)
		{
			const Scene::Model::Mesh& mesh = *(const Scene::Model::Mesh*)pMesh;
			const uint32_t vertexCount = mesh.vbStride > 0 ? (mesh.vbSize / mesh.vbStride) : 0u;

			for (uint16_t drawIndex = 0; drawIndex < mesh.numDraws; ++drawIndex)
			{
				const uint32_t primitiveCount = mesh.draw[drawIndex].primCount / 3;
				if (primitiveCount == 0 || vertexCount == 0)
					continue;

				if (cursor >= m_BlasBuffers.size())
					break;

				D3D12_RAYTRACING_INSTANCE_DESC desc = {};
				// ------------------------------------------------------------
				// 【关键】DXR 的实例变换是**列向量**约定：
				//   worldPos = mul(InstanceTransform, localPos)
				// 其中 Transform 是 3x4 行主序矩阵，**平移在第 3 列**
				// （Transform[i][3] = 平移分量），线性部分 Transform[i][j]。
				//
				// 而本项目的 Math::Matrix4 是**行向量**约定（v * M），
				// 平移在第 3 行（m[3][0..2]）。两者互为转置：
				//   Transform[i][j] = M.m[j][i]   (j < 3)
				//   Transform[i][3] = M.m[3][i]
				//
				// 曾经这里直接按行拷贝（Transform[i][j] = m[i][j]），后果是
				// **平移被整行丢掉**（m[i][3] 对仿射矩阵恒为 0），模型停留在
				// 未平移的局部坐标系里。表现极具欺骗性：BLAS/TLAS 全部构建成功、
				// Debug Layer 零报错、DispatchRays 照常提交、RayGen 也确实在写 UAV
				// （整屏是 miss 背景色），但所有射线都打不到模型 —— 因为模型
				// 根本不在相机看的那个位置（实测踩过，排查了很久）。
				// ------------------------------------------------------------
				desc.Transform[0][0] = rows.m[0][0];
				desc.Transform[0][1] = rows.m[1][0];
				desc.Transform[0][2] = rows.m[2][0];
				desc.Transform[0][3] = rows.m[3][0];
				desc.Transform[1][0] = rows.m[0][1];
				desc.Transform[1][1] = rows.m[1][1];
				desc.Transform[1][2] = rows.m[2][1];
				desc.Transform[1][3] = rows.m[3][1];
				desc.Transform[2][0] = rows.m[0][2];
				desc.Transform[2][1] = rows.m[1][2];
				desc.Transform[2][2] = rows.m[2][2];
				desc.Transform[2][3] = rows.m[3][2];

				desc.InstanceID = cursor;             // == instanceData 的下标
				desc.InstanceMask = 0xFF;
				desc.InstanceContributionToHitGroupIndex = 0;
				// 双面：CULL_DISABLE。命中着色器里已按入射方向翻转法线，
				// 因此从背面看也是同一种着色结果，不会出现整片死黑。
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				desc.AccelerationStructure = m_BlasBuffers[cursor]->GetGpuVirtualAddress();

				descs.push_back(desc);
				++cursor;
				++instanceIndex;
			}

			pMesh += sizeof(Scene::Model::Mesh) + (mesh.numDraws - 1) * sizeof(Scene::Model::Mesh::Draw);
		}
	}

	(void)instanceIndex;

	if (descs.empty())
		return;

	// instance desc 走 UPLOAD 堆：GPU 只读、CPU 每帧改写，
	// 用 UPLOAD 堆可以完全不碰状态转换（对它发 Transition 会触发 Debug Layer #741）。
	const uint32_t bytes = (uint32_t)(descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
	if (m_InstanceDescsUpload.GetBufferSize() != bytes)
		m_InstanceDescsUpload.Create(L"Raytracing Instance Descs", bytes);

	void* mapped = m_InstanceDescsUpload.Map();
	memcpy(mapped, descs.data(), bytes);
	m_InstanceDescsUpload.Unmap(0, bytes);

	RebuildTopLevel(gfxContext);
}

void Renderer::RayTracing::RaytracingScene::RebuildTopLevel(GraphicsContext& gfxContext)
{
	if (g_DeviceDXR == nullptr || m_BlasBuffers.empty())
		return;

	const uint32_t instanceCount = (uint32_t)(m_InstanceDescsUpload.GetBufferSize() / sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
	if (instanceCount == 0)
		return;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = instanceCount;
	inputs.InstanceDescs = m_InstanceDescsUpload.GetGpuVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	g_DeviceDXR->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

	// TLAS 结果缓冲尺寸变了就重建（第一帧或实例数变化）。
	if (m_TopLevel == nullptr || m_TopLevel->GetBufferSize() < info.ResultDataMaxSizeInBytes)
	{
		m_TopLevel = std::make_unique<RaytracingBuffer>();
		m_TopLevel->CreateAccelerationStructure(L"Raytracing TLAS", info.ResultDataMaxSizeInBytes);
		m_TopLevelBuilt = false;
	}

	// scratch 需要容纳 TLAS 那一段。BLAS 段已在 RebuildGeometry 时用完，
	// 这里可以把 TLAS 段直接放在 0 号位置（两者不同时使用）。
	const uint64_t tlasScratchSize = AlignUp64(info.ScratchDataSizeInBytes, kASByteAlignment);
	if (m_Scratch == nullptr || m_Scratch->GetBufferSize() < tlasScratchSize)
	{
		m_Scratch = std::make_unique<RaytracingBuffer>();
		m_Scratch->Create(L"Raytracing Scratch", (uint32_t)tlasScratchSize, 1);
		gfxContext.TransitionResource(*m_Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// TLAS 被重建 / 重复构建 → 上一帧的 Build 或 DispatchRays 可能仍在读它。
	// 这是路线图 D1「明确 AS 输出 buffer 的 UAV barrier」的落点。
	// UAV barrier 是合法的（它不是「状态转换」，不触发 #526）。
	if (m_TopLevelBuilt)
		gfxContext.InsertUAVBarrier(*m_TopLevel, true);

	// 同样**没有** TransitionResource：TLAS 创建时即为 AS 状态。

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs = inputs;
	buildDesc.ScratchAccelerationStructureData = m_Scratch->GetGpuVirtualAddress();
	buildDesc.DestAccelerationStructureData = m_TopLevel->GetGpuVirtualAddress();
	buildDesc.SourceAccelerationStructureData = 0;
	// 同 BLAS：Build 不会自动提交挂起的屏障（scratch 的 UNORDERED_ACCESS
	// 转换、instance desc 的 COPY_DEST 等都可能还压在缓冲里）。
	gfxContext.FlushResourceBarriers();
	gfxContext.GetCommandList4()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	m_TopLevelBuilt = true;

	// DispatchRays 的尺寸在 Renderer 里每帧填，这里只保证地址正确。
	m_DispatchRaysDesc.Width = 0;
	m_DispatchRaysDesc.Height = 0;
	m_DispatchRaysDesc.Depth = 1;
}

// ============================================================================
//  Shader Binding Table
// ----------------------------------------------------------------------------
//  布局（每 section 起点按 64 字节对齐，记录 32 字节）：
//    [0]   RayGeneration  32 B
//    [64]  Miss           32 B
//    [128] HitGroup       32 B
//  总 160 B，向上取整到 256。
// ============================================================================
bool Renderer::RayTracing::RaytracingScene::BuildShaderBindingTable(
	ID3D12StateObjectProperties* stateObjectProperties,
	const wchar_t* rayGenExport, const wchar_t* missExport, const wchar_t* hitGroupName)
{
	if (stateObjectProperties == nullptr)
	{
		LOG_INFO(L"[DXR] BuildShaderBindingTable 失败：StateObjectProperties 为空\n");
		return false;
	}

	constexpr uint32_t kRayGenOffset = 0;
	constexpr uint32_t kMissOffset = kRayGenOffset + kSbtSectionAlignment;
	constexpr uint32_t kHitGroupOffset = kMissOffset + kSbtSectionAlignment;
	constexpr uint32_t kTotalBytes = kHitGroupOffset + kSbtSectionAlignment; // 192

	if (m_SbtUpload.GetBufferSize() != kTotalBytes)
		m_SbtUpload.Create(L"Raytracing SBT", kTotalBytes);

	uint8_t* sbt = (uint8_t*)m_SbtUpload.Map();
	memset(sbt, 0, kTotalBytes);

	void* rayGenId = stateObjectProperties->GetShaderIdentifier(rayGenExport);
	if (rayGenId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 失败：导出 %s 没有着色器标识符\n", rayGenExport);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kRayGenOffset, rayGenId, kSbtRecordSize);

	void* missId = stateObjectProperties->GetShaderIdentifier(missExport);
	if (missId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 失败：导出 %s 没有着色器标识符\n", missExport);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kMissOffset, missId, kSbtRecordSize);

	void* hitGroupId = stateObjectProperties->GetShaderIdentifier(hitGroupName);
	if (hitGroupId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 失败：命中组 %s 没有着色器标识符\n", hitGroupName);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kHitGroupOffset, hitGroupId, kSbtRecordSize);

	m_SbtUpload.Unmap(0, kTotalBytes);

	const D3D12_GPU_VIRTUAL_ADDRESS base = m_SbtUpload.GetGpuVirtualAddress();
	m_DispatchRaysDesc.RayGenerationShaderRecord.StartAddress = base + kRayGenOffset;
	m_DispatchRaysDesc.RayGenerationShaderRecord.SizeInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.MissShaderTable.StartAddress = base + kMissOffset;
	m_DispatchRaysDesc.MissShaderTable.SizeInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.MissShaderTable.StrideInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.HitGroupTable.StartAddress = base + kHitGroupOffset;
	m_DispatchRaysDesc.HitGroupTable.SizeInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.HitGroupTable.StrideInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.CallableShaderTable = {};
	m_DispatchRaysDesc.Depth = 1;

	LOG_INFO(L"[DXR] SBT 构建完成：RayGen=%s Miss=%s HitGroup=%s 记录=%u B\n",
		rayGenExport, missExport, hitGroupName, kSbtRecordSize);
	return true;
}

void Renderer::RayTracing::RaytracingScene::DestroyBlasBuffers()
{
	for (auto& blas : m_BlasBuffers)
		blas.reset();
	m_BlasBuffers.clear();
	m_BlasScratchOffsets.clear();
}

void Renderer::RayTracing::RaytracingScene::DestroySceneBuffers()
{
	m_TopLevel.reset();
	m_Scratch.reset();
	m_SceneGeometry.reset();
	m_InstanceDataBuffer.reset();
	m_InstanceDescsUpload.Destroy();
	m_InstanceDataUpload.Destroy();
	m_SbtUpload.Destroy();

	m_DispatchRaysDesc = {};
	m_InstanceCount = 0;
	m_TriangleCount = 0;
	m_ScratchSize = 0;
	m_TlasScratchOffset = 0;
	m_TopLevelBuilt = false;
}

void Renderer::RayTracing::RaytracingScene::Destroy()
{
	DestroyBlasBuffers();
	DestroySceneBuffers();
}
