
// 屏幕空间后流程所用的！！！



#include "00_Core/PCH.h"
#include "00_Core/Utility/Utility.h"
#include "00_Core/SystemTime.h"
#include "Display.h"
#include "02_RHI/Resource/ColorBuffer.h" 
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/BufferManager.h"
#include "01_Application/GameCore.h"   // R2：Resize 需要回调 IGameApp::OnResize




#pragma comment(lib, "dxgi.lib")

namespace GameCore { extern HWND g_hWnd; }



#define SWAP_CHAIN_BUFFER_COUNT 3




DXGI_FORMAT SwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // 交换链规格 DXGI_FORMAT_R8G8B8A8_UNORM DXGI_FORMAT_R16G16B16A16_FLOAT

using namespace Math;
using namespace Graphics;

namespace Display
{
	std::shared_ptr<ColorBuffer> SceneColorBuffer;
}
namespace
{
	float s_FrameTime = 0.0f;
	uint64_t s_FrameIndex = 0;
	int64_t s_FrameStartTick = 0;
}

namespace Graphics
{
	void PreparePresentSDR(ColorBuffer& SceneColorBuffer);


	enum eResolution { k720p, k900p, k1080p, k1440p, k1800p, k2160p };
	enum eEQAAQuality { kEQAA1x1, kEQAA1x8, kEQAA1x16 };

	const uint32_t kNumPredefinedResolutions = 6;

	uint32_t g_NativeWidth = 0;
	uint32_t g_NativeHeight = 0;
	uint32_t g_DisplayWidth = 1920;
	uint32_t g_DisplayHeight = 1080;
	int NativeResolution = k1080p;

	void ResolutionToUINT(eResolution res, uint32_t& width, uint32_t& height)
	{
		switch (res)
		{
		default:
		case k720p:
			width = 1280;
			height = 720;
			break;
		case k900p:
			width = 1600;
			height = 900;
			break;
		case k1080p:
			width = 1920;
			height = 1080;
			break;
		case k1440p:
			width = 2560;
			height = 1440;
			break;
		case k1800p:
			width = 3200;
			height = 1800;
			break;
		case k2160p:
			width = 3840;
			height = 2160;
			break;
		}
	}

	// 动态分辨率调整
	void SetNativeResolution(void)
	{
		uint32_t NativeWidth, NativeHeight;

		ResolutionToUINT(eResolution((int)NativeResolution), NativeWidth, NativeHeight);

		if (g_NativeWidth == NativeWidth && g_NativeHeight == NativeHeight)
			return;
		DEBUGPRINT("Changing native resolution to %ux%u", NativeWidth, NativeHeight);

		g_NativeWidth = NativeWidth;
		g_NativeHeight = NativeHeight;

		g_CommandManager.IdleGPU();

		// InitializeRenderingBuffers(NativeWidth, NativeHeight);
	}

