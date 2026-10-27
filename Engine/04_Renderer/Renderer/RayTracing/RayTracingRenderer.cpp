#include "00_Core/PCH.h"

#include "RayTracingRenderer.h"

#include "04_Renderer/Renderer/Base/RendererRegistry.h"
#include "04_Renderer/Renderer/RendererConfig.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "04_Renderer/BufferManager.h"
#include "04_Renderer/RenderParams.h"
#include "05_Scene/Model/Model.h"

#include "RayTracing.h"

using namespace Math;
using namespace Graphics;
using namespace Renderer;
using namespace Renderer::RayTracing;

// ============================================================================
//  根签名槽位（全局根签名，给 RTPSO 用）
// ----------------------------------------------------------------------------
//  DXR 的全局根签名与光栅根签名的差别：
//    * 没有 Vertex/Pixel 可见性之分，只有 ALL。
//    * 加速结构、几何缓冲、实例数据都用**根级 SRV**（ROOT_PARAMETER_TYPE_SRV），
//      直接传 GPU 虚拟地址，不需要描述符表 —— 这样 SBT / 堆管理都更简单。
//    * 输出图用描述符表（UAV），因为 RWTexture2D 必须走描述符。
// ============================================================================
namespace
{
	enum GlobalRootBindings
	{
		kGlobalRootScene = 0,        // t0：TLAS（根级 SRV，给的是 AS 的 GPU 地址）
		kGlobalRootSceneData,        // t1：聚合几何（根级 SRV）
		kGlobalRootInstanceData,     // t2：每实例数据（根级 SRV）
		kGlobalRootOutputTable,      // u0：输出 UAV（描述符表，1 个）
		kGlobalRootConstants,        // b0：根常量 52 DWORD（R6 48 → R7 52）
		kGlobalRootMaterialTextures, // t3：无界 SRV 表（BaseColor/MR/AO/Emissive/Normal）
		kNumGlobalRootBindings
	};

	// 根常量个数。**改这里必须同时改**：
	//   * HLSL 的 cbuffer RaytracingConstants 字段数
	//   * RayTracingRenderer.h 的 RaytracingConstants（有 static_assert 挡着）
	//   * Render() 里的 SetConstantArray 实参
	// 三处任一漏改的后果都一样：后续字段整体错位，画面异常但**不报错**。
	constexpr uint32_t kGlobalRootConstantDwords = 52;

	// 输出 UAV 在 s_TextureHeap 里的槽位。
	// 【R7 关键改动】输出 UAV 与材质 SRV 现在共用同一个 shader-visible 堆：
	//   D3D12 规定一个命令列表同时只能绑定**一个** CBV_SRV_UAV 堆，
	//   而 DispatchRays 必须在同一份根签名下既写输出又采样贴图。
	//   槽 0 留作输出 UAV，材质 SRV 从槽 1 起由 ModelLoader 的 Alloc 顺序占用。
	constexpr uint32_t kOutputUavSlot = 0;

	// 材质贴图 SRV 表的起始槽位。ModelLoader::LoadMaterials 每材质 Alloc(5)，
	// 所以第一张表拿到的偏移就是 5（0 号被输出 UAV 占了）。
	// 这里**不硬编码 5**：RefreshDescriptorHeap 只负责占住 0 号槽，
	// 真正的基址由每实例数据里的 SrvTableIndex 传给 shader。
	constexpr uint32_t kMaterialSrvsStartSlot = 1;

	// 静态采样器的寄存器号。DXR 全局根签名里给材质贴图用 s0，
	// 采样参数与光栅路径的 GetSampler 默认值一致（各向异性 16x / WRAP / LOD 全范围），
	// 这样同一张贴图在两条路径下取到的是同一个纹素。
	constexpr uint32_t kMaterialSamplerRegister = 0;
}

