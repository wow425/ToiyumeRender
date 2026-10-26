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

#include <vector>

// ImGui 的 Win32 头文件把 WndProcHandler 的声明放在 `#if 0` 里（注释要求使用者在自己的
// .cpp 中前向声明），这里照 backends/imgui_impl_win32.cpp 的写法补一次声明。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Renderer::ImGuiLayer
{
	namespace
	{
		// 描述符堆容量。ImGui 自己只用 1 个字体 SRV，留出余量给以后往 UI 里贴图用。
		constexpr uint32_t kSRVHeapCapacity = 64;

		// 与引擎的帧资源数量解耦的固定值：ImGui DX12 后端会按它 new 一组每帧的
		// 顶点/索引缓冲，只要不中途改就行。
		constexpr int kNumFramesInFlight = 3;

		// 本模块私有的 shader-visible CBV_SRV_UAV 堆，不复用 Renderer::s_TextureHeap。
		DescriptorHeap g_SRVHeap;

		// DescriptorHeap 只有 Alloc 没有 Free，这里自己记占用情况：true = 已被占用。
		std::vector<bool> g_SRVSlotUsed;

		bool g_Initialized = false;

		// 从空闲表里挑一个槽位，转成 ImGui 需要的 CPU/GPU 句柄。
		// 注意这里不走 DescriptorHeap::Alloc：Alloc 只 bump 游标、归还后无法复用，
		// 而 ImGui 在 InvalidateDeviceObjects/CreateDeviceObjects 里会反复申请/归还字体 SRV。
		void AllocSRVDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
			D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
		{
			(void)info;

			for (uint32_t i = 0; i < kSRVHeapCapacity; ++i)
			{
				if (g_SRVSlotUsed[i])
					continue;

				g_SRVSlotUsed[i] = true;
				DescriptorHandle handle = g_SRVHeap[i];
				*outCpuHandle = handle;
				*outGpuHandle = handle;
				return;
			}

			// 走到这里说明容量算错了或者有槽位没归还，返回空句柄让后续断言炸出来。
			ASSERT(false, "ImGuiLayer 描述符堆已满，请增大 kSRVHeapCapacity");
			outCpuHandle->ptr = 0;
			outGpuHandle->ptr = 0;
		}

		// 按 CPU 句柄反查槽位并归还。ImGui 传回来的句柄就是 AllocSRVDescriptor 发出去的那份。
		void FreeSRVDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
			D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
		{
			(void)info;
			(void)gpuHandle;

			for (uint32_t i = 0; i < kSRVHeapCapacity; ++i)
			{
				DescriptorHandle handle = g_SRVHeap[i];
				if (handle.GetCpuPtr() != static_cast<size_t>(cpuHandle.ptr))
					continue;

				ASSERT(g_SRVSlotUsed[i], "ImGuiLayer 归还了一个未被占用的描述符槽位");
				g_SRVSlotUsed[i] = false;
				return;
			}

			ASSERT(false, "ImGuiLayer 归还的描述符不属于本模块的堆");
		}
	}

	bool Initialize(void* hwnd)
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

		// 先建自建堆：ImGui_ImplDX12_Init 内部就会调 SrvDescriptorAllocFn 要一个字体 SRV。
		g_SRVHeap.Create(L"ImGuiLayer SRV Heap", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSRVHeapCapacity);
		g_SRVSlotUsed.assign(kSRVHeapCapacity, false);

		if (!ImGui_ImplWin32_Init(hwnd))
		{
			ASSERT(false, "ImGui_ImplWin32_Init 失败");
			g_SRVHeap.Destroy();
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
		initInfo.SrvDescriptorHeap = g_SRVHeap.GetHeapPointer();
		initInfo.SrvDescriptorAllocFn = &AllocSRVDescriptor;
		initInfo.SrvDescriptorFreeFn = &FreeSRVDescriptor;

		if (!ImGui_ImplDX12_Init(&initInfo))
		{
			ASSERT(false, "ImGui_ImplDX12_Init 失败");
			ImGui_ImplWin32_Shutdown();
			g_SRVHeap.Destroy();
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

		// 顺序不能变：ImGui_ImplDX12_Shutdown 内部会调 SrvDescriptorFreeFn 归还字体 SRV，
		// 所以自建堆必须活到它之后。
		ImGui_ImplDX12_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		g_SRVHeap.Destroy();
		g_SRVSlotUsed.assign(kSRVHeapCapacity, false);
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

		// ImGui 的 DX12 后端只管根签名/PSO，全文没有 SetDescriptorHeaps：字体 SRV 的 GPU 句柄
		// 指向哪个堆，得由调用方自己绑。不绑的话 SetGraphicsRootDescriptorTable 拿到的是悬空句柄。
		context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_SRVHeap.GetHeapPointer());

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