	void SetDisplayResolution(void)
	{
#ifdef _GAMING_DESKTOP
		static int SelectedDisplayRes = DisplayResolution;
		if (SelectedDisplayRes == DisplayResolution)
			return;

		SelectedDisplayRes = DisplayResolution;
		ResolutionToUINT((eResolution)SelectedDisplayRes, g_DisplayWidth, g_DisplayHeight);
		DEBUGPRINT("Changing display resolution to %ux%u", g_DisplayWidth, g_DisplayHeight);

		g_CommandManager.IdleGPU();

		Display::Resize(g_DisplayWidth, g_DisplayHeight);

		SetWindowPos(GameCore::g_hWnd, 0, 0, 0, g_DisplayWidth, g_DisplayHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
	}

	ColorBuffer g_DisplayPlane[SWAP_CHAIN_BUFFER_COUNT];
	UINT g_CurrentBuffer = 0;

	// R2：升级到 IDXGISwapChain3。
	// 原因：FLIP_DISCARD 模型下，当前可写的 backbuffer 索引**由 DXGI 决定**，
	// 不能用 "+1 % N" 手工递推。DXGI 会跳过正在显示的缓冲区，
	// 手工递推可能算到线程正在读的那个，导致渲染到正在显示的 buffer。
	// GetCurrentBackBufferIndex() 才是权威来源。
	// IDXGISwapChain3 是 DXGI 1.4 接口，Win10 起可用，本工程已满足。
	IDXGISwapChain3* s_SwapChain1 = nullptr;

	RootSignature s_PresentRS;
	GraphicsPSO PresentSDRPS(L"Core: PresentSDR");
}

void Display::Resize(uint32_t width, uint32_t height)
{
	// ============================================================
	// 零尺寸守卫（R2）
	// ------------------------------------------------------------
	// 窗口最小化时 WM_SIZE 会送来 0×0。此时：
	//   - ResizeBuffers(0, 0) 会失败
	//   - 各项 ColorBuffer 无法创建（D3D12 不允许 0 尺寸资源）
	//   - 渲染循环若继续跑，会拿空资源做 CopyResource
	// 正确做法是忽略这次 resize，等窗口恢复时再来。
	// ============================================================
	if (width == 0 || height == 0)
	{
		LOG_TRACE(L"[Display] Ignoring resize to %ux%u (minimized)\n", width, height);
		return;
	}

	// 同一尺寸无需重建（ResizeBuffers 每次都重建代价不小）
	if (width == g_DisplayWidth && height == g_DisplayHeight && s_SwapChain1 != nullptr)
	{
		LOG_TRACE(L"[Display] Resize skipped, already %ux%u\n", width, height);
		return;
	}

	// 全部队列闲置，等待围栏值
	g_CommandManager.IdleGPU();

	g_DisplayWidth = width;
	g_DisplayHeight = height;
	// 告知当前画质
	DEBUGPRINT("Changing display resolution to %ux%u", width, height);
	// 清空后台缓冲数组
	for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
		g_DisplayPlane[i].Destroy();
	// 确保交换链无误
	ASSERT(s_SwapChain1 != nullptr);
	// ============================================================
	// R4.4：ResizeBuffers 的 SwapChainFlags 必须与创建时**完全一致**
	// ------------------------------------------------------------
	// 症状：Deferred 下反复拉伸窗口，若干次后闪退。
	// 退出码 0xC000041D（窗口回调中未处理异常），日志显示
	//     ResizeBuffers FAILED hr=0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)
	//
	// 根因：创建交换链时用的 flags 是
	//     swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	// 而这里 ResizeBuffers 传的是 0。DXGI 要求两者的 SwapChainFlags
	// 一致，否则 ResizeBuffers 失败并可能连带设备被移除。
	//
	// 为什么"有时候能过几次才崩"：前几次 ResizeBuffers 在驱动容忍范围内
	// 勉强成功，累积到某个点后设备被移除 —— 表现为难以复现的间歇性崩溃。
	//
	// 修复：把创建时用的 flags 提取成具名常量，两处共用同一个值。
	// ============================================================
	const UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	HRESULT rbHr = s_SwapChain1->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height,
		SwapChainFormat, kSwapChainFlags);
	if (FAILED(rbHr))
	{
		// 设备真正丢失时已经没有恢复余地，但至少要让用户看到明确原因，
		// 而不是一个"莫名闪退"。
		LOG_INFO(L"[Display] ResizeBuffers 失败 hr=0x%08X（%ux%u）\n",
			(unsigned)rbHr, width, height);
		return;
	}
	// 为后台缓冲数组创建交换链资源
	for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
	{
		ComPtr<ID3D12Resource> DisplayPlane;
		ASSERT_SUCCEEDED(s_SwapChain1->GetBuffer(i, TY_IID_PPV_ARGS(&DisplayPlane)));
		g_DisplayPlane[i].CreateFromSwapChain(L"Primary SwapChain Buffer", DisplayPlane.Detach());
	}

	// ============================================================
	// R4.4：ResizeBuffers 后必须重新查询当前 backbuffer 索引
	// ------------------------------------------------------------
	// 这是 DEVICE_REMOVAL(EXECUTION ERROR #232, DXGI_ERROR_ACCESS_DENIED)
	// 的直接原因：
	//    ResizeBuffers 会重建全部 backbuffer，DXGI 的"当前可写索引"
	//    也随之改变。但 g_CurrentBuffer 仍是 resize 之前的值。
	//    于是下一帧 PreparePresentSDR 对这个索引对应的 buffer 发
	//    COPY_DEST 屏障并 CopyResource —— 而该 buffer 实际处于 PRESENT
	//    或其他状态，GPU 侧判定为"访问了不该访问的资源"，直接移除设备。
	//
	// 注意：这里**必须先重绑 g_DisplayPlane[]**（上面已完成），
	// 因为 GetCurrentBackBufferIndex 返回的是新交换链的索引。
	// ============================================================
	g_CurrentBuffer = s_SwapChain1->GetCurrentBackBufferIndex();

