// ============================================================================
// 模块：RayTracingMainPass（DXR 主射线 DispatchRays Pass · 实现）
// 架构层：Engine/04_Renderer/Passes
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/Passes/RayTracingMainPass.h"

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "04_Renderer/AccelerationStructureManager.h"
#include "04_Renderer/FrameBlackboard.h"
#include "04_Renderer/RenderParams.h"
#include "04_Renderer/RenderServices.h"
#include "04_Renderer/Renderer/RayTracing/RayTracingRenderer.h"
#include "05_Scene/Camera/Camera.h"

// 本文件是 RayTracing_cso 的唯一包含点（避免静态库 LNK2005 ODR 冲突）
#include "07_RayTracing/RayTracing.h"

namespace Renderer
{
	namespace
	{
		enum GlobalRootBindings
		{
			kGlobalRootScene = 0,        // t0：TLAS（根级 SRV）
			kGlobalRootSceneData,        // t1：聚合几何（根级 SRV）
			kGlobalRootInstanceData,     // t2：每实例数据（根级 SRV）
			kGlobalRootOutputTable,      // u0：输出 UAV（描述符表，1 个）
			kGlobalRootConstants,        // b0：[WP-2C] 2 DWORD 动态 CBV（原 52 DWORD 根常量收回 50 DWORD）
			kGlobalRootMaterialTextures, // t3：无界 SRV 表（指向统一堆槽 0）
			kNumGlobalRootBindings
		};
		constexpr uint32_t kMaterialSamplerRegister  = 0;
	}

	bool RayTracingMainPass::Initialize(RenderServices& services)
	{
		if (m_Initialized)
			return true;

		if (!BuildRootSignature())
			return false;

		m_OutputUavAlloc = services.GetDescriptorHeap().AllocPersistentSingle();
		if (!m_OutputUavAlloc.IsValid())
		{
			LOG_ERROR(L"[DXR] RayTracingMainPass 分配 Zone A 输出 UAV 槽位失败\n");
			return false;
		}

		if (!BuildStateObject())
		{
			services.GetDescriptorHeap().FreePersistentSingle(
				m_OutputUavAlloc.HeapIndex, &services.GetReleaseQueue());
			m_OutputUavAlloc = {};
			return false;
		}

		m_DispatchedFrames = 0;
		m_Initialized = true;
		return true;
	}

	void RayTracingMainPass::Shutdown(RenderServices* services)
	{
		if (!m_Initialized)
			return;

		m_StateObject.Destroy();
		m_StateObjectReady = false;

		if (services != nullptr && m_OutputUavAlloc.IsValid())
		{
			services->GetDescriptorHeap().FreePersistentSingle(
				m_OutputUavAlloc.HeapIndex, &services->GetReleaseQueue());
			m_OutputUavAlloc = {};
		}

		m_GlobalRootSig.Reset(0, 0);
		m_Initialized = false;
	}

	void RayTracingMainPass::RefreshOutputUav(ColorBuffer* outputColor, RenderServices& services)
	{
		if (outputColor == nullptr || outputColor->GetResource() == nullptr)
			return;
		if (!services.IsInitialized() || !m_OutputUavAlloc.IsValid())
			return;

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = outputColor->GetFormat();
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		uavDesc.Texture2D.PlaneSlice = 0;

		Graphics::g_Device->CreateUnorderedAccessView(
			outputColor->GetResource(), nullptr, &uavDesc, m_OutputUavAlloc.Cpu);
	}

	bool RayTracingMainPass::BuildRootSignature()
	{
		m_GlobalRootSig.Reset(kNumGlobalRootBindings, 1);
		m_GlobalRootSig[kGlobalRootScene].InitAsBufferSRV(0, D3D12_SHADER_VISIBILITY_ALL);
		m_GlobalRootSig[kGlobalRootSceneData].InitAsBufferSRV(1, D3D12_SHADER_VISIBILITY_ALL);
		m_GlobalRootSig[kGlobalRootInstanceData].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_ALL);
		m_GlobalRootSig[kGlobalRootOutputTable].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
		// [WP-2C / 契约 5] 52 DWORD 根常量迁至 2 DWORD 动态 CBV，收回 50 DWORD 根签名空间
		m_GlobalRootSig[kGlobalRootConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
		m_GlobalRootSig[kGlobalRootMaterialTextures].InitAsDescriptorRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, UINT_MAX, D3D12_SHADER_VISIBILITY_ALL);

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
		m_GlobalRootSig.InitStaticSampler(kMaterialSamplerRegister, samplerDesc, D3D12_SHADER_VISIBILITY_ALL);

