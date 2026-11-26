// ============================================================================
// 模块：PrimaryRayPass（DXR 1.1 Inline RayQuery 主表面几何 Pass · 实现）
// 架构层：Engine/04_Renderer/Passes（契约 4 / 统一 PrimarySurfaceDesc DXR 生产端）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/PrimaryRayPass.h"

#include "00_Core/Math/VectorMath.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCaps.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Memory/DeferredReleaseQueue.h"
#include "02_RHI/Pipeline/ShaderHotReloader.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "04_Renderer/AccelerationStructureManager.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 PrimaryRayCS_cso 的唯一包含点（避免静态库 LNK2005 ODR 冲突）
#include "07_RayTracing/PrimaryRayCS.h"

#include <cstdio>

namespace Renderer
{
	namespace
	{
		// 与 PrimaryRayCS.hlsl 的 cbuffer PrimaryRayFrameCB (b1) 逐字段对齐（160 字节）
		struct alignas(16) PrimaryRayFrameConstants
		{
			Math::Matrix4 InvViewProj; // offset   0 (64 B)
			Math::Matrix4 ViewProj;    // offset  64 (64 B)
			Math::Vector4 CameraPos;   // offset 128 (16 B)
			Math::Vector4 OutputSize;  // offset 144 (16 B): xy=W,H, zw=1/W,1/H
		};
		static_assert(sizeof(PrimaryRayFrameConstants) == 160,
			"PrimaryRayFrameConstants must be 160 bytes to match PrimaryRayCS.hlsl");
		static_assert(sizeof(PrimaryRayFrameConstants) % 16 == 0,
			"PrimaryRayFrameConstants must be 16-byte aligned");
	}