	// 强制等待确保上述创建完成
	g_CommandManager.IdleGPU();

	// ============================================================
	// 通知 App 重建它自己的渲染目标（R2 接通了这条断链）
	// ------------------------------------------------------------
	// 这是 R2 的核心：原先 swapchain 重建完成后就结束了，
	// 上层 GBuffer / SceneColor / Depth 仍是旧尺寸，
	// 导致 PreparePresentSDR 的 CopyResource 源(新尺寸的
	// backbuffer)与目的(旧尺寸的 SceneColorBuffer)不一致。
	//
	// 转发顺序很重要：必须在 backbuffer 重建完成之后，
	// 因为 App 可能会直接引用 swapchain 的尺寸信息。
	// ============================================================
	if (GameCore::g_pActiveApp != nullptr)
	{
		GameCore::g_pActiveApp->OnResize(width, height);
	}
}

// 
void Display::Initialize(void)
{
	// 创交换链
	LOG_TRACE(L"[Boot] Display: create DXGIFactory (hWnd=%p)\n", GameCore::g_hWnd);
	ASSERT(s_SwapChain1 == nullptr, "Graphics has already been initialized");
	ASSERT_SUCCEEDED(CreateDXGIFactory2(0, TY_IID_PPV_ARGS(&g_DXGIFactory)));
	LOG_TRACE(L"[Boot] Display: factory OK, creating swapchain %ux%u\n", g_DisplayWidth, g_DisplayHeight);

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = g_DisplayWidth;
	swapChainDesc.Height = g_DisplayHeight;
	swapChainDesc.Format = SwapChainFormat;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.Scaling = DXGI_SCALING_NONE;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	// 注意：ResizeBuffers 必须传入**相同的** flags（见 Display::Resize 里的说明）。
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
	fsSwapChainDesc.Windowed = TRUE;

	// CreateSwapChainForHwnd 输出 IDXGISwapChain1*，需要 QueryInterface 升级到 3
	{
		IDXGISwapChain1* swapChain1 = nullptr;
		ASSERT_SUCCEEDED(g_DXGIFactory->CreateSwapChainForHwnd(
			g_CommandManager.GetCommandQueue(),
			GameCore::g_hWnd,
			&swapChainDesc,
			&fsSwapChainDesc,
			nullptr,
			&swapChain1));

		ASSERT_SUCCEEDED(swapChain1->QueryInterface(TY_IID_PPV_ARGS(&s_SwapChain1)));
		swapChain1->Release();
	}

	for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
	{
		ComPtr<ID3D12Resource> DisplayPlane;
		ASSERT_SUCCEEDED(s_SwapChain1->GetBuffer(i, TY_IID_PPV_ARGS(&DisplayPlane)));
		g_DisplayPlane[i].CreateFromSwapChain(L"Primary SwapChain Buffer", DisplayPlane.Detach());
	}

	// 配置封装根签名
	// 最终 Present Pass用根签名（后处理→屏幕）
	//s_PresentRS.Reset(4, 2); // 根签名设置为4根参，2静态采样器

	//PresentSDRPS.SetRootSignature(s_PresentRS);
	// SetNativeResolution();
}

void Display::Shutdown(void)
{
	s_SwapChain1->SetFullscreenState(FALSE, nullptr);
	s_SwapChain1->Release();

	for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
		g_DisplayPlane[i].Destroy();
}

