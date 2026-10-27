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
		kGlobalRootConstants,        // b0：根常量 32 DWORD
		kNumGlobalRootBindings
	};

	// 输出 UAV 在自有堆里的槽位
	constexpr uint32_t kOutputUavSlot = 0;
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

	// ---- 3. 输出 UAV 描述符堆 ------------------------------------------
	m_DescriptorHeap.Create(L"RayTracingRenderer Output Descriptors",
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
	RefreshDescriptorHeap();

	// ---- 3b. 全局材质堆（看似与 DXR 无关，但**必须**建） ----------------
	// ModelLoader::LoadModel 会无条件执行
	//     Renderer::s_TextureHeap.Alloc(kNumTextures);
	//     Renderer::s_SamplerHeap.Alloc(kNumTextures);
	// 给每个材质分配 SRV/Sampler 描述符表（见 ModelLoader.cpp:103 / 185）。
	// 这两个堆归 Renderer 所有，Deferred/Forward 在各自 Initialize 里 Create。
	// 如果本渲染器不建，切换后 LoadModelByIndex 就会在已 Destroy 的空堆上
	// Alloc —— HasAvailableSpace 断言失败，进程直接 STATUS_BREAKPOINT。
	// DXR 路径本身不采样材质，但堆必须存在，模型才能加载成功。
	s_TextureHeap.Create(L"RayTracing Texture Descriptors",
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
	s_SamplerHeap.Create(L"RayTracing Sampler Descriptors",
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);

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

	m_DescriptorHeap.Destroy();

	// 与 Deferred/Forward 对称：Shutdown 时销毁全局材质堆。
	// 顺序上必须在模型卸载之后（SwitchRenderer 的步骤 3 先 UnloadModel，
	// 那时 TextureRef 已经不再需要采样器偏移）。
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
	// 这里在自有 shader-visible 堆里再建一份 UAV 描述符。
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = m_SceneColorBuffer->GetFormat();
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	uavDesc.Texture2D.PlaneSlice = 0;

	// DescriptorHandle 有 operator D3D12_CPU_DESCRIPTOR_HANDLE()，但直接当实参传
	// 会与 GetCpuPtr() 的 size_t 产生歧义，因此先显式转换到局部变量。
	const DescriptorHandle handle = m_DescriptorHeap[kOutputUavSlot];
	const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = handle;
	g_Device->CreateUnorderedAccessView(
		m_SceneColorBuffer->GetResource(), nullptr, &uavDesc, cpuHandle);
}

// ============================================================================
//  全局根签名
// ============================================================================
bool RayTracingRenderer::BuildRootSignature()
{
	m_GlobalRootSig.Reset(kNumGlobalRootBindings, 0);

	// 注意：DXR 的全局根签名里所有参数都是 ALL 可见性。
	m_GlobalRootSig[kGlobalRootScene].InitAsBufferSRV(0, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootSceneData].InitAsBufferSRV(1, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootInstanceData].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootOutputTable].InitAsDescriptorRange(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
	m_GlobalRootSig[kGlobalRootConstants].InitAsConstants(0, 32, D3D12_SHADER_VISIBILITY_ALL);

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
	desc.Exports = { L"RayGen", L"Miss", L"ClosestHit" };

	desc.HitGroupName = L"HitGroup";
	desc.ClosestHitExport = L"ClosestHit";
	desc.AnyHitExport = nullptr;
	desc.IntersectionExport = nullptr;

	desc.RayGenExport = L"RayGen";
	desc.MissExport = L"Miss";

	// 与 HLSL 的 RayPayload { float3 Color; float HitDistance; } = 16 B 一致。
	desc.MaxPayloadSizeInBytes = 16;
	desc.MaxAttributeSizeInBytes = 8;
	desc.MaxTraceRecursionDepth = 1;

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
		m_Scene.BuildShaderBindingTable(m_StateObject.GetProperties(),
			L"RayGen", L"Miss", L"HitGroup");

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
	context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		m_DescriptorHeap.GetHeapPointer());

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
		(D3D12_GPU_DESCRIPTOR_HANDLE)m_DescriptorHeap[kOutputUavSlot]);

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

	// 同上：根常量也必须走计算根参数槽位（SetComputeRoot32BitConstants）。
	compute.SetConstantArray(kGlobalRootConstants, 32, &constants);

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
