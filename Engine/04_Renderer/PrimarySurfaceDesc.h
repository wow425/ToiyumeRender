// ============================================================================
// 模块：PrimarySurfaceDesc（光栅 / DXR 统一主可见性与表面几何契约）
// 架构层：Engine/04_Renderer（契约 4 / 统一表面基座；零 DXR 头文件与对象依赖）
// 核心职责：
//   - 定义与上游来源（Deferred GBufferPass vs DXR PrimaryRayPass）完全解耦的
//     统一主表面数据结构 PrimarySurfaceDesc 与 32 字节 GPU 根常量打包结构
//     PrimarySurfaceBindlessIndices；
//   - 统一约定 4 个必选核心通道（Depth / AlbedoAO / Normal / Material）与
//     2 个可选扩展通道（Emission / Velocity）的格式、语义与 Bindless 下标；
//   - 为未来下游 Pass（阴影 / SSAO / 去噪 / Phase 4 间接光等）提供同构的
//     LinearPassList 屏障声明辅助与 Zone B Bindless SRV 一次性暂存入口。
// 线程与同步前提：
//   - 仅单线程帧录制期写入与读取；
//   - PrimarySurfaceDesc 本身不持有任何 GPU 资源所有权（非拥有裸指针），资源生命周期
//     由各管线（DeferredRenderer / RayTracingRenderer）托管；
//   - 即使使用 SM 6.6 Bindless 直索引，下游 Pass 仍必须调用 AppendPrimarySurfaceReadAccesses
//     向 LinearPassList 完整声明 ResourceAccess 状态转换，绝不可省略屏障！
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>
#include <d3d12.h>

#include "04_Renderer/LinearPassList.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

class ColorBuffer;
class DepthBuffer;
class GpuResource;

namespace Renderer
{
	// 主表面来源标识：下游消费者可据此区分光栅深度（DepthBuffer D32）与 DXR 浮点深度（ColorBuffer R32F），
	// 但两者 SRV 均为 Texture2D<float> (R32_FLOAT) 且值域完全一致（NDC z ∈ [0,1]，1.0=未命中/天空）。
	enum class PrimarySurfaceSource : uint8_t
	{
		None          = 0,
		RasterGBuffer = 1, // DeferredRenderer::GBufferPass 零拷贝复用产出
		DxrPrimaryRay = 2, // RayTracingRenderer::PrimaryRayPass (DXR 1.1 Inline RayQuery) 产出
	};

	inline const char* PrimarySurfaceSourceName(PrimarySurfaceSource source)
	{
		switch (source)
		{
		case PrimarySurfaceSource::RasterGBuffer: return "RasterGBuffer";
		case PrimarySurfaceSource::DxrPrimaryRay: return "DxrPrimaryRay";
		default:                                  return "None";
		}
	}

	// 单通道表面描述符槽位（非拥有视图：同时承载屏障目标指针、CPU 源 SRV 与统一可见堆 Bindless 下标）
	struct PrimarySurfaceSlot
	{
		GpuResource*                Resource       = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv         = { 0 };
		uint32_t                    SrvBindlessIdx = kInvalidBindlessIndex;
		uint32_t                    UavBindlessIdx = kInvalidBindlessIndex;
		DXGI_FORMAT                 Format         = DXGI_FORMAT_UNKNOWN;

		bool IsValid() const { return Resource != nullptr; }
		bool HasCpuSrv() const { return Resource != nullptr && CpuSrv.ptr != 0 && CpuSrv.ptr != D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN; }
		bool HasBindlessSrv() const { return Resource != nullptr && SrvBindlessIdx != kInvalidBindlessIndex; }
	};