// ============================================================================
//  Initialize
// ============================================================================
bool RayTracingRenderer::Initialize(const RendererCreateDesc& desc)
{
	m_CreateDesc = desc;

	// ---- 0. 设备能力前置检查 -------------------------------------------
	// 路线图 D0：「无 DXR 支持时禁用相应 UI 项并继续运行光栅模式」。
	// 这里返回 false 而不是崩溃 —— RendererDemoApp::SwitchRenderer 会在
	// Initialize 失败时 reset 掉本渲染器并保持当前渲染器不变。
	if (!g_bRaytracingSupported || g_DeviceDXR == nullptr)
	{
		LOG_INFO(L"[DXR] RayTracingRenderer::Initialize 中止：设备不支持 DXR"
			L"（supported=%d device=%p）\n",
			g_bRaytracingSupported ? 1 : 0, (void*)g_DeviceDXR.Get());
		return false;
	}

	m_MainViewport = { 0.0f, 0.0f, (float)desc.width, (float)desc.height, 0.0f, 1.0f };
	m_MainScissor = { 0, 0, (LONG)desc.width, (LONG)desc.height };

	// ---- 1. 输出缓冲 ---------------------------------------------------
	if (!CreateOutputTargets(desc.width, desc.height))
		return false;

	// ---- 2. 全局根签名 -------------------------------------------------
	if (!BuildRootSignature())
		return false;

	// ---- 3. 全局材质堆（R7 起输出 UAV 也并入这里） ----------------------
	// 为什么输出 UAV 和材质 SRV 必须共用同一个堆：
	//   D3D12 规定一个命令列表同时只能绑定一个 CBV_SRV_UAV 堆。
	//   DispatchRays 需要在同一份根签名下既写 SceneColor（UAV）又采样 BaseColor
	//   贴图（SRV），若两者在不同的堆里就只能二选一 —— 表现为贴图全黑或输出不写。
	//
	// 槽位分配：0 号给输出 UAV，1 号起由 ModelLoader::LoadMaterials 的 Alloc(5)
	// 顺序占用（每材质连续 5 个）。因此材质表的基址从 1*5 开始，
	// 但 shader 侧不假设这个值 —— 每实例数据里带 SrvTableIndex。
	//
	// 顺带一提：ModelLoader 会无条件执行 s_TextureHeap.Alloc(kNumTextures) 与
	// s_SamplerHeap.Alloc(kNumTextures)（见 ModelLoader.cpp:103 / 185），
	// 这两个堆归 Renderer 所有，Deferred/Forward 在各自 Initialize 里 Create。
	// 若本渲染器不建，切换后 LoadModelByIndex 就会在已 Destroy 的空堆上 Alloc，
	// HasAvailableSpace 断言失败 → STATUS_BREAKPOINT。
	s_TextureHeap.Create(L"RayTracing Texture Descriptors",
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
	s_SamplerHeap.Create(L"RayTracing Sampler Descriptors",
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

	// 占住 0 号槽作为输出 UAV。必须在模型加载（ModelLoader 的 Alloc）之前完成，
	// 否则 0 号会被第一个材质的 BaseColor 抢走。
	const DescriptorHandle outputUav = s_TextureHeap.Alloc(1);
	m_OutputUavOffset = s_TextureHeap.GetOffsetOfHandle(outputUav);
	RefreshDescriptorHeap();

	// 采样器：材质采样走**根签名里的静态采样器** s0（见 BuildRootSignature），
	// 因此不需要从 s_SamplerHeap 取描述符。s_SamplerHeap 仍然必须 Create ——
	// ModelLoader 会无条件在里面 Alloc(kNumTextures)，堆不存在就会断言失败。

	// ---- 4. State Object（RTPSO） --------------------------------------
	if (!BuildStateObject())
		return false;

	m_Initialized = true;
	LOG_INFO(L"[DXR] RayTracingRenderer 初始化完成：%ux%u，tier=%d，"
		L"RTPSO=%s\n",
		desc.width, desc.height, (int)g_RaytracingTier,
		m_StateObject.IsValid() ? L"OK" : L"FAILED");
	return true;
}

// ============================================================================
//  Shutdown
// ============================================================================
void RayTracingRenderer::Shutdown()
{
	// 顺序很关键：
	//   1) 先等 GPU 空闲 —— 否则 GPU 可能还在用下面要销毁的 AS / 缓冲。
	//   2) 销毁场景（BLAS/TLAS/scratch/instance data）。
	//   3) 销毁 State Object —— 它引用着全局根签名，必须早于根签名销毁。
	//   4) 最后销毁根签名与输出缓冲。
	if (g_Device != nullptr)
		g_CommandManager.IdleGPU();

	m_Scene.Destroy();
	m_StateObject.Destroy();
	m_StateObjectReady = false;
	m_GeometryReady = false;
	m_GeometrySignature.clear();

	// R7 起输出 UAV 与材质 SRV 共用一个堆（s_TextureHeap），
	// 因此这里只需要销毁那一个堆 —— 不再有独立的 m_DescriptorHeap。
	m_OutputUavOffset = 0;

	// 与 Deferred/Forward 对称：Shutdown 时销毁全局材质堆。
	s_TextureHeap.Destroy();
	s_SamplerHeap.Destroy();

	DestroyOutputTargets();

	m_Initialized = false;
	LOG_INFO(L"[DXR] RayTracingRenderer 已关闭（累计 DispatchRays 帧数=%u）\n", m_DispatchedFrames);
	m_DispatchedFrames = 0;
}

// ============================================================================
//  OnResize
// ============================================================================
void RayTracingRenderer::OnResize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	m_MainViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	m_MainScissor = { 0, 0, (LONG)width, (LONG)height };

	m_CreateDesc.width = width;
	m_CreateDesc.height = height;

	// 加速结构只依赖几何与实例变换，与分辨率无关 —— 这里不动 m_Scene。
	g_CommandManager.IdleGPU();
	DestroyOutputTargets();
	CreateOutputTargets(width, height);
	RefreshDescriptorHeap();
}

// ============================================================================
//  输出缓冲
// ============================================================================
bool RayTracingRenderer::CreateOutputTargets(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return false;

	// 格式与 Deferred 的 SceneColorBufferFormat 保持一致（R8G8B8A8_UNORM），
	// 这样 Display::PreparePresentSDR 的 IsCopyCompatible 家族比较能通过，
	// DXR 结果直接走统一 Present 路径。
	m_SceneColorBuffer = BufferManager::CreateColorBuffer(
		L"RayTracing Scene Color",
		width, height,
		Renderer::Deferred::SceneColorBufferFormat,
		1, nullptr);

	if (m_SceneColorBuffer == nullptr)
	{
		LOG_INFO(L"[DXR] 创建输出缓冲失败：%ux%u\n", width, height);
		return false;
	}
	return true;
}

void RayTracingRenderer::DestroyOutputTargets()
{
	if (m_SceneColorBuffer != nullptr)
	{
		m_SceneColorBuffer->Destroy();
		m_SceneColorBuffer.reset();
	}
}

// ============================================================================
//  输出 UAV 描述符
// ============================================================================
void RayTracingRenderer::RefreshDescriptorHeap()
{
	if (m_SceneColorBuffer == nullptr || m_SceneColorBuffer->GetResource() == nullptr)
		return;

	// ColorBuffer::CreateDerivedViews 已经为 mip0 建过 UAV（存在 CPU 堆里），
	// 但那是 CPU-only 描述符，不能直接给 shader 用。
	// 这里在 shader-visible 的 s_TextureHeap 的 kOutputUavSlot 槽位再建一份。
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = m_SceneColorBuffer->GetFormat();
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	// 用记录的偏移取槽位（不用裸指针 + 0，避免与 ModelLoader 的分配错位）。
	// 注意：s_TextureHeap 可能尚未 Create（RefreshDescriptorHeap 在 Initialize 里
	// 只被调用一次，那时堆刚建好；但 OnResize 会再调一次，堆仍在）。
	if (s_TextureHeap.GetHeapPointer() == nullptr)
		return;

	// DescriptorHandle 有 operator D3D12_CPU_DESCRIPTOR_HANDLE()，但直接当实参传
	// 会与 GetCpuPtr() 的 size_t 产生歧义，因此先显式转换到局部变量。
	const DescriptorHandle handle = s_TextureHeap[m_OutputUavOffset];
	const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = handle;
	g_Device->CreateUnorderedAccessView(
		m_SceneColorBuffer->GetResource(), nullptr, &uavDesc, cpuHandle);
}

// ============================================================================
//  全局根签名
// ============================================================================
bool RayTracingRenderer::BuildRootSignature()
{
	// 第二参数 = 静态采样器个数。R7 起需要 1 个（材质贴图 s0）。
	// 注意 RootSignature::InitStaticSampler 会把 BorderColor 硬写成
	// D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE（这是该类的既有实现）。
	m_GlobalRootSig.Reset(kNumGlobalRootBindings, 1);

	// 注意：DXR 的全局根签名里所有参数都是 ALL 可见性。
	m_GlobalRootSig[kGlobalRootScene].InitAsBufferSRV(0, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootSceneData].InitAsBufferSRV(1, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootInstanceData].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootOutputTable].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootConstants].InitAsConstants(0, kGlobalRootConstantDwords,
		D3D12_SHADER_VISIBILITY_ALL);

	// R7：材质贴图 SRV 表（t3 起，无界）。
	// 【为什么必须无界】15 个材质 × 5 张贴图 = 75 个描述符，且数量随模型变化。
	//   写成固定 5 会让 shader 里的下标越界（同一张表内只能访问前 5 个）。
	// 【为什么用 D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND】表格起点由
	//   SetDescriptorTable 传入的 GPU handle 决定，range 内部偏移从 0 起。
	m_GlobalRootSig[kGlobalRootMaterialTextures].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, UINT_MAX, D3D12_SHADER_VISIBILITY_ALL);

	// 静态采样器 s0。参数与 ModelLoader::GetSampler 的默认值保持一致
	// （各向异性 16x / WRAP / LOD 全范围），保证两条路径取样同一纹素。
	{
		D3D12_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.MipLODBias = 0.0f;
		samplerDesc.MaxAnisotropy = 16;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		samplerDesc.MinLOD = 0.0f;
		samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
		m_GlobalRootSig.InitStaticSampler(kMaterialSamplerRegister, samplerDesc,
			D3D12_SHADER_VISIBILITY_ALL);
	}

	// DXR 全局根签名不需要 INPUT_ASSEMBLER 标志。
	m_GlobalRootSig.Finalize(L"RayTracingRenderer Global Root Signature",
		D3D12_ROOT_SIGNATURE_FLAG_NONE);

	if (m_GlobalRootSig.GetSignature() == nullptr)
	{
		LOG_INFO(L"[DXR] 全局根签名创建失败\n");
		return false;
	}
	return true;
}

