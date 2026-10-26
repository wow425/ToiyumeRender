#pragma once

// 为什么需要它：
//	引擎里需要 ImGui 的地方（主循环、调试面板、性能 HUD）都想要同一个入口，但 ImGui 的
//	DX12 后端是有状态的：Win32 后端要把窗口消息喂进去，DX12 后端自己持有字体 SRV、根签名、
//	PSO 和每帧的顶点/索引缓冲，而且必须严格按 NewFrame/Render/RenderDrawData 的顺序调用。
//	把这些散落在每个调用方，会导致初始化/销毁顺序各写一遍（尤其是「先关后端再销毁描述符堆」
//	这个隐含顺序），所以统一封装成这里的自由函数。
//
// 做法：
//	- 自建一个独立的 shader-visible CBV_SRV_UAV 描述符堆，不复用 Renderer::s_TextureHeap。
//	  原因：ImGui 字体 SRV 的生命周期和场景贴图完全不同，混在一起会在 Shutdown 时把字体 SRV
//	  归还到别人的堆里；而且 s_TextureHeap 的所有权在 BaseRenderer，本模块不应插手。
//	- Engine 的 DescriptorHeap 只有 Alloc、没有 Free，所以这里额外维护一张定长空闲表，
//	  按槽位索引（operator[]）取句柄，自己记录占用/空闲。
//	- 生命周期顺序：Initialize -> (BeginFrame + Render) 每帧 -> Shutdown。
//	  Shutdown 必须先调 ImGui_ImplDX12_Shutdown（它内部会回调 SrvDescriptorFreeFn 归还字体
//	  SRV），再销毁自建描述符堆，顺序反了就会访问已销毁的堆。
//
// 限制：
//	- NumFramesInFlight 固定为 3；Render 每帧只能调用一次（DX12 后端内部靠 frameIndex 自增
//	  轮换每帧的顶点/索引缓冲，一帧调两次会打乱帧资源）。
//	- Render 只负责把 ImGui 画到指定 ColorBuffer 上：不提交命令列表、不把目标切到别的状态，
//	  后续状态由调用方自己处理。
//	- 【重要】ImGui 的 DX12 后端直接对 ID3D12GraphicsCommandList 调 SetPipelineState /
//	  SetGraphicsRootSignature，绕过了引擎 CommandContext 的状态缓存，于是 Render 之后
//	  CommandContext 里记的「当前 PSO / 根签名 / 描述符堆」就和命令列表实际状态不一致了。
//	  所以 Render 必须是这条命令列表里最后一个绘制步骤；CommandContext 在 Begin/Reset 时会清缓存，
//	  下一帧自然恢复。若确实要在 ImGui 之后继续画东西，调用方得自己重新绑定根签名与描述符堆。
//	- 未初始化时所有接口都是空操作（WndProcHandler 返回 false），方便无窗口的测试路径直接跳过。
//	- 没有加载中文字体，ImGui 默认字体只有 ASCII 字形集；需要中文界面时得在 Initialize 里
//	  另外 AddFontFromFileTTF。

class ColorBuffer;
class GraphicsContext;

namespace Renderer::ImGuiLayer
{
	// 创建 ImGui 上下文并初始化 Win32/DX12 两个后端，同时建立本模块私有的描述符堆。
	// hwnd 是 Win32 后端需要的窗口句柄（void* 是为了让头文件不依赖 Windows.h）。
	// 重复调用返回 false；g_Device 为空或 hwnd 为空时返回 false。
	bool Initialize(void* hwnd);

	// 逆序销毁：DX12 后端 -> Win32 后端 -> ImGui 上下文 -> 自建描述符堆。可重复调用。
	void Shutdown(void);

	// 是否已初始化成功。所有接口在未初始化时都是安全的空操作。
	bool IsInitialized(void);

	// 每帧开始时调用一次：Win32 后端先填 DeltaTime/DisplaySize，DX12 后端再读它，
	// 所以顺序必须是 Win32 -> DX12 -> ImGui::NewFrame。
	void BeginFrame(void);

	// 每帧渲染时调用一次：ImGui::Render 取 draw data，切到目标 ColorBuffer 的 RTV 并画上去。
	// 空帧（没有 draw cmd）直接返回，不做任何状态切换；不自建/不提交命令列表。
	void Render(GraphicsContext& context, ColorBuffer& target);

	// 把窗口消息转发给 ImGui。返回 true 表示 ImGui 已消费该消息（调用方应跳过后续处理）。
	// 未初始化或 hwnd 为空时返回 false。
	bool WndProcHandler(void* hwnd, unsigned msg, uint64_t wParam, int64_t lParam);

	// 当前帧 ImGui 是否想要独占鼠标/键盘。相机等输入逻辑应该在这两个为 true 时跳过，
	// 否则拖动面板的同时场景相机会跟着动。
	bool WantCaptureMouse(void);
	bool WantCaptureKeyboard(void);
}