	// 与 Engine/Shader/00_Common/PrimarySurfaceCommon.hlsli 逐字段严格对齐的 GPU 索引包（32 字节 = 8 DWORD）。
	// 可直接通过 BindlessRootSignature 的 b0 根常量（占 8/16 DWORD）或 b2 Per-Pass CBV 传给任意 SM 6.6 着色器。
	struct PrimarySurfaceBindlessIndices
	{
		uint32_t DepthSrvIdx    = kInvalidBindlessIndex; // offset  0: Texture2D<float>  (NDC z ∈ [0,1], 1.0=sky/miss)
		uint32_t AlbedoAoSrvIdx = kInvalidBindlessIndex; // offset  4: Texture2D<float4> (RGB=Linear BaseColor, A=AO)
		uint32_t NormalSrvIdx   = kInvalidBindlessIndex; // offset  8: Texture2D<float2> (RG=Octahedral WorldNormal [0,1])
		uint32_t MaterialSrvIdx = kInvalidBindlessIndex; // offset 12: Texture2D<float4> (R=Metallic, G=Roughness, B=Specular, A=MatID)
		uint32_t EmissionSrvIdx = kInvalidBindlessIndex; // offset 16: Texture2D<float4> (RGB=Emissive, 可选)
		uint32_t VelocitySrvIdx = kInvalidBindlessIndex; // offset 20: Texture2D<float2> (屏幕运动矢量, 可选)
		uint32_t Width          = 0;                     // offset 24: 表面宽度（像素）
		uint32_t Height         = 0;                     // offset 28: 表面高度（像素）
	};
	static_assert(sizeof(PrimarySurfaceBindlessIndices) == 32,
		"PrimarySurfaceBindlessIndices must be 32 bytes (8 DWORDs) to match PrimarySurfaceCommon.hlsli");
	static_assert(sizeof(PrimarySurfaceBindlessIndices) % 16 == 0,
		"PrimarySurfaceBindlessIndices must be 16-byte aligned for HLSL cbuffer packing");

	// ========================================================================
	// 统一主可见性与表面几何契约（契约 4）
	// ------------------------------------------------------------------------
	// 所有权：每帧由活跃管线在 FrameBlackboard 中填充（不拥有 GPU 资源），帧末随
	// FrameBlackboard::Reset() 清空。
	// 通道编码统一约定（Deferred 与 DXR 必须 100% 遵守同一编码，保证下游 Shader 零分支复用）：
	//   - Depth    : R32_FLOAT SRV（光栅来自 D32_FLOAT，DXR 来自 R32_FLOAT UAV），NDC z∈[0,1]，1.0=未命中/天空
	//   - AlbedoAO : R8G8B8A8_UNORM，RGB=线性 BaseColor，A=AO
	//   - Normal   : R16G16_FLOAT，RG=EncodeOctNormal(WorldNormal) ∈ [0,1]
	//   - Material : R16G16B16A16_FLOAT，R=Metallic，G=Roughness，B=Specular(0.5)，A=MaterialID
	//   - Emission : R8G8B8A8_UNORM（可选），RGB=线性自发光
	//   - Velocity : R16G16_FLOAT（可选），屏幕空间运动矢量
	// ========================================================================
	struct PrimarySurfaceDesc
	{
		PrimarySurfaceSource Source = PrimarySurfaceSource::None;
		uint32_t             Width  = 0;
		uint32_t             Height = 0;

		// 4 个必选核心表面通道
		PrimarySurfaceSlot Depth;
		PrimarySurfaceSlot AlbedoAO;
		PrimarySurfaceSlot Normal;
		PrimarySurfaceSlot Material;

		// 2 个可选扩展通道
		PrimarySurfaceSlot Emission;
		PrimarySurfaceSlot Velocity;

		void Reset()
		{
			*this = PrimarySurfaceDesc{};
		}

		bool IsComplete() const
		{
			return Source != PrimarySurfaceSource::None &&
				Width > 0 && Height > 0 &&
				Depth.IsValid() &&
				AlbedoAO.IsValid() &&
				Normal.IsValid() &&
				Material.IsValid();
		}

		bool IsBindlessReady() const
		{
			return IsComplete() &&
				Depth.HasBindlessSrv() &&
				AlbedoAO.HasBindlessSrv() &&
				Normal.HasBindlessSrv() &&
				Material.HasBindlessSrv();
		}

		bool HasEmission() const { return Emission.IsValid(); }
		bool HasVelocity() const { return Velocity.IsValid(); }