// ============================================================================
//  State Object
// ============================================================================
bool RayTracingRenderer::BuildStateObject()
{
	RaytracingStateObjectDesc desc;
	desc.DXILLibrary = RayTracing_cso;
	desc.DXILLibrarySize = sizeof(RayTracing_cso);

	// lib_6_4 编译出的每个 [shader("...")] 都是一个可导出符号。
	// 全部登记，让 State Object 把它们都保留下来。
	//
	// 【关键】ShadowMiss 必须出现在这个导出列表里，否则它不会进入 DXIL 库的
	// 「可导出」集合，SBT 的 miss 表第二条记录拿到的就是全零标识符 ——
	// 运行时表现为「阴影射线未命中时也判为被遮挡」（整个场景全黑），
	// 而 Debug Layer 只在 SBT 校验非常严格时才提一句，很容易漏看。
	desc.Exports = { L"RayGen", L"Miss", L"ShadowMiss", L"ClosestHit" };

	desc.HitGroupName = L"HitGroup";
	desc.ClosestHitExport = L"ClosestHit";
	desc.AnyHitExport = nullptr;
	desc.IntersectionExport = nullptr;

	desc.RayGenExport = L"RayGen";
	desc.MissExport = L"Miss";

	// 与 HLSL 的 RayPayload 一致：float3 Color(12) + float HitDistance(4)
	//                       + uint Depth(4) + uint _pad0(4) + float2 _pad1(8) = 32 B。
	// R5 时是 16 B（只有 Color + HitDistance），R6 加 Depth 后必须同步放大，
	// 否则 TraceRay 会**越界写**载荷（Debug Layer 报
	// "Payload is larger than the size specified in the shader config"）。
	desc.MaxPayloadSizeInBytes = 32;
	desc.MaxAttributeSizeInBytes = 8;
	// 递归深度 2：主射线(0) → ClosestHit → 二级射线(1) → ClosestHit 早退。
	// 与 HLSL 里 `if (payload.Depth >= 1u) return;` 的闸门严格对应。
	// 写 1 而代码里发二级射线，设备会直接移除进程（0x887A0005），无友好提示。
	desc.MaxTraceRecursionDepth = 2;

	desc.GlobalRootSignature = m_GlobalRootSig.GetSignature();

	if (!m_StateObject.Finalize(desc))
	{
		LOG_INFO(L"[DXR] RTPSO 创建失败：%s\n", m_StateObject.GetLastError().c_str());
		return false;
	}

	m_StateObjectReady = true;
	return true;
}

