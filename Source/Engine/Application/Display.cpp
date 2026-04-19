#include "PCH.h"
#include "Display.h"
#include "../RHI/GraphicsCore.h"
#include "../Resource/ResourceManager/BufferManager.h"
#include "../Resource/Buffer/ColorBuffer.h"
#include "SystemTime.h"
#include "../RHI/Command/CommandContext.h"
#include "../RHI/PipelineState/RootSignature.h"

#pragma comment(lib, "dxgi.lib")

namespace GameCore { extern HWND g_hWnd; }

#include "CompiledShaders/ScreenQuadPresentVS.h"
#include "CompiledShaders/PresentSDRPS.h"



#define SWAP_CHAIN_BUFFER_COUNT 3

DXGI_FORMAT SwapChainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

using namespace Math;
using namespace Graphics;

namespace
{
    float s_FrameTime = 0.0f;
    uint64_t s_FrameIndex = 0;
    int64_t s_FrameStartTick = 0;
}

namespace Graphics
{
    void PreparePresentSDR();


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

        InitializeRenderingBuffers(NativeWidth, NativeHeight);
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
    ResizeDisplayDependentBuffers(g_NativeWidth, g_NativeHeight);
}

// 初始化DX运行所需的资源
void Display::Initialize(void)
{
    // 创交换链
    ASSERT(s_SwapChain1 == nullptr, "Graphics has already been initialized");

    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    ASSERT_SUCCEEDED(CreateDXGIFactory2(0, TY_IID_PPV_ARGS(&dxgiFactory)));

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

    ASSERT_SUCCEEDED(dxgiFactory->CreateSwapChainForHwnd(
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
    s_PresentRS.Reset(4, 2); // 根签名设置为4根参，2静态采样器
    s_PresentRS[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    s_PresentRS[1].InitAsConstants(0, 6, D3D12_SHADER_VISIBILITY_ALL);
    s_PresentRS[2].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_PIXEL);
    s_PresentRS[3].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 2);
    s_PresentRS.InitStaticSampler(0, SamplerLinearClampDesc);
    s_PresentRS.InitStaticSampler(1, SamplerPointClampDesc);
    s_PresentRS.Finalize(L"Present");

    // 配置封装PSO
    PresentSDRPS.SetRootSignature(s_PresentRS);
    PresentSDRPS.SetRasterizerState(RasterizerTwoSided);
    PresentSDRPS.SetBlendState(BlendDisable);
    PresentSDRPS.SetDepthStencilState(DepthStateDisabled);
    PresentSDRPS.SetSampleMask(0xFFFFFFFF);
    PresentSDRPS.SetInputLayout(0, nullptr);
    PresentSDRPS.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    PresentSDRPS.SetVertexShader(g_pScreenQuadPresentVS, sizeof(g_pScreenQuadPresentVS));
    PresentSDRPS.SetPixelShader(g_pPresentSDRPS, sizeof(g_pPresentSDRPS));
    PresentSDRPS.SetRenderTargetFormat(SwapChainFormat, DXGI_FORMAT_UNKNOWN);
    PresentSDRPS.Finalize();

    SetNativeResolution();
}

void Display::Shutdown(void)
{
    s_SwapChain1->SetFullscreenState(FALSE, nullptr);
    s_SwapChain1->Release();

    for (UINT i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
        g_DisplayPlane[i].Destroy();
}

void Display::Present(void)
{
    PreparePresentSDR();

    s_SwapChain1->Present(0, 0);

    g_CurrentBuffer = (g_CurrentBuffer + 1) % SWAP_CHAIN_BUFFER_COUNT;

    int64_t CurrentTick = SystemTime::GetCurrentTick();
    s_FrameTime = (float)SystemTime::TimeBetweenTicks(s_FrameStartTick, CurrentTick);
    s_FrameStartTick = CurrentTick;

    ++s_FrameIndex;

    SetNativeResolution();
    SetDisplayResolution();
}


void Graphics::PreparePresentSDR(void)
{
    GraphicsContext& Context = GraphicsContext::Begin(L"Present");

    Context.SetRootSignature(s_PresentRS);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 屏障转换：准备读取 3D 场景缓冲区
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.SetDynamicDescriptor(0, 0, g_SceneColorBuffer.GetSRV());

    // 直接将目标指向交换链后台缓冲区
    ColorBuffer& Dest = g_DisplayPlane[g_CurrentBuffer];

    // 使用最基础的 Present PSO 进行直画
    Context.SetPipelineState(PresentSDRPS);
    Context.TransitionResource(Dest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Context.SetRenderTarget(Dest.GetRTV());
    Context.SetViewportAndScissor(0, 0, g_NativeWidth, g_NativeHeight);
    Context.Draw(3);

    // 屏障转换：将交换链资源转换为呈递状态
    Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_PRESENT);

    Context.Finish();
}

uint64_t Graphics::GetFrameCount(void) { return s_FrameIndex; }
float Graphics::GetFrameTime(void) { return s_FrameTime; }
float Graphics::GetFrameRate(void) { return s_FrameTime == 0.0f ? 0.0f : 1.0f / s_FrameTime; }