		// 打包为 32 字节 GPU Bindless 索引结构；无效可选通道保持 kInvalidBindlessIndex。
		PrimarySurfaceBindlessIndices PackBindlessIndices() const
		{
			PrimarySurfaceBindlessIndices packed;
			packed.DepthSrvIdx    = Depth.HasBindlessSrv()    ? Depth.SrvBindlessIdx    : kInvalidBindlessIndex;
			packed.AlbedoAoSrvIdx = AlbedoAO.HasBindlessSrv() ? AlbedoAO.SrvBindlessIdx : kInvalidBindlessIndex;
			packed.NormalSrvIdx   = Normal.HasBindlessSrv()   ? Normal.SrvBindlessIdx   : kInvalidBindlessIndex;
			packed.MaterialSrvIdx = Material.HasBindlessSrv() ? Material.SrvBindlessIdx : kInvalidBindlessIndex;
			packed.EmissionSrvIdx = Emission.HasBindlessSrv() ? Emission.SrvBindlessIdx : kInvalidBindlessIndex;
			packed.VelocitySrvIdx = Velocity.HasBindlessSrv() ? Velocity.SrvBindlessIdx : kInvalidBindlessIndex;
			packed.Width          = Width;
			packed.Height         = Height;
			return packed;
		}
	};

	// ---- 纯 CPU / 管线装配辅助函数 ----

	// 从 DeferredRenderer 既有 GBuffer + SceneDepth 零拷贝组装 PrimarySurfaceDesc（零 DXR 依赖）。
	PrimarySurfaceDesc BuildPrimarySurfaceFromDeferred(
		uint32_t width,
		uint32_t height,
		DepthBuffer* sceneDepth,
		ColorBuffer* gbufBaseColor,
		ColorBuffer* gbufNormal,
		ColorBuffer* gbufMaterial,
		ColorBuffer* gbufEmission = nullptr,
		ColorBuffer* velocity = nullptr);

	// 从 DXR PrimaryRayPass 产出的浮点深度 + 表面缓冲组装 PrimarySurfaceDesc。
	PrimarySurfaceDesc BuildPrimarySurfaceFromDxr(
		uint32_t width,
		uint32_t height,
		ColorBuffer* depthR32F,
		ColorBuffer* albedoAO,
		ColorBuffer* normalOct,
		ColorBuffer* material,
		ColorBuffer* emission = nullptr,
		ColorBuffer* velocity = nullptr);

	// 将主表面的全部有效 CPU SRV 一次性连续暂存到 4 分区单可见堆 Zone B，并回填各槽位 SrvBindlessIdx。
	// 返回 true 表示 4 个必选核心通道均成功取得有效 Bindless 下标。
	bool StagePrimarySurfaceSRVs(PrimarySurfaceDesc& surface, UnifiedDescriptorHeap& unifiedHeap);

	// 将读取 PrimarySurface 所需的全部 ResourceAccess 追加到 LinearPassList 依赖列表：
	//   - 若 depthReadMode 为 ReadDepthAndSRV/ReadDepthOnly 且来源为 DxrPrimaryRay（ColorBuffer，非 DSV 资源），
	//     自动降级为 colorReadMode，避免对非深度格式发 DEPTH_READ 触发 D3D12 Debug Layer 报错！
	//   - 设为 inline 纯 CPU 逻辑，使独立 TooiyumeCpuTests 无需链接 D3D12 设备即可完整单测该安全不变量。
	inline void AppendPrimarySurfaceReadAccesses(
		const PrimarySurfaceDesc& surface,
		PassAccess colorReadMode,
		PassAccess depthReadMode,
		bool includeEmission,
		bool includeVelocity,
		std::vector<ResourceAccess>& outAccesses)
	{
		if (!surface.IsComplete())
			return;

		PassAccess safeDepthMode = depthReadMode;
		if (surface.Source == PrimarySurfaceSource::DxrPrimaryRay &&
			(depthReadMode == PassAccess::ReadDepthOnly || depthReadMode == PassAccess::ReadDepthAndSRV))
		{
			safeDepthMode = colorReadMode;
		}

		outAccesses.push_back({ surface.Depth.Resource,    safeDepthMode });
		outAccesses.push_back({ surface.AlbedoAO.Resource, colorReadMode });
		outAccesses.push_back({ surface.Normal.Resource,   colorReadMode });
		outAccesses.push_back({ surface.Material.Resource, colorReadMode });

		if (includeEmission && surface.Emission.IsValid())
			outAccesses.push_back({ surface.Emission.Resource, colorReadMode });
		if (includeVelocity && surface.Velocity.IsValid())
			outAccesses.push_back({ surface.Velocity.Resource, colorReadMode });
	}
}