		m_GlobalRootSig.Finalize(L"RayTracingMainPass Global Root Signature", D3D12_ROOT_SIGNATURE_FLAG_NONE);
		if (m_GlobalRootSig.GetSignature() == nullptr)
		{
			LOG_ERROR(L"[DXR] RayTracingMainPass 全局根签名创建失败\n");
			return false;
		}
		return true;
	}

	bool RayTracingMainPass::BuildStateObject()
	{
		RayTracing::RaytracingStateObjectDesc desc;
		desc.DXILLibrary = RayTracing_cso;
		desc.DXILLibrarySize = sizeof(RayTracing_cso);
		desc.Exports = { L"RayGen", L"Miss", L"ShadowMiss", L"ClosestHit" };
		desc.HitGroupName = L"HitGroup";
		desc.ClosestHitExport = L"ClosestHit";
		desc.AnyHitExport = nullptr;
		desc.IntersectionExport = nullptr;
		desc.RayGenExport = L"RayGen";
		desc.MissExport = L"Miss";
		desc.MaxPayloadSizeInBytes = 32;
		desc.MaxAttributeSizeInBytes = 8;
		desc.MaxTraceRecursionDepth = 2;
		desc.GlobalRootSignature = m_GlobalRootSig.GetSignature();

		if (!m_StateObject.Finalize(desc))
		{
			LOG_ERROR(L"[DXR] RTPSO 创建失败：%s\n", m_StateObject.GetLastError().c_str());
			return false;
		}

		m_StateObjectReady = true;
		return true;
	}

	void RayTracingMainPass::Execute(GraphicsContext& context,
		const RenderFrameDesc& frame,
		AccelerationStructureManager& asManager,
		const FrameBlackboard& blackboard,
		uint32_t width,
		uint32_t height,
		RenderServices& services,
		RayTracingFrameStats& outStats)
	{
		if (!m_Initialized || !m_StateObjectReady)
			return;

		ColorBuffer* outputColor = blackboard.Core.LinearHDRColor != nullptr
			? blackboard.Core.LinearHDRColor
			: blackboard.Core.SceneColorLDR;
		if (outputColor == nullptr || outputColor->GetResource() == nullptr)
			return;

		// [TD-09c / WP-2C] 空场景时清屏为 (0,0,0,0)（alpha=0 通知 TonemapPass 直通纯黑）
		if (!asManager.IsReady() || frame.Models.empty())
		{
			context.PIXBeginEvent(L"RayTracingMainPass (Empty Clear)");
			context.TransitionResource(*outputColor, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
			context.ClearColor(*outputColor);
			context.TransitionResource(*outputColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context.PIXEndEvent();
			outStats = {};
			return;
		}

		if (frame.Camera == nullptr)
			return;

		RayTracing::RaytracingScene& scene = asManager.GetScene();
		const D3D12_DISPATCH_RAYS_DESC* dispatchDesc = scene.GetDispatchRaysDesc();
		if (dispatchDesc == nullptr || dispatchDesc->RayGenerationShaderRecord.StartAddress == 0)
		{
			LOG_INFO(L"[DXR] DispatchRays 跳过：SBT 尚未就绪\n");
			return;
		}

		context.PIXBeginEvent(L"RayTracingMainPass");

		context.TransitionResource(*outputColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		// DXR 绑定走 Compute 根参数空间；绑定 4 分区单可见堆保证帧内 SetDescriptorHeaps <= 1
		ComputeContext& compute = context.GetComputeContext();
		compute.SetRootSignature(m_GlobalRootSig);
		const auto& unifiedHeap = services.GetDescriptorHeap();
		unifiedHeap.BindToContext(context);

		// TLAS 永远处于 RAYTRACING_ACCELERATION_STRUCTURE 态，直接设根 SRV 地址（不得发常规 Transition）
		compute.GetCommandList()->SetComputeRootShaderResourceView(
			kGlobalRootScene, scene.GetTopLevelBuffer()->GetGpuVirtualAddress());
		compute.SetBufferSRV(kGlobalRootSceneData, *scene.GetSceneGeometryBuffer());
		compute.SetBufferSRV(kGlobalRootInstanceData, *scene.GetInstanceDataBuffer());
		compute.SetDescriptorTable(kGlobalRootOutputTable, m_OutputUavAlloc.Gpu);
		compute.SetDescriptorTable(kGlobalRootMaterialTextures, unifiedHeap.GetCbvSrvUavBaseGpuHandle());

		RayTracing::RaytracingConstants constants = {};
		constants.InvViewProj = frame.Camera->GetInViewProjMatrix();
		constants.CameraPosition = Math::Vector4(frame.Camera->GetPosition());
		const float fWidth  = static_cast<float>(width);
		const float fHeight = static_cast<float>(height);
		constants.OutputSize = Math::Vector4(fWidth, fHeight,
			fWidth > 0.0f ? 1.0f / fWidth : 0.0f,
			fHeight > 0.0f ? 1.0f / fHeight : 0.0f);

		const RenderParams& params = frame.Params ? *frame.Params : RenderParams{};
		constants.DisplayParams = Math::Vector4(params.Exposure,
			params.EnableTonemap ? 1.0f : 0.0f,
			params.EnableSRGBEncode ? 1.0f : 0.0f,
			0.0f);
		constants.DebugParams = Math::Vector4(Math::kZero);
		constants.LightDirection = Math::Vector4(params.SunDirection[0], params.SunDirection[1],
			params.SunDirection[2], params.EnableSunLight ? params.SunIntensity : 0.0f);
		constants.LightColor = Math::Vector4(params.SunColor[0], params.SunColor[1], params.SunColor[2], 0.0f);
		constants.RTParams = Math::Vector4(
			params.RTShadowNormalBias,
			params.RTShadowMaxDistance,
			params.RTReflectionMaxDistance,
			params.RTReflectionIntensity);
		constants.RTParams2 = Math::Vector4(
			params.EnableRTShadow ? 1.0f : 0.0f,
			params.EnableRTReflection ? 1.0f : 0.0f,
			static_cast<float>(static_cast<uint32_t>(params.RTDebugView)),
			0.0f);
		constants.RTParams3 = Math::Vector4(
			params.EnableMaterialTextures ? 1.0f : 0.0f,
			params.EnableGGX ? 1.0f : 0.0f,
			0.0f,
			0.0f);

		// [WP-2C / 契约 5] 动态 CBV 上传（LinearAllocator 自动 256B 对齐），仅占 2 DWORD 根参数
		compute.SetDynamicConstantBufferView(kGlobalRootConstants, sizeof(constants), &constants);

		const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Width  = width;
		const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Height = height;
		const_cast<D3D12_DISPATCH_RAYS_DESC*>(dispatchDesc)->Depth  = 1;

		// DispatchRays 前必须显式刷出所有资源屏障（UAV 转换等）
		context.FlushResourceBarriers();
		context.GetCommandList4()->SetPipelineState1(m_StateObject.GetStateObject());
		context.GetCommandList4()->DispatchRays(dispatchDesc);

		context.TransitionResource(*outputColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context.PIXEndEvent();

		const uint64_t primary = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
		outStats.PrimaryRays    = primary;
		outStats.ShadowRays     = params.EnableRTShadow ? primary : 0;
		outStats.ReflectionRays = params.EnableRTReflection ? primary : 0;
		outStats.TotalRays      = outStats.PrimaryRays + outStats.ShadowRays + outStats.ReflectionRays;
		outStats.TriangleCount  = scene.GetTriangleCount();
		outStats.BlasCount      = scene.GetBlasCount();
		outStats.InstanceCount  = scene.GetInstanceCount();

		if (params.EnableRTShadow != m_LastEnableShadow || params.EnableRTReflection != m_LastEnableReflection)
		{
			m_LastEnableShadow = params.EnableRTShadow;
			m_LastEnableReflection = params.EnableRTReflection;
			LOG_INFO(L"[DXR] 光追开关：阴影=%s 反射=%s（偏差=%.4f m，阴影距离=%.1f m，反射距离=%.1f m，反射强度=%.2f）\n",
				params.EnableRTShadow ? L"开" : L"关",
				params.EnableRTReflection ? L"开" : L"关",
				params.RTShadowNormalBias, params.RTShadowMaxDistance,
				params.RTReflectionMaxDistance, params.RTReflectionIntensity);
		}
		if (params.EnableMaterialTextures != m_LastEnableMaterialTextures || params.EnableGGX != m_LastEnableGGX)
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
				width, height,
				static_cast<unsigned long long>(dispatchDesc->RayGenerationShaderRecord.StartAddress));
		}
	}
}