	bool PrimaryRayPass::Initialize(RenderServices& services)
	{
		if (m_Initialized)
			return true;

		const Graphics::GpuCaps& caps = Graphics::GetGpuCaps();
		if (!services.IsBindlessSM66Ready() ||
			caps.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
		{
			LOG_INFO(L"[PrimaryRayPass] 跳过初始化：需要 SM 6.6 Bindless (%d) 与 DXR Tier 1.1 (tier=%u)\n",
				services.IsBindlessSM66Ready() ? 1 : 0,
				static_cast<unsigned>(caps.RaytracingTier));
			return false;
		}

		m_ComputePSO = ComputePSO(L"PrimaryRayPass : Inline RayQuery SM6.6 Bindless PSO");
		m_ComputePSO.SetRootSignature(services.GetBindlessComputeRootSignature());
		m_ComputePSO.SetComputeShader(PrimaryRayCS_cso, sizeof(PrimaryRayCS_cso));
		m_ComputePSO.Finalize();

		m_Initialized = (m_ComputePSO.GetPipelineStateObject() != nullptr);
		LOG_INFO(L"[PrimaryRayPass] initialized=%d (SM6.6 Bindless + DXR 1.1 Inline RayQuery)\n",
			m_Initialized ? 1 : 0);
		return m_Initialized;
	}

	void PrimaryRayPass::Shutdown()
	{
		m_HotComputePSO.Reset();
		m_Initialized = false;
	}

	void PrimaryRayPass::RegisterHotReload(Graphics::ShaderHotReloader& reloader, RenderServices& services)
	{
		if (!m_Initialized || !services.IsBindlessSM66Ready())
			return;

		Graphics::ShaderHotReloadDesc desc;
		desc.Id            = "PrimaryRayCS";
		desc.SourceRelPath = L"Engine/Shader/07_RayTracing/PrimaryRayCS.hlsl";
		desc.EntryPoint    = L"MainCS";
		desc.TargetProfile = L"cs_6_6";
		desc.RebuildPipeline = [this, &services](const std::vector<uint8_t>& dxil, std::string& outErr) {
			return HotReloadComputeShader(dxil, services, outErr);
		};
		reloader.Register(std::move(desc));
	}

	bool PrimaryRayPass::HotReloadComputeShader(const std::vector<uint8_t>& dxil,
		RenderServices& services, std::string& outErr)
	{
		if (!m_Initialized || Graphics::g_Device == nullptr || dxil.empty())
		{
			outErr = "PrimaryRayPass not initialized or empty DXIL";
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = services.GetBindlessComputeRootSignature().GetSignature();
		desc.CS             = { dxil.data(), dxil.size() };

		Microsoft::WRL::ComPtr<ID3D12PipelineState> candidate;
		const HRESULT hr = Graphics::g_Device->CreateComputePipelineState(&desc, TY_IID_PPV_ARGS(&candidate));
		if (FAILED(hr) || candidate == nullptr)
		{
			char buf[128] = {};
			std::snprintf(buf, sizeof(buf), "PrimaryRayPass CreateComputePipelineState failed (hr=0x%08X)", (unsigned)hr);
			outErr = buf;
			return false;
		}
		candidate->SetName(L"PrimaryRayPass HotReload Compute PSO");

		// [契约 7.2 事务化安全铁律] 旧 PSO 可能仍被在途命令列表引用，必须压入帧尾延迟释放队列。
		if (m_HotComputePSO != nullptr)
		{
			Microsoft::WRL::ComPtr<IUnknown> oldUnk;
			m_HotComputePSO.As(&oldUnk);
			services.GetReleaseQueue().Enqueue(std::move(oldUnk));
		}
		m_HotComputePSO = std::move(candidate);
		return true;
	}

	void PrimaryRayPass::Execute(GraphicsContext& context,
		const RenderFrameDesc& frame,
		const AccelerationStructureManager& asManager,
		const PrimaryRayTargets& targets,
		uint32_t width,
		uint32_t height,
		RenderServices& services) const
	{
		if (!targets.IsValid() || width == 0 || height == 0)
			return;

		// 空场景或硬件不支持 Inline RayQuery 时安全清屏到统一未命中哨兵值（Depth=1.0），
		// 保证下游读取 PrimarySurface 时绝不读到未初始化垃圾显存。
		const bool canTrace = m_Initialized &&
			asManager.IsReady() &&
			!frame.Models.empty() &&
			frame.Camera != nullptr &&
			asManager.GetScene().GetTopLevelBuffer() != nullptr &&
			asManager.GetScene().GetSceneGeometryBuffer() != nullptr &&
			asManager.GetScene().GetInstanceDataBuffer() != nullptr;

		if (!canTrace)
		{
			context.PIXBeginEvent(L"PrimaryRayPass (Clear)");
			context.TransitionResource(*targets.DepthR32F, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.TransitionResource(*targets.AlbedoAO,  D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.TransitionResource(*targets.NormalOct, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.TransitionResource(*targets.Material,  D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.TransitionResource(*targets.Emission,  D3D12_RESOURCE_STATE_RENDER_TARGET);
			context.FlushResourceBarriers();
			context.ClearColor(*targets.DepthR32F);
			context.ClearColor(*targets.AlbedoAO);
			context.ClearColor(*targets.NormalOct);
			context.ClearColor(*targets.Material);
			context.ClearColor(*targets.Emission);
			context.PIXEndEvent();
			return;
		}

		const RayTracing::RaytracingScene& scene = asManager.GetScene();
		const GpuBuffer* tlasBuf     = scene.GetTopLevelBuffer();
		const GpuBuffer* sceneGeom   = scene.GetSceneGeometryBuffer();
		const GpuBuffer* instDataBuf = scene.GetInstanceDataBuffer();

		UnifiedDescriptorHeap& heap = services.GetDescriptorHeap();

		// 1. 在 4 分区单可见堆 Zone B 瞬态创建 3 个读取 SRV（TLAS + 聚合几何 Raw + 实例数据 Raw），
		//    零持久槽占用、零 CPU 描述符泄漏，且整帧保持 SetDescriptorHeaps <= 1。
		const BindlessAllocation inputSrvs = heap.AllocTransient(3);
		if (!inputSrvs.IsValid())
			return;

		{
			// [DX12 铁律] 加速结构 SRV 的 pResource 必须传 nullptr，GPU 地址填在 RaytracingAccelerationStructure.Location。
			D3D12_SHADER_RESOURCE_VIEW_DESC tlasDesc = {};
			tlasDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			tlasDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			tlasDesc.RaytracingAccelerationStructure.Location = tlasBuf->GetGpuVirtualAddress();
			Graphics::g_Device->CreateShaderResourceView(
				nullptr, &tlasDesc, heap.GetCbvSrvUavHandle(inputSrvs.HeapIndex + 0u).Cpu);

			D3D12_SHADER_RESOURCE_VIEW_DESC geomDesc = {};
			geomDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			geomDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			geomDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			geomDesc.Buffer.FirstElement = 0;
			geomDesc.Buffer.NumElements = static_cast<UINT>(sceneGeom->GetBufferSize() / 4u);
			geomDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			Graphics::g_Device->CreateShaderResourceView(
				const_cast<ID3D12Resource*>(sceneGeom->GetResource()), &geomDesc,
				heap.GetCbvSrvUavHandle(inputSrvs.HeapIndex + 1u).Cpu);

			D3D12_SHADER_RESOURCE_VIEW_DESC instDesc = {};
			instDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			instDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			instDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			instDesc.Buffer.FirstElement = 0;
			instDesc.Buffer.NumElements = static_cast<UINT>(instDataBuf->GetBufferSize() / 4u);
			instDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			Graphics::g_Device->CreateShaderResourceView(
				const_cast<ID3D12Resource*>(instDataBuf->GetResource()), &instDesc,
				heap.GetCbvSrvUavHandle(inputSrvs.HeapIndex + 2u).Cpu);
		}

		// 2. 在 Zone B 瞬态暂存 5 张输出表面目标的 UAV 描述符
		const D3D12_CPU_DESCRIPTOR_HANDLE uavHandles[5] = {
			targets.DepthR32F->GetUAV(),
			targets.AlbedoAO->GetUAV(),
			targets.NormalOct->GetUAV(),
			targets.Material->GetUAV(),
			targets.Emission->GetUAV(),
		};
		const BindlessAllocation outUavs = heap.StageTransientDescriptors(uavHandles, 5);
		if (!outUavs.IsValid())
			return;

		context.PIXBeginEvent(L"PrimaryRayPass");

		ComputeContext& compute = context.GetComputeContext();
		heap.BindToContext(context);
		compute.SetRootSignature(services.GetBindlessComputeRootSignature());
		compute.SetPipelineStateRaw(m_HotComputePSO ? m_HotComputePSO.Get() : m_ComputePSO.GetPipelineStateObject());

		const RenderParams activeParams = (frame.Params != nullptr) ? *frame.Params : RenderParams{};

		const uint32_t rootConsts[12] = {
			inputSrvs.HeapIndex + 0u,
			inputSrvs.HeapIndex + 1u,
			inputSrvs.HeapIndex + 2u,
			outUavs.HeapIndex + 0u,
			outUavs.HeapIndex + 1u,
			outUavs.HeapIndex + 2u,
			outUavs.HeapIndex + 3u,
			outUavs.HeapIndex + 4u,
			activeParams.EnableMaterialTextures ? 1u : 0u,
			width,
			height,
			0u,
		};
		compute.SetConstantArray(0, 12, rootConsts);

		PrimaryRayFrameConstants frameCB = {};
		frameCB.InvViewProj = frame.Camera->GetInViewProjMatrix();
		frameCB.ViewProj    = frame.Camera->GetViewProjMatrix();
		frameCB.CameraPos   = Math::Vector4(frame.Camera->GetPosition());
		const float fW = static_cast<float>(width);
		const float fH = static_cast<float>(height);
		frameCB.OutputSize  = Math::Vector4(fW, fH, 1.0f / fW, 1.0f / fH);
		compute.SetDynamicConstantBufferView(1, sizeof(frameCB), &frameCB);

		// Dispatch2D 自动向上取整为 (width+7)/8 x (height+7)/8，并在提交前刷出全部挂起屏障
		compute.Dispatch2D(width, height, 8, 8);

		context.PIXEndEvent();
	}
}