// 原本是最终 Present Pass（后处理→屏幕）
// 目前不需要，直接 Present 后交换链后台缓冲区到屏幕
void Display::Present()
{
	// ============================================================
	// R2：空指针守卫
	// ------------------------------------------------------------
	// 最小化期间 WM_SIZE 送来 0×0 被 Resize 忽略，但渲染循环仍在跑。
	// 此时若 SceneColorBuffer 为空（例如 App 尚未 Startup，或
	// OnResize 尚未重绑），直接解引用会崩溃。
	// 跳过整个呈现环节，等窗口恢复后再正常呈现。
	// ============================================================
	// R4.4：这里必须连 GetResource() 一起判断。
	// SceneColorBuffer 是 shared_ptr，Resize 期间对象还活着但底层资源已被
	// Destroy —— 只看指针非空会漏掉这种情况。
	if (Display::SceneColorBuffer == nullptr || s_SwapChain1 == nullptr)
		return;
	if (Display::SceneColorBuffer->GetResource() == nullptr)
	{
		LOG_TRACE(L"[Display] Present skipped: SceneColorBuffer resource is null\n");
		return;
	}

	// 最终 Present Pass（后处理→屏幕）。将先前绘制的 scenecolorbuffer 复制给最后呈现用的 back buffer 并 present
	PreparePresentSDR(*Display::SceneColorBuffer);

	HRESULT presentHr = s_SwapChain1->Present(0, 0); // s_SwapChain1绑定的是DisplayPlane

	// R4.2：Present 的返回码必须可观测。
	// 之前窗口全黑时，代码在 PreparePresentSDR 的格式校验处就 return 了，
	// Present 从未执行；而那条失败信息是 LOG_TRACE，在非 VERBOSE 下被
	// 编译成空语句 —— 于是"没有报错但就是不显示"，极难定位。
	// 这里把"Present 真的失败"这种情况提升为可观测的日志。
	if (FAILED(presentHr))
	{
		LOG_INFO(L"[Display] Present 失败 hr=0x%08X backbuffer=%u\n",
			(unsigned)presentHr, (uint32_t)g_CurrentBuffer);
	}

	// R2：Present 之后向 DXGI 查询下一次可写的 backbuffer 索引。
	// 不再手工 "+1 % N" —— FLIP 模型下索引由 DXGI 管理，
	// 手工递推可能与 DXGI 的真实状态脱节。
	g_CurrentBuffer = s_SwapChain1->GetCurrentBackBufferIndex();

	int64_t CurrentTick = SystemTime::GetCurrentTick();
	s_FrameTime = (float)SystemTime::TimeBetweenTicks(s_FrameStartTick, CurrentTick);
	s_FrameStartTick = CurrentTick;

	++s_FrameIndex;

	// 动态分辨率模型（Runtime Resolution System）
	// SetNativeResolution();
	// SetDisplayResolution();
}