// ============================================================================
//  几何变化检测
// ----------------------------------------------------------------------------
//  签名 = 模型指针 + 数据缓冲版本号 + mesh 数。
//  ModelInstance 内部的 m_DataBuffer 版本号在模型重载时会变（GpuResource::Destroy
//  会 ++m_VersionID），因此能捕捉到「换了模型 / 重新导入几何」。
// ============================================================================
bool RayTracingRenderer::GeometryChanged(
	const std::vector<Scene::Model::ModelInstance*>& models) const
{
	std::vector<uint64_t> signature;
	signature.reserve(models.size() * 3);

	for (const Scene::Model::ModelInstance* instance : models)
	{
		if (instance == nullptr || instance->IsNull())
			continue;

		const Scene::Model::Model* model = instance->GetModel();
		if (model == nullptr)
			continue;

		signature.push_back((uint64_t)(uintptr_t)model);
		signature.push_back((uint64_t)model->m_DataBuffer.GetVersionID());
		signature.push_back((uint64_t)model->m_NumMeshes);
	}

	return signature != m_GeometrySignature;
}

// ============================================================================
//  Update：准备加速结构
// ============================================================================
void RayTracingRenderer::Update(const RenderFrameDesc& frame, GraphicsContext& gfxContext)
{
	if (!m_Initialized)
		return;

	if (frame.Models.empty())
		return;

	// ---- 1. 几何变化 → 重建 BLAS + 每实例数据 --------------------------
	if (GeometryChanged(frame.Models) || !m_GeometryReady)
	{
		if (!m_Scene.RebuildGeometry(gfxContext, frame.Models))
		{
			LOG_INFO(L"[DXR] RebuildGeometry 失败，本帧跳过 DispatchRays\n");
			m_GeometryReady = false;
			return;
		}

		// 记下签名，避免每帧重建
		m_GeometrySignature.clear();
		for (const Scene::Model::ModelInstance* instance : frame.Models)
		{
			if (instance == nullptr || instance->IsNull())
				continue;
			const Scene::Model::Model* model = instance->GetModel();
			if (model == nullptr)
				continue;
			m_GeometrySignature.push_back((uint64_t)(uintptr_t)model);
			m_GeometrySignature.push_back((uint64_t)model->m_DataBuffer.GetVersionID());
			m_GeometrySignature.push_back((uint64_t)model->m_NumMeshes);
		}
		m_GeometryReady = true;

		// BLAS 刚建完，SBT 还没建（依赖 State Object，已在 Initialize 建好）
		// R6：miss 表需要两条记录 —— Miss（主/反射）与 ShadowMiss（阴影）。
		m_Scene.BuildShaderBindingTable(m_StateObject.GetProperties(),
			L"RayGen", L"Miss", L"ShadowMiss", L"HitGroup");

		// 关键：RebuildGeometry 只建 BLAS 与每实例数据，**不建 TLAS**。
		// TLAS 的构建在 UpdateInstances 里（它同时写 instance desc）。
		// 若这里不补这一步，m_TopLevel 会一直是 nullptr，Render 里
		//   m_Scene.GetTopLevelBuffer()->GetGpuVirtualAddress()
		// 就是空指针解引用（实测 c0000005 @ GpuResource::GetGpuVirtualAddress）。
		m_Scene.UpdateInstances(gfxContext, frame.Models);

		LOG_INFO(L"[DXR] 加速结构就绪：BLAS=%u TLAS实例=%u 三角=%u\n",
			m_Scene.GetBlasCount(), m_Scene.GetInstanceCount(), m_Scene.GetTriangleCount());
	}
	else
	{
		// ---- 2. 几何没变 → 只刷新实例变换并重建 TLAS -------------------
		// 这是路线图 D1「静态几何尽量复用 BLAS，只在实例变换变化时更新 TLAS」
		// 的落点。UpdateInstances 内部会重建 TLAS。
		m_Scene.UpdateInstances(gfxContext, frame.Models);
	}
}

