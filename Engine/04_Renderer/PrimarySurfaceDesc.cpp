// ============================================================================
// 模块：PrimarySurfaceDesc（统一主表面契约组装、Zone B Bindless 暂存与屏障声明 · 实现）
// 架构层：Engine/04_Renderer（零 DXR 头文件依赖；纯 CPU 屏障翻译逻辑可独立单测）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/PrimarySurfaceDesc.h"

#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"

namespace Renderer
{
	namespace
	{
		PrimarySurfaceSlot MakeColorSlot(ColorBuffer* buf)
		{
			PrimarySurfaceSlot slot;
			if (buf != nullptr)
			{
				slot.Resource = buf;
				slot.CpuSrv   = buf->GetSRV();
				slot.Format   = buf->GetFormat();
			}
			return slot;
		}

		PrimarySurfaceSlot MakeDepthSlot(DepthBuffer* buf)
		{
			PrimarySurfaceSlot slot;
			if (buf != nullptr)
			{
				slot.Resource = buf;
				slot.CpuSrv   = buf->GetDepthSRV();
				// DepthBuffer 在 SRV 视图下以 R32_FLOAT 读取（D32_FLOAT -> R32_FLOAT）
				slot.Format   = buf->GetFormat();
			}
			return slot;
		}
	}

	PrimarySurfaceDesc BuildPrimarySurfaceFromDeferred(
		uint32_t width,
		uint32_t height,
		DepthBuffer* sceneDepth,
		ColorBuffer* gbufBaseColor,
		ColorBuffer* gbufNormal,
		ColorBuffer* gbufMaterial,
		ColorBuffer* gbufEmission,
		ColorBuffer* velocity)
	{
		PrimarySurfaceDesc desc;
		desc.Source   = PrimarySurfaceSource::RasterGBuffer;
		desc.Width    = width;
		desc.Height   = height;
		desc.Depth    = MakeDepthSlot(sceneDepth);
		desc.AlbedoAO = MakeColorSlot(gbufBaseColor);
		desc.Normal   = MakeColorSlot(gbufNormal);
		desc.Material = MakeColorSlot(gbufMaterial);
		desc.Emission = MakeColorSlot(gbufEmission);
		desc.Velocity = MakeColorSlot(velocity);
		return desc;
	}

	PrimarySurfaceDesc BuildPrimarySurfaceFromDxr(
		uint32_t width,
		uint32_t height,
		ColorBuffer* depthR32F,
		ColorBuffer* albedoAO,
		ColorBuffer* normalOct,
		ColorBuffer* material,
		ColorBuffer* emission,
		ColorBuffer* velocity)
	{
		PrimarySurfaceDesc desc;
		desc.Source   = PrimarySurfaceSource::DxrPrimaryRay;
		desc.Width    = width;
		desc.Height   = height;
		desc.Depth    = MakeColorSlot(depthR32F);
		desc.AlbedoAO = MakeColorSlot(albedoAO);
		desc.Normal   = MakeColorSlot(normalOct);
		desc.Material = MakeColorSlot(material);
		desc.Emission = MakeColorSlot(emission);
		desc.Velocity = MakeColorSlot(velocity);
		return desc;
	}

	bool StagePrimarySurfaceSRVs(PrimarySurfaceDesc& surface, UnifiedDescriptorHeap& unifiedHeap)
	{
		if (!surface.IsComplete() || !unifiedHeap.IsInitialized())
			return false;

		if (!surface.Depth.HasCpuSrv() ||
			!surface.AlbedoAO.HasCpuSrv() ||
			!surface.Normal.HasCpuSrv() ||
			!surface.Material.HasCpuSrv())
		{
			return false;
		}

		// 一次性将 4 个必选通道 + 最多 2 个可选通道连续拷贝到 Zone B 环形区（最多消耗 6/5461 槽），
		// 整帧零切堆，直接产出供 SM 6.6 ResourceDescriptorHeap[] 索引的稳定 Bindless 下标。
		D3D12_CPU_DESCRIPTOR_HANDLE handles[6] = {};
		PrimarySurfaceSlot* slots[6] = {};
		uint32_t count = 0;

		auto PushSlot = [&](PrimarySurfaceSlot& s)
		{
			handles[count] = s.CpuSrv;
			slots[count]   = &s;
			++count;
		};

		PushSlot(surface.Depth);
		PushSlot(surface.AlbedoAO);
		PushSlot(surface.Normal);
		PushSlot(surface.Material);

		if (surface.Emission.HasCpuSrv())
			PushSlot(surface.Emission);
		if (surface.Velocity.HasCpuSrv())
			PushSlot(surface.Velocity);

		const BindlessAllocation block = unifiedHeap.StageTransientDescriptors(handles, count);
		if (!block.IsValid())
			return false;

		for (uint32_t i = 0; i < count; ++i)
		{
			slots[i]->SrvBindlessIdx = block.HeapIndex + i;
		}

		return surface.IsBindlessReady();
	}
}
