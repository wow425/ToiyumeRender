// ============================================================================
// 模块：LinearPassList（三阶段轻量线性 Pass 调度器 · 实现）
// 架构层：Engine/04_Renderer（契约 3 / WP-3A）
// ============================================================================
#include "00_Core/PCH.h"
#include "04_Renderer/LinearPassList.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "02_RHI/Resource/DepthBuffer.h"
#include "02_RHI/Resource/GpuResource.h"
#include "04_Renderer/GpuTimer.h"
#include "04_Renderer/RenderServices.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Renderer
{
	D3D12_RESOURCE_STATES TranslatePassAccess(PassAccess access)
	{
		switch (access)
		{
		case PassAccess::ReadSRV_Pixel:         return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case PassAccess::ReadSRV_NonPixel:      return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case PassAccess::ReadSRV_All:           return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case PassAccess::ReadDepthOnly:         return D3D12_RESOURCE_STATE_DEPTH_READ;
		case PassAccess::ReadDepthAndSRV:       return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case PassAccess::WriteRTV:              return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case PassAccess::WriteDepth:            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		case PassAccess::WriteUAV:              return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case PassAccess::ReadWriteUAV:          return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case PassAccess::CopySrc:               return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case PassAccess::CopyDst:               return D3D12_RESOURCE_STATE_COPY_DEST;
		case PassAccess::AccelerationStructure: return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		default:                                return D3D12_RESOURCE_STATE_COMMON;
		}
	}

	bool IsReadAccess(PassAccess access)
	{
		switch (access)
		{
		case PassAccess::ReadSRV_Pixel:
		case PassAccess::ReadSRV_NonPixel:
		case PassAccess::ReadSRV_All:
		case PassAccess::ReadDepthOnly:
		case PassAccess::ReadDepthAndSRV:
		case PassAccess::ReadWriteUAV:
		case PassAccess::CopySrc:
		case PassAccess::AccelerationStructure:
			return true;
		default:
			return false;
		}
	}

	bool IsWriteAccess(PassAccess access)
	{
		switch (access)
		{
		case PassAccess::WriteRTV:
		case PassAccess::WriteDepth:
		case PassAccess::WriteUAV:
		case PassAccess::ReadWriteUAV:
		case PassAccess::CopyDst:
			return true;
		default:
			return false;
		}
	}

	void BindReadOnlyDepth(GraphicsContext& ctx, ColorBuffer* rtv, DepthBuffer& depth)
	{
		// [契约 3 / TD-04c 防护] 强制走 GetDSV_DepthReadOnly()，确保与 DEPTH_READ 状态严格一致
		if (rtv != nullptr)
			ctx.SetRenderTarget(rtv->GetRTV(), depth.GetDSV_DepthReadOnly());
		else
			ctx.SetDepthStencilTarget(depth.GetDSV_DepthReadOnly());
	}

	bool TransientRTDesc::operator==(const TransientRTDesc& rhs) const
	{
		return Width == rhs.Width &&
			Height == rhs.Height &&
			MipCount == rhs.MipCount &&
			Format == rhs.Format &&
			ClearRGBA == rhs.ClearRGBA;
	}

	TransientRTMap::Entry::Entry() = default;
	TransientRTMap::Entry::~Entry() = default;
	TransientRTMap::Entry::Entry(Entry&&) noexcept = default;
	TransientRTMap::Entry& TransientRTMap::Entry::operator=(Entry&&) noexcept = default;

	TransientRTMap::TransientRTMap() = default;
	TransientRTMap::~TransientRTMap() { Clear(); }

	void TransientRTMap::BeginFrame(uint64_t frameIndex)
	{
		m_CurrentFrame = frameIndex;
		for (auto& e : m_Entries)
			e.InUseThisFrame = false;

		// 淘汰超过 60 帧未使用的瞬态 RT（调用方需保证处于安全帧边界）
		m_Entries.erase(
			std::remove_if(m_Entries.begin(), m_Entries.end(),
				[frameIndex](const Entry& e) {
					return frameIndex > e.LastUsedFrame && (frameIndex - e.LastUsedFrame) > kEvictFrames;
				}),
			m_Entries.end());
	}

	ColorBuffer* TransientRTMap::Acquire(const std::wstring& debugName, const TransientRTDesc& desc)
	{
		if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN)
			return nullptr;

		for (auto& e : m_Entries)
		{
			if (!e.InUseThisFrame && e.Desc == desc && e.Buffer != nullptr)
			{
				e.InUseThisFrame = true;
				e.LastUsedFrame  = m_CurrentFrame;
				return e.Buffer.get();
			}
		}

		// 未命中：新建带精确 ClearRGBA 的 ColorBuffer（消除 Debug Layer #820 ClearValue 不匹配）
		auto buf = std::make_unique<ColorBuffer>(
			Color(desc.ClearRGBA[0], desc.ClearRGBA[1], desc.ClearRGBA[2], desc.ClearRGBA[3]));
		buf->Create(debugName, desc.Width, desc.Height, desc.MipCount, desc.Format);
		ColorBuffer* raw = buf.get();
		Entry entry;
		entry.Desc           = desc;
		entry.Buffer         = std::move(buf);
		entry.LastUsedFrame  = m_CurrentFrame;
		entry.InUseThisFrame = true;
		m_Entries.push_back(std::move(entry));
		return raw;
	}

	void TransientRTMap::Clear()
	{
		m_Entries.clear();
	}

	// =========================================================================
	// LinearPassList：严格三阶段（Declare -> Validate/LifetimeScan -> Execute）
	// =========================================================================

	void LinearPassList::Reset()
	{
		m_Passes.clear();
		m_TransientResources.clear();
		m_Lifetimes.clear();
		m_LastValidationError.clear();
		m_Validated = false;
	}

	void LinearPassList::MarkTransient(GpuResource* res)
	{
		if (res == nullptr)
			return;
		if (!IsMarkedTransient(res))
			m_TransientResources.push_back(res);
	}

	bool LinearPassList::IsMarkedTransient(GpuResource* res) const
	{
		return std::find(m_TransientResources.begin(), m_TransientResources.end(), res) != m_TransientResources.end();
	}

	void LinearPassList::AddPass(std::string name, std::vector<ResourceAccess> accesses, PassCallback fn)
	{
		m_Passes.push_back(PassNode{ std::move(name), std::move(accesses), std::move(fn), false });
		m_Validated = false;
	}

	void LinearPassList::AddNativePass(std::string name, std::vector<ResourceAccess> accesses, PassCallback fn)
	{
		m_Passes.push_back(PassNode{ std::move(name), std::move(accesses), std::move(fn), true });
		m_Validated = false;
	}

	bool LinearPassList::ValidateAndScanLifetimes(std::string* outError)
	{
		m_Lifetimes.clear();
		m_LastValidationError.clear();

		// O(N) 线性扫描：记录每个 GpuResource* 首次与末次被访问的 Pass 下标，同时检查悬空读
		std::unordered_map<GpuResource*, size_t> indexMap;
		indexMap.reserve(32);

		bool ok = true;
		for (uint32_t passIdx = 0; passIdx < static_cast<uint32_t>(m_Passes.size()); ++passIdx)
		{
			const PassNode& pass = m_Passes[passIdx];
			for (const ResourceAccess& acc : pass.Accesses)
			{
				if (acc.Res == nullptr)
					continue;

				const bool isTransient = (!acc.IsExternal) || IsMarkedTransient(acc.Res);
				auto it = indexMap.find(acc.Res);
				if (it == indexMap.end())
				{
					const bool firstIsPureRead = IsReadAccess(acc.Access) && !IsWriteAccess(acc.Access);
					PassResourceLifetime lt;
					lt.Resource     = acc.Res;
					lt.FirstPassIdx = passIdx;
					lt.LastPassIdx  = passIdx;
					lt.FirstIsRead  = firstIsPureRead;
					lt.IsExternal   = !isTransient;
					indexMap.emplace(acc.Res, m_Lifetimes.size());
					m_Lifetimes.push_back(lt);

					// [契约 3 悬空读阻断] 瞬态资源在任何 Pass 写入前被纯读，必定读到未定义内容！
					if (isTransient && firstIsPureRead)
					{
						ok = false;
						m_LastValidationError =
							"Dangling read of uninitialized transient resource in pass '" + pass.Name + "'";
						LOG_ERROR(L"[LinearPassList] %hs\n", m_LastValidationError.c_str());
					}
				}
				else
				{
					m_Lifetimes[it->second].LastPassIdx = passIdx;
				}
			}
		}

		if (!ok && outError != nullptr)
			*outError = m_LastValidationError;

		m_Validated = true;
		return ok;
	}

	void LinearPassList::Execute(GraphicsContext& ctx, RenderServices* services, GpuTimer* timer)
	{
		if (!m_Validated)
		{
			std::string err;
			const bool valid = ValidateAndScanLifetimes(&err);
			ASSERT(valid, "LinearPassList validation failed (dangling transient read)");
			(void)valid;
		}

		// 追踪最近一次以写/UAV 方式接触的资源，支持连续 UAV->UAV 自动插 InsertUAVBarrier
		std::unordered_map<GpuResource*, PassAccess> prevAccessMap;
		prevAccessMap.reserve(32);

		for (const PassNode& pass : m_Passes)
		{
			std::wstring wideName(pass.Name.begin(), pass.Name.end());
			ctx.PIXBeginEvent(wideName.c_str());
			if (timer != nullptr)
				timer->BeginPass(ctx, pass.Name.c_str());

			// 1. 翻译所有声明的 ResourceAccess -> DX12 屏障
			for (const ResourceAccess& acc : pass.Accesses)
			{
				if (acc.Res == nullptr || acc.Res->GetResource() == nullptr)
					continue;

				// 跳过 UPLOAD / READBACK 固定态堆（防 #741）
				const D3D12_HEAP_TYPE heapType = acc.Res->GetHeapType();
				if (heapType == D3D12_HEAP_TYPE_UPLOAD || heapType == D3D12_HEAP_TYPE_READBACK)
					continue;

				if (acc.Access == PassAccess::AccelerationStructure)
				{
					// [契约 3] AS 永不发常规 Transition；若前序同资源刚写过/构建过则补 UAV 屏障
					auto prevIt = prevAccessMap.find(acc.Res);
					if (prevIt != prevAccessMap.end() && IsWriteAccess(prevIt->second))
						ctx.InsertUAVBarrier(*acc.Res);
					prevAccessMap[acc.Res] = acc.Access;
					continue;
				}

				const D3D12_RESOURCE_STATES targetState = TranslatePassAccess(acc.Access);

				// 检查 UAV -> UAV 连续访问（即使状态枚举相同，DX12 仍要求 UAV Barrier 同步写后读/写后写）
				const bool isUavTarget = (targetState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				if (isUavTarget && !acc.Res->HasSubresourceStates() &&
					acc.Res->GetUsageState() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				{
					ctx.InsertUAVBarrier(*acc.Res);
				}
				else
				{
					ctx.TransitionSubresource(*acc.Res, acc.Subresource, targetState);
				}

				prevAccessMap[acc.Res] = acc.Access;
			}

			// 2. Native Escape 入口护栏：强制 Flush 全部屏障并确保 4 分区单可见堆已绑定
			if (pass.IsNative)
			{
				ctx.FlushResourceBarriers();
				if (services != nullptr && ctx.GetSetDescriptorHeapsCount() == 0)
				{
					services->GetDescriptorHeap().BindToContext(ctx);
				}
			}

			// 3. 调用实际 Pass 回调（WP-2A/2B 已有 Pass 0 行重写！）
			if (pass.Callback)
				pass.Callback(ctx);

			// 4. Native Escape 出口护栏：作废 CommandContext 的 RS/PSO 缓存，防 DXR SetPipelineState1 / DirectML 污染后续 Pass
			if (pass.IsNative)
			{
				ctx.InvalidateCachedPipelineState();
			}

			if (timer != nullptr)
				timer->EndPass(ctx);
			ctx.PIXEndEvent();
		}
	}
}
