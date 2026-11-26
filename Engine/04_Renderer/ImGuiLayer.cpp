#include "00_Core/PCH.h"
#include "04_Renderer/ImGuiLayer.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include "00_Core/Utility/Utility.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"

#include <vector>

// ImGui 的 Win32 头文件把 WndProcHandler 的声明放在 `#if 0` 里（注释要求使用者在自己的
// .cpp 中前向声明），这里照 backends/imgui_impl_win32.cpp 的写法补一次声明。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Renderer::ImGuiLayer
{
	namespace
	{
		// 回退私有堆容量（仅在未注入 UnifiedDescriptorHeap 时启用）。
		constexpr uint32_t kSRVHeapCapacity = 64;

		// 与引擎的帧资源数量解耦的固定值：ImGui DX12 后端会按它 new 一组每帧的
		// 顶点/索引缓冲，只要不中途改就行。
		constexpr int kNumFramesInFlight = 3;

		// [WP-1B / 契约 1] 优先复用全局 4 分区单可见堆 Zone D [49152, 49408)，
		// 彻底消除原 64 槽独立私有堆导致的 SetDescriptorHeaps 切堆与管线冲刷。
		UnifiedDescriptorHeap* g_UnifiedHeap = nullptr;

		// 兼容旧调用方（未传 unifiedHeap）的回退私有堆
		DescriptorHeap g_FallbackSRVHeap;
		std::vector<bool> g_FallbackSRVSlotUsed;

		bool g_Initialized = false;

		void AllocSRVDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
			D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
		{
			(void)info;

			if (g_UnifiedHeap != nullptr && g_UnifiedHeap->IsInitialized())
			{
				const BindlessAllocation alloc = g_UnifiedHeap->AllocUI();
				ASSERT(alloc.IsValid(), "ImGuiLayer Zone D 描述符槽已耗尽");
				*outCpuHandle = alloc.Cpu;
				*outGpuHandle = alloc.Gpu;
				return;
			}

			for (uint32_t i = 0; i < kSRVHeapCapacity; ++i)
			{
				if (g_FallbackSRVSlotUsed[i])
					continue;

				g_FallbackSRVSlotUsed[i] = true;
				DescriptorHandle handle = g_FallbackSRVHeap[i];
				*outCpuHandle = handle;
				*outGpuHandle = handle;
				return;
			}

			ASSERT(false, "ImGuiLayer 描述符堆已满，请增大 kSRVHeapCapacity");
			outCpuHandle->ptr = 0;
			outGpuHandle->ptr = 0;
		}

		void FreeSRVDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
		{
			(void)info;
			(void)gpuHandle;

			if (g_UnifiedHeap != nullptr && g_UnifiedHeap->IsInitialized())
			{
				g_UnifiedHeap->FreeUI(cpuHandle);
				return;
			}

			for (uint32_t i = 0; i < kSRVHeapCapacity; ++i)
			{
				DescriptorHandle handle = g_FallbackSRVHeap[i];
				if (handle.GetCpuPtr() != static_cast<size_t>(cpuHandle.ptr))
					continue;

				ASSERT(g_FallbackSRVSlotUsed[i], "ImGuiLayer 归还了一个未被占用的描述符槽位");
				g_FallbackSRVSlotUsed[i] = false;
				return;
			}

			ASSERT(false, "ImGuiLayer 归还的描述符不属于本模块的堆");
		}
	}

	bool Initialize(void* hwnd, UnifiedDescriptorHeap* unifiedHeap)
	{
		if (g_Initialized)
		{
			ASSERT(false, "ImGuiLayer::Initialize 被重复调用");
			return false;
		}

		if (hwnd == nullptr)
		{
			ASSERT(false, "ImGuiLayer::Initialize 收到空窗口句柄");
			return false;
		}

		if (Graphics::g_Device == nullptr)
		{
			ASSERT(false, "ImGuiLayer::Initialize 时 Graphics::g_Device 尚未创建");
			return false;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ID3D12DescriptorHeap* targetSrvHeap = nullptr;
		if (unifiedHeap != nullptr && unifiedHeap->IsInitialized())
		{
			g_UnifiedHeap = unifiedHeap;
			targetSrvHeap = unifiedHeap->GetCbvSrvUavHeap();
			LOG_INFO(L"[ImGuiLayer] 已接入 UnifiedDescriptorHeap Zone D [%u, %u)\n",
				UnifiedDescriptorHeap::kZoneDStart, UnifiedDescriptorHeap::kZoneDEnd);
		}
		else
		{
			g_UnifiedHeap = nullptr;
			g_FallbackSRVHeap.Create(L"ImGuiLayer Fallback SRV Heap", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSRVHeapCapacity);
			g_FallbackSRVSlotUsed.assign(kSRVHeapCapacity, false);
			targetSrvHeap = g_FallbackSRVHeap.GetHeapPointer();
		}

		if (!ImGui_ImplWin32_Init(hwnd))
		{
			ASSERT(false, "ImGui_ImplWin32_Init 失败");
			if (g_UnifiedHeap == nullptr)
				g_FallbackSRVHeap.Destroy();
			g_UnifiedHeap = nullptr;
			ImGui::DestroyContext();
			return false;
		}

		ImGui_ImplDX12_InitInfo initInfo = {};
		initInfo.Device = Graphics::g_Device;
		initInfo.CommandQueue = Graphics::g_CommandManager.GetGraphicsQueue().GetCommandQueue();
		initInfo.NumFramesInFlight = kNumFramesInFlight;
		initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		initInfo.UserData = nullptr;
		initInfo.SrvDescriptorHeap = targetSrvHeap;
		initInfo.SrvDescriptorAllocFn = &AllocSRVDescriptor;
		initInfo.SrvDescriptorFreeFn = &FreeSRVDescriptor;

		if (!ImGui_ImplDX12_Init(&initInfo))
		{
			ASSERT(false, "ImGui_ImplDX12_Init 失败");
			ImGui_ImplWin32_Shutdown();
			if (g_UnifiedHeap == nullptr)
				g_FallbackSRVHeap.Destroy();
			g_UnifiedHeap = nullptr;
			ImGui::DestroyContext();
			return false;
		}

		g_Initialized = true;
		return true;
	}

	void Shutdown(void)
	{
		if (!g_Initialized)
			return;

		// 严格反序：先调 ImGui_ImplDX12_Shutdown（回调 FreeSRVDescriptor 归还 Zone D 字体槽），
		// 再解绑 UnifiedDescriptorHeap 或销毁回退私有堆。
		ImGui_ImplDX12_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		if (g_UnifiedHeap == nullptr)
		{
			g_FallbackSRVHeap.Destroy();
			g_FallbackSRVSlotUsed.assign(kSRVHeapCapacity, false);
		}
		g_UnifiedHeap = nullptr;
		g_Initialized = false;
	}

	bool IsInitialized(void)
	{
		return g_Initialized;
	}

	void BeginFrame(void)
	{
		if (!g_Initialized)
			return;

		// Win32 后端负责填 DeltaTime/DisplaySize，DX12 后端的 NewFrame 依赖它们；
		// 所以 Win32 必须在 DX12 之前。
		ImGui_ImplWin32_NewFrame();
		ImGui_ImplDX12_NewFrame();
		ImGui::NewFrame();
	}

	void Render(GraphicsContext& context, ColorBuffer& target)
	{
		if (!g_Initialized)
			return;

		ImGui::Render();
		ImDrawData* drawData = ImGui::GetDrawData();

		// 空帧直接返回：没有 draw cmd 时下面的状态切换全是白做，还会平白插一次屏障。
		if (drawData == nullptr || drawData->CmdListsCount == 0)
			return;

		const uint32_t width = target.GetWidth();
		const uint32_t height = target.GetHeight();
		if (width == 0 || height == 0)
			return;

		context.TransitionResource(target, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
		context.SetRenderTarget(target.GetRTV());
		context.SetViewportAndScissor(0, 0, width, height);

		// ImGui 的 DX12 后端不主动调 SetDescriptorHeaps：当接入 UnifiedDescriptorHeap 时，
		// 绑定与主渲染路径完全一致的 65,536 槽统一堆（Zone D 字体 SRV）。
		if (g_UnifiedHeap != nullptr && g_UnifiedHeap->IsInitialized())
		{
			context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_UnifiedHeap->GetCbvSrvUavHeap());
		}
		else
		{
			context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_FallbackSRVHeap.GetHeapPointer());
		}

		// 每帧一次。DX12 后端内部会自增 frameIndex 轮换每帧的顶点/索引缓冲。
		ImGui_ImplDX12_RenderDrawData(drawData, context.GetCommandList());
	}

	bool WndProcHandler(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam)
	{
		if (!g_Initialized || hwnd == nullptr)
			return false;

		const LRESULT result = ImGui_ImplWin32_WndProcHandler(
			static_cast<HWND>(hwnd),
			static_cast<UINT>(msg),
			static_cast<WPARAM>(wParam),
			static_cast<LPARAM>(lParam));

		return result != 0;
	}

	bool WantCaptureMouse(void)
	{
		// 没有上下文时 GetIO() 会断言，所以先短路。
		return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
	}

	bool WantCaptureKeyboard(void)
	{
		return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
	}
}