// 最终 Present Pass（后处理→屏幕）
void Graphics::PreparePresentSDR(ColorBuffer& SceneColorBuffer)
{
	// ============================================================
	// R2：尺寸一致性校验
	// ------------------------------------------------------------
	// CopyResource 要求源与目的**尺寸和格式完全一致**，否则
	// Debug Layer 报错、运行行为未定义。
	//
	// 正常情况下 OnResize 已保证 Renderer 的 SceneColorBuffer 与
	// swapchain backbuffer 同尺寸。这里是最后一道防线：
	// 一旦 Resize 时序出问题（例如 App 覆写 OnResize 时忘了重绑），
	// 宁可这一帧不呈现，也不要把尺寸不匹配的命令提交上去。
	// ============================================================
	// ============================================================
	// R4.4：空资源守卫（Resize 期间的窗口期）
	// ------------------------------------------------------------
	// Resize 的顺序是：Display::Resize → App::OnResize → Renderer::OnResize
	// → DestroyDeferredBufferTargets()（把旧 SceneColorBuffer 的资源 Destroy 掉）
	// → CreateDeferredBufferTargets()（建新的）→ App 重新 BindRendererOutput()。
	//
	// 问题在于 Display::SceneColorBuffer 是一个 shared_ptr：Destroy() 只是
	// 释放底层 ID3D12Resource（m_pResource 被 Reset），**对象本身还活着**。
	// 因此 `SceneColorBuffer != nullptr` 这道守卫拦不住它 —— 必须再检查
	// GetResource() 是否为空，否则下一句 GetResource()->GetDesc() 就是
	// 对空指针解引用，表现为「拉伸窗口直接闪退」。
	//
	// 这里宁可跳过这一帧呈现，也不要崩溃：Resize 完成后目标会被重建并重绑。
	// ============================================================
	if (SceneColorBuffer.GetResource() == nullptr ||
		g_DisplayPlane[g_CurrentBuffer].GetResource() == nullptr)
	{
		LOG_TRACE(L"[Display] Present skipped: resource is null (resize in progress)\n");
		return;
	}

	const D3D12_RESOURCE_DESC srcDesc = SceneColorBuffer.GetResource()->GetDesc();
	const D3D12_RESOURCE_DESC dstDesc = g_DisplayPlane[g_CurrentBuffer].GetResource()->GetDesc();

	// ============================================================
	// R4.2：格式比较必须用"位布局相同"，而不是枚举值字面相等。
	// ------------------------------------------------------------
	// 症状：窗口全黑，GPU 回读（DebugCapture）却显示渲染完全正常。
	// 原因：本工程所有 ColorBuffer 在创建时都会把格式换成 *_TYPELESS
	//       （PixelBuffer::DescribeTex2D 调 GetBaseFormat，这是迷你引擎为
	//        后续创建 UAV/SRV 视图预留的故意设计）。于是：
	//           SceneColorBuffer 的实际资源格式 = R8G8B8A8_TYPELESS (27)
	//           swapchain backbuffer      fmt = R8G8B8A8_UNORM     (28)
	//       两者位布局完全一致，CopyResource 合法；但原先的
	//       `srcDesc.Format != dstDesc.Format` 判等让每一帧都"尺寸/格式不匹配"，
	//       于是 PreparePresentSDR 直接 return —— 从不拷贝、也从不 Present。
	//
	// 更隐蔽的是：这条早退原本用的是 LOG_TRACE，而 LOG_TRACE 在非
	// TOOIYUME_VERBOSE_LOG 下被编译成 do{}while(0)，所以日志里一片安静，
	// 表现为"没有报错但就是不显示"。
	//
	// 现在改为：宽高必须相等；格式必须"可互相拷贝"（TYPELESS 与其具体
	// 类型视为同一布局）。真正不可拷贝的情况仍然拦下来，并把日志提到
	// LOG_INFO，避免同类问题再次静默。
	// ============================================================
	// 注意：这里刻意不调用 PixelBuffer::GetBaseFormat —— 它是 protected，
	// 而且 Display 层不应反向依赖 RHI 的内部格式归一化细节。
	// CopyResource 的硬性约束只有两条：宽高一致、**单个元素位宽与布局一致**。
	// 因此只需判断"两边是否属于同一个位布局家族"。
	auto IsCopyCompatible = [](DXGI_FORMAT a, DXGI_FORMAT b) -> bool
	{
		if (a == b)
			return true;

		// 把 TYPELESS / UNORM / UNORM_SRGB 等归到同一个"家族"。
		// 这些转换在 DXGI 里位宽相同、通道顺序相同，CopyResource 合法
		// （SRGB 只是采样/写入时的传递函数标记，不改变存储布局）。
		auto Family = [](DXGI_FORMAT f) -> int
		{
			switch (f)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SNORM:
			case DXGI_FORMAT_R8G8B8A8_SINT:
				return 1;

			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return 2;

			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R16G16B16A16_UINT:
				return 3;

			case DXGI_FORMAT_R16G16_TYPELESS:
			case DXGI_FORMAT_R16G16_FLOAT:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R16G16_UINT:
				return 4;

			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_UINT:
			case DXGI_FORMAT_R32_TYPELESS:
				return 5;

			default:
				return -1; // 未知格式：不做放宽，回落到严格比较
			}
		};

		const int fa = Family(a);
		const int fb = Family(b);
		return fa != -1 && fa == fb;
	};

	if (srcDesc.Width  != dstDesc.Width ||
		srcDesc.Height != dstDesc.Height ||
		!IsCopyCompatible(srcDesc.Format, dstDesc.Format))
	{
		LOG_INFO(L"[Display] Present skipped: incompatible src=%ux%u fmt=%d dst=%ux%u fmt=%d\n",
			(uint32_t)srcDesc.Width, srcDesc.Height, (int)srcDesc.Format,
			(uint32_t)dstDesc.Width, dstDesc.Height, (int)dstDesc.Format);
		return;
	}

	GraphicsContext& Context = GraphicsContext::Begin(L"Present");

	Context.TransitionResource(SceneColorBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_COPY_DEST);
	Context.CopyBuffer(g_DisplayPlane[g_CurrentBuffer], SceneColorBuffer);


Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_PRESENT);

	Context.Finish();
}

uint64_t Graphics::GetFrameCount(void) { return s_FrameIndex; }
float Graphics::GetFrameTime(void) { return s_FrameTime; }
float Graphics::GetFrameRate(void) { return s_FrameTime == 0.0f ? 0.0f : 1.0f / s_FrameTime; }