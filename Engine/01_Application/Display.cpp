
// 屏幕空间后流程所用的！！！



#include "00_Core/PCH.h"
#include "00_Core/SystemTime.h"
#include "Display.h"
#include "02_RHI/Resource/ColorBuffer.h" 
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "04_Renderer/BufferManager.h"




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

	IDXGISwapChain1* s_SwapChain1 = nullptr;

	RootSignature s_PresentRS;
	GraphicsPSO PresentSDRPS(L"Core: PresentSDR");
}

void Display::Resize(uint32_t width, uint32_t height)
{
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
	ASSERT_SUCCEEDED(s_SwapChain1->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height, SwapChainFormat, 0));
	// 为后台缓冲数组创建交换链资源
	for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
	{
		ComPtr<ID3D12Resource> DisplayPlane;
		ASSERT_SUCCEEDED(s_SwapChain1->GetBuffer(i, TY_IID_PPV_ARGS(&DisplayPlane)));
		g_DisplayPlane[i].CreateFromSwapChain(L"Primary SwapChain Buffer", DisplayPlane.Detach());
	}

	g_CurrentBuffer = 0;
	// 强制等待确保上述创建完成
	g_CommandManager.IdleGPU();
	// 创建叠加层，水平模糊缓冲区
	// ResizeDisplayDependentBuffers(g_NativeWidth, g_NativeHeight);
}

// 
void Display::Initialize(void)
{
	// 创交换链
	ASSERT(s_SwapChain1 == nullptr, "Graphics has already been initialized");
	ASSERT_SUCCEEDED(CreateDXGIFactory2(0, TY_IID_PPV_ARGS(&g_DXGIFactory)));

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
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
	fsSwapChainDesc.Windowed = TRUE;

	ASSERT_SUCCEEDED(g_DXGIFactory->CreateSwapChainForHwnd(
		g_CommandManager.GetCommandQueue(),
		GameCore::g_hWnd,
		&swapChainDesc,
		&fsSwapChainDesc,
		nullptr,
		&s_SwapChain1));

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
	// 最终 Present Pass（后处理→屏幕）。将先前绘制的scenecolorbuffer复制给最后呈现用的back buffer并present。目前阉割
	PreparePresentSDR(*Display::SceneColorBuffer);

	s_SwapChain1->Present(0, 0); // s_SwapChain1绑定的是DisplayPlane

	g_CurrentBuffer = (g_CurrentBuffer + 1) % SWAP_CHAIN_BUFFER_COUNT;

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