// ============================================================================
//  Render：DispatchRays
// ============================================================================
void RayTracingRenderer::Render(GraphicsContext& context, const RenderFrameDesc& frame,
	DrawPass pass, BatchType batchType)
{
	(void)batchType;

	// DXR 只有一遍，kZPass / kTransparent 都由 RendererDemo 之外的路径
	// 决定是否调用 —— 这里只在 kOpaque 发一次 DispatchRays。
	if (pass != kOpaque)
		return;

	if (!m_Initialized || !m_GeometryReady || !m_StateObjectReady)
		return;

	if (m_SceneColorBuffer == nullptr || m_SceneColorBuffer->GetResource() == nullptr)
		return;

	if (frame.Camera == nullptr)
		return;

	const D3D12_DISPATCH_RAYS_DESC* dispatchDesc = m_Scene.GetDispatchRaysDesc();
	// 只校验 SBT 是否就绪（RayGen 记录地址非 0）。
	// **不要**在这里校验 Width/Height —— 它们由本函数下面按输出尺寸填，
	// 此时还是 RebuildTopLevel 留下的占位 0。
	if (dispatchDesc == nullptr || dispatchDesc->RayGenerationShaderRecord.StartAddress == 0)
	{
		LOG_INFO(L"[DXR] DispatchRays 跳过：SBT 尚未就绪\n");
		return;
	}

	context.PIXBeginEvent(L"RayTracingRenderer");

	// ---- 1. 输出缓冲切到 UAV ------------------------------------------
	context.TransitionResource(*m_SceneColorBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ---- 2. 绑定全局根签名 --------------------------------------------
	// 【关键】DispatchRays 读取的是**计算根签名**（compute root signature）槽位，
	// 不是图形根签名。D3D12 把光追着色器归到 compute 侧的根参数空间，
	// 官方 D3D12RaytracingHelloWorld 也是 SetComputeRootSignature +
	// SetComputeRootDescriptorTable + DispatchRays 这一套。
	//
	// 如果这里误用 GraphicsContext::SetRootSignature（SetGraphicsRootSignature），
	// 现象极具欺骗性：状态对象能建、SBT 能建、DispatchRays 不报任何错、
	// 日志一切正常，但所有根参数都是未定义值 —— TLAS 指针为 null（射线打不到
	// 任何东西，连 miss 着色器都不跑）、输出 UAV 描述符表为 null（写入被丢弃），
	// 最终截图纯黑，连「未命中背景色」都看不到。
	ComputeContext& compute = context.GetComputeContext();
	compute.SetRootSignature(m_GlobalRootSig);

	// 描述符堆是命令列表级状态，图形/计算共用，仍通过 context 设置。
	// R7：这个堆现在是 s_TextureHeap —— 它同时装着输出 UAV（槽 0）与所有材质
	// BaseColor/MR/AO/Emissive/Normal 的 SRV，所以同一次 DispatchRays 里
	// 既能写输出又能采样贴图。
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		s_TextureHeap.GetHeapPointer());

	// ---- 3. 绑定资源 ---------------------------------------------------
	// 根级 SRV 直接给 GPU 虚拟地址（不是描述符句柄）。
	//
	// TLAS 特别注意：加速结构资源的状态是 RAYTRACING_ACCELERATION_STRUCTURE，
	// 而 SetBufferSRV 里有
	//   ASSERT(m_UsageState & (PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE))
	// 这个断言是为普通 buffer 准备的，AS 永远不会处于那两个状态 ——
	// 因此这里直接下发 SetComputeRootShaderResourceView，绕开该断言。
	// （DXR 官方写法也是直接给 AS 的 GPU 虚拟地址，AS 不需要也不允许做状态转换。）
	compute.GetCommandList()->SetComputeRootShaderResourceView(
		kGlobalRootScene, m_Scene.GetTopLevelBuffer()->GetGpuVirtualAddress());

	compute.SetBufferSRV(kGlobalRootSceneData, *m_Scene.GetSceneGeometryBuffer());
	compute.SetBufferSRV(kGlobalRootInstanceData, *m_Scene.GetInstanceDataBuffer());

	// 输出 UAV 走描述符表（RWTexture2D 必须用描述符，不能用根 UAV）
	compute.SetDescriptorTable(kGlobalRootOutputTable,
		(D3D12_GPU_DESCRIPTOR_HANDLE)s_TextureHeap[m_OutputUavOffset]);

	// ---- 3b. 材质贴图 SRV 表（R7）--------------------------------------
	// 整张堆从槽 0 起一次性传给 shader，shader 侧用
	//    g_Textures[NonUniformResourceIndex(每实例 SrvTableIndex + 槽位)]
	// 定位任意材质的任意贴图。
	//
	// 【为什么整张堆而不是每材质一张表】DispatchRays 一次派发覆盖全屏，
	// 相邻像素命中不同材质，无法在派发前为某个材质单独绑表。
	// 这也是本方案选「无界数组 + 稳定下标」而不是「SBT 本地根签名」的原因
	// （后者留作演进，见设计方案 §6 D6）。
	//
	// 注意：GetHeapPointer() 返回的是 ID3D12DescriptorHeap*（对象指针），
	// **不是** GPU 描述符句柄。描述符表的起点必须用 operator[0] 取到的
	// DescriptorHandle 再转成 GPU handle —— 两者强转能编译通过但值完全错，
	// 运行时表现为所有贴图采样越界（返回 0 → 全黑模型）。
	compute.SetDescriptorTable(kGlobalRootMaterialTextures,
		(D3D12_GPU_DESCRIPTOR_HANDLE)s_TextureHeap[0]);

	// ---- 4. 根常量 -----------------------------------------------------
	RaytracingConstants constants = {};
	constants.InvViewProj = frame.Camera->GetInViewProjMatrix();
	constants.CameraPosition = Vector4(frame.Camera->GetPosition());
	const float width = (float)m_CreateDesc.width;
	const float height = (float)m_CreateDesc.height;
	constants.OutputSize = Vector4(width, height, width > 0.0f ? 1.0f / width : 0.0f,
		height > 0.0f ? 1.0f / height : 0.0f);

	// 显示参数直接取 RenderParams，保证与光栅路径同一套 tonemap/sRGB 开关。
	const RenderParams& params = frame.Params ? *frame.Params : RenderParams{};
	constants.DisplayParams = Vector4(params.Exposure,
		params.EnableTonemap ? 1.0f : 0.0f,
		params.EnableSRGBEncode ? 1.0f : 0.0f,
		0.0f);
	constants.DebugParams = Vector4(kZero);

	// ---- R6 / D3：灯光与光追参数 ---------------------------------------
	// 方向光直接取 RenderParams —— 与 DeferredRenderer / LightingSystem 同一真源。
	//
	// 【语义】RenderParams::SunDirection 是**光的传播方向**，不是"指向光源的方向"。
	// LightingPS.hlsl:118 里是 `float3 dirL = normalize(-DirectionIntensity.xyz);`。
	// 这里原样上传传播方向，由 HLSL 的 ClosestHit 自己取反 —— 两侧保持同一约定，
	// 谁都不做"顺手翻一下"，否则两条路径的主光会一上一下（这种 bug 在单张
	// 截图里很难判断是"光反了"还是"法线反了"）。
	constants.LightDirection = Vector4(params.SunDirection[0], params.SunDirection[1],
		params.SunDirection[2], params.EnableSunLight ? params.SunIntensity : 0.0f);
	constants.LightColor = Vector4(params.SunColor[0], params.SunColor[1],
		params.SunColor[2], 0.0f);

	constants.RTParams = Vector4(
		params.RTShadowNormalBias,       // x：法线偏差（米）
		params.RTShadowMaxDistance,      // y：阴影射线最大距离
		params.RTReflectionMaxDistance,  // z：反射射线最大距离
		params.RTReflectionIntensity);   // w：反射强度倍率

	// 关掉太阳时阴影没有意义（toLight 方向还在，但 radiance 为 0），
	// 这里仍允许阴影射线照常发射 —— 因为调试通道 2 要能单独看"几何遮挡关系"，
	// 与光照强度解耦。是否发射只由 EnableRTShadow 决定。
	constants.RTParams2 = Vector4(
		params.EnableRTShadow ? 1.0f : 0.0f,
		params.EnableRTReflection ? 1.0f : 0.0f,
		(float)(uint32_t)params.RTDebugView,
		0.0f);

	// ---- R7 / D4：材质接驳的两个正交开关 --------------------------------
	// x = EnableMaterialTextures：关掉后 HLSL 里 albedo 恒为 (0.8,0.8,0.8)，
	//     于是着色路径逐指令回到 R6 —— 这是验收项 A5a（md5 必须等于
	//     a43b6059…）能够成立的前提。
	// y = EnableGGX：关掉后回退 R6 的 `diffuse + albedo * ambient` 表达式。
	constants.RTParams3 = Vector4(
		params.EnableMaterialTextures ? 1.0f : 0.0f,
		params.EnableGGX ? 1.0f : 0.0f,
		0.0f,
		0.0f);

	// 同上：根常量也必须走计算根参数槽位（SetComputeRoot32BitConstants）。
	compute.SetConstantArray(kGlobalRootConstants, kGlobalRootConstantDwords, &constants);

	// ---- 5. DispatchRays -----------------------------------------------
	// 关键：DispatchRays 的 Width/Height 必须在这里按输出尺寸填。
	// RaytracingScene 只在 BuildShaderBindingTable 时设过 SBT 三段地址，
	// RebuildTopLevel 末尾会显式把 Width/Height 清成 0（那是它的占位约定）。
	// 若这里不填，DispatchRays 派发的是 **0 条射线** ——
	// 没有任何错误、没有任何 Debug Layer 报错、日志也照常打，
	// 但 UAV 一个像素都没被写，最终截图是纯黑（实测踩过）。
	const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Width = m_CreateDesc.width;
	const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Height = m_CreateDesc.height;
	const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Depth = 1;

	// DrawInstanced 内部会先 FlushResourceBarriers() 再提交动态描述符表。
	// DispatchRays 是我们直接调 CommandList4 的，那一步不会自动发生，
	// 必须手动补 —— 否则前面 TransitionResource 攒的屏障还压在
	// m_ResourceBarrierBuffer 里没提交，UAV 写入会与旧状态冲突
	// （Debug Layer 报 RESOURCE_BARRIER_* 错误）。
	//
	// 描述符表这边不需要额外提交：SetDescriptorTable 走的是
	// SetGraphicsRootDescriptorTable(直接 GPU 句柄)，不经过 DynamicDescriptorHeap。
	context.FlushResourceBarriers();

	context.GetCommandList4()->SetPipelineState1(m_StateObject.GetStateObject());
	context.GetCommandList4()->DispatchRays(dispatchDesc);

	// ---- 6. 输出缓冲切回可被 Present 读取的状态 ------------------------
	// Display::PreparePresentSDR 会自己再做一次 COPY_SOURCE 转换，
	// 这里给 PIXEL_SHADER_RESOURCE 是为了将来做后处理/UI 叠加时可直接采样。
	context.TransitionResource(*m_SceneColorBuffer,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	context.PIXEndEvent();

	// ---- 7. 统计（供 UI 面板显示）--------------------------------------
	// 这些是**发射数**而不是命中数：命中率要靠调试通道肉眼看，
	// 逐射线回读需要额外 UAV + 读回，属性能分析工具范畴，D3 不做。
	//
	//   主射线 = 输出像素数（每个像素一条，无采样抖动）
	//   阴影射线 = 开阴影时，每条主射线在命中后发 1 条；未命中的主射线不发
	//   反射射线 = 同理
	// 因此 ShadowRays/ReflectionRays 的**上界**是 PrimaryRays，
	// 实际值只有命中像素才计入 —— 这里用"是否开启"做增量为
	// PrimaryRays 的近似，并注明这是上界而不是精确值。
	// 精确值需要 GPU 计数器，本阶段不做，UI 里也标注为「上界」。
	const uint64_t primary = (uint64_t)m_CreateDesc.width * (uint64_t)m_CreateDesc.height;
	m_Stats.PrimaryRays = primary;
	m_Stats.ShadowRays = params.EnableRTShadow ? primary : 0;
	m_Stats.ReflectionRays = params.EnableRTReflection ? primary : 0;
	m_Stats.TotalRays = m_Stats.PrimaryRays + m_Stats.ShadowRays + m_Stats.ReflectionRays;
	m_Stats.TriangleCount = m_Scene.GetTriangleCount();
	m_Stats.BlasCount = m_Scene.GetBlasCount();
	m_Stats.InstanceCount = m_Scene.GetInstanceCount();

	// 开关变化时打一条日志：让「拨了开关但画面没变」在日志里立刻可见。
	// 只在值真的翻转时打，不会每帧刷屏。
	if (params.EnableRTShadow != m_LastEnableShadow
		|| params.EnableRTReflection != m_LastEnableReflection)
	{
		m_LastEnableShadow = params.EnableRTShadow;
		m_LastEnableReflection = params.EnableRTReflection;
		LOG_INFO(L"[DXR] 光追开关：阴影=%s 反射=%s（偏差=%.4f m，阴影距离=%.1f m，"
			L"反射距离=%.1f m，反射强度=%.2f）\n",
			params.EnableRTShadow ? L"开" : L"关",
			params.EnableRTReflection ? L"开" : L"关",
			params.RTShadowNormalBias, params.RTShadowMaxDistance,
			params.RTReflectionMaxDistance, params.RTReflectionIntensity);
	}

	// R7：材质开关同理。这两个开关是 2×2 对照的**唯一**变量来源，
	// 所以必须在日志里留痕 —— 否则「A5a 的 md5 对不上」时无法判断
	// 是开关没生效还是着色代码本身不等价。
	if (params.EnableMaterialTextures != m_LastEnableMaterialTextures
		|| params.EnableGGX != m_LastEnableGGX)
	{
		m_LastEnableMaterialTextures = params.EnableMaterialTextures;
		m_LastEnableGGX = params.EnableGGX;
		LOG_INFO(L"[DXR] 材质开关：贴图=%s GGX=%s（调试通道=%s）\n",
			params.EnableMaterialTextures ? L"开" : L"关",
			params.EnableGGX ? L"开" : L"关",
			RayTracingDebugViewName(params.RTDebugView));
	}

	++m_DispatchedFrames;
	if (m_DispatchedFrames == 1)
	{
		LOG_INFO(L"[DXR] 首帧 DispatchRays 已提交：%ux%u，SBT RayGen=0x%llX\n",
			m_CreateDesc.width, m_CreateDesc.height,
			(unsigned long long)dispatchDesc->RayGenerationShaderRecord.StartAddress);
	}
}

// ============================================================================
//  EndFrame
// ============================================================================
void RayTracingRenderer::EndFrame(GraphicsContext& context, const RenderFrameDesc& frame)
{
	(void)context;
	(void)frame;

	// DXR 路径不持有「脏标记」需要清理：
	// 加速结构由 m_Scene 自己维护（几何签名比对），
	// 模型侧的 m_IsDirty 由 Deferred/Forward 的 EndFrame 负责清理。
	// 这里留空是有意的 —— 若将来加 TAA 需要清历史帧，再在此处补。
}

// ============================================================================
//  自动注册
// ----------------------------------------------------------------------------
//  注意：本项目用静态库链接，静态对象的构造函数可能被链接器丢弃，
//  因此 RendererRegistry.cpp 里还有一处**显式**注册（与 Deferred/Forward 同）。
//  这里的 auto-register 只是双保险，两处注册同一个名字是幂等的（unordered_map 覆盖）。
// ============================================================================
static RendererAutoRegister<RayTracingRenderer> s_RegisterRayTracingRenderer(L"RayTracingRenderer");
