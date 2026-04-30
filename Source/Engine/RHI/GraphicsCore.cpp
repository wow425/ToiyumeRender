#include "PCH.h"
#include "GraphicsCore.h"
#include "../Application/GameCore.h"
#include "../Resource/ResourceManager/BufferManager.h"
// [非核心渲染功能 - 性能分析] #include "GpuTimeManager.h"
// [非核心渲染功能 - 后处理链] #include "PostEffects.h"
// [非核心渲染功能 - 环境光遮蔽] #include "SSAO.h"
// [非核心渲染功能 - UI与文本] #include "TextRenderer.h"
#include "../Resource/Buffer/ColorBuffer.h"
#include "SystemTime.h"
#include "../RHI/PipelineState/samplerManager.h"
#include "../RHI/DescriptorHeap/DescriptorHeap.h"
#include "../RHI/Command/CommandContext.h"
#include "../RHI/Command/CommandListManager.h"
#include "../RHI/PipelineState/RootSignature.h"
#include "../RHI/Command/CommandSignature.h"
// [非核心渲染功能 - 粒子特效] #include "ParticleEffectManager.h"
// [非核心渲染功能 - 性能图表] #include "GraphRenderer.h"
// [非核心渲染功能 - 时间抗锯齿等] #include "TemporalEffects.h"
#include "Display.h"

#pragma comment(lib, "d3d12.lib") 
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib") 

using namespace Math;

namespace Graphics
{
    // ?
#ifndef RELEASE
    const GUID WKPDID_D3DDebugObjectName = { 0x429b8c22,0x9188,0x4b0c, { 0x87,0x42,0xac,0xb0,0xbf,0x85,0xc2,0x00 } };
#endif

    bool g_bTypedUAVLoadSupport_R11G11B10_FLOAT = false;
    bool g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT = false;

    ID3D12Device* g_Device = nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory6> g_DXGIFactory;
    CommandListManager g_CommandManager;
    ContextManager g_ContextManager;



    DescriptorAllocator g_DescriptorAllocator[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] =
    {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV
    };

    static const uint32_t vendorID_Nvidia = 0x10DE;

    // 目前只nVidia
    uint32_t GetDesiredGPUVendor()
    {
        uint32_t desiredVendor = vendorID_Nvidia;
        return desiredVendor;
    }
    // 目前只nVidia
    const wchar_t* GPUVendorToString(uint32_t vendorID)
    {
        switch (vendorID)
        {
        case vendorID_Nvidia:
            return L"Nvidia";
        default:
            return L"Unknown";
            break;
        }
    }

    uint32_t GetVendorIdFromDevice(ID3D12Device* pDevice)
    {
        LUID luid = pDevice->GetAdapterLuid();

        // Obtain the DXGI factory
        Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
        ASSERT_SUCCEEDED(CreateDXGIFactory2(0, TY_IID_PPV_ARGS(&dxgiFactory)));

        Microsoft::WRL::ComPtr<IDXGIAdapter1> pAdapter;

        if (SUCCEEDED(dxgiFactory->EnumAdapterByLuid(luid, TY_IID_PPV_ARGS(&pAdapter))))
        {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(pAdapter->GetDesc1(&desc)))
            {
                return desc.VendorId;
            }
        }

        return 0;
    }

    bool IsDeviceNvidia(ID3D12Device* pDevice) { return GetVendorIdFromDevice(pDevice) == vendorID_Nvidia; }
}

// 初始化DX运行所需的资源Initialize the DirectX resources required to run.
void Graphics::Initialize(bool RequireDXRSupport)
{
    Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
    uint32_t useDebugLayers = 0;
#if _DEBUG
    useDebugLayers = 1;
#endif

    //
    // ===================== Debug Layer =====================
    //
    if (useDebugLayers)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugInterface;
        if (SUCCEEDED(D3D12GetDebugInterface(TY_IID_PPV_ARGS(&debugInterface))))
        {
            debugInterface->EnableDebugLayer();
#if _DEBUG
            //  GPU Validation
            Microsoft::WRL::ComPtr<ID3D12Debug1> debugInterface1;
            if (SUCCEEDED(debugInterface->QueryInterface(TY_IID_PPV_ARGS(&debugInterface1))))
            {
                debugInterface1->SetEnableGPUBasedValidation(true);
            }
            else
            {
                Utility::Print("WARNING:  Unable to enable D3D12 debug validation layer\n");
            }
#endif
        }
    }

    //
    // ===================== DXGI Factory =====================
    // 用于枚举gpu和创建设备
    DWORD dxgiFactoryFlags = 0;
    ASSERT_SUCCEEDED(CreateDXGIFactory2(dxgiFactoryFlags, TY_IID_PPV_ARGS(&g_DXGIFactory)));

    //
    // ===================== 获取唯一高性能GPU =====================
    //            
    Microsoft::WRL::ComPtr<IDXGIAdapter1> pAdapter;

    uint32_t desiredVendor = GetDesiredGPUVendor();
    if (desiredVendor)
    {
        Utility::Printf(L"Looking for a %s GPU\n", GPUVendorToString(desiredVendor));
    }
    ASSERT_SUCCEEDED(g_DXGIFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, TY_IID_PPV_ARGS(&pAdapter)));

    DXGI_ADAPTER_DESC1 desc = {};
    pAdapter->GetDesc1(&desc);

    if (SUCCEEDED(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_12_1, TY_IID_PPV_ARGS(&pDevice))))
        g_Device = pDevice.Detach();
    else
    {
        Utility::Printf(L"FAILED to create D3D12 device on this adapter");
        return;
    }

    Utility::Printf(L"Selected GPU:  %s (%u MB)\n", desc.Description, desc.DedicatedVideoMemory >> 20);

    if (RequireDXRSupport && !g_Device)
    {
        Utility::Printf("Unable to find a DXR-capable device. Halting.\n");
        __debugbreak();
    }

    //
    // ===================== InfoQueue（精简版） =====================
    //
#if _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(g_Device->QueryInterface(TY_IID_PPV_ARGS(&infoQueue))))
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        }
    }
#endif

    //
    // ===================== Feature Query =====================
    //
        // We like to do read-modify-write operations on UAVs during post processing.  To support that, we
        // need to either have the hardware do typed UAV loads of R11G11B10_FLOAT or we need to manually
        // decode an R32_UINT representation of the same buffer.  This code determines if we get the hardware
        // load support.
    D3D12_FEATURE_DATA_D3D12_OPTIONS FeatureData = {};
    if (SUCCEEDED(g_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &FeatureData, sizeof(FeatureData))))
    {
        if (FeatureData.TypedUAVLoadAdditionalFormats)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT Support =
            {
                DXGI_FORMAT_R11G11B10_FLOAT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE
            };

            if (SUCCEEDED(g_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &Support, sizeof(Support))) &&
                (Support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0)
            {
                g_bTypedUAVLoadSupport_R11G11B10_FLOAT = true;
            }

            Support.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

            if (SUCCEEDED(g_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &Support, sizeof(Support))) &&
                (Support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0)
            {
                g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT = true;
            }
        }
    }

    // 创建命令管理器
    g_CommandManager.Create(g_Device);

    // 初始化全局渲染状态
    InitializeCommonState();
    // 初始化显示模块 （交换链，后台缓冲区及描述符)
    Display::Initialize();
    /*初始化GPU时间戳模块
    GpuTimeManager::Initialize(4096);
     初始化TAA模块
    TemporalEffects::Initialize();
     初始化后处理管线
    PostEffects::Initialize();
     初始化SSAO
    SSAO::Initialize();
     初始化文本渲染
    TextRenderer::Initialize();
     初始化性能图标渲染器
    GraphRenderer::Initialize();
     初始化粒子系统管理器
    ParticleEffectManager::Initialize(3840, 2160);*/
}

void Graphics::Shutdown(void)
{
    // CPU挂起，等GPU处理完命令(
    g_CommandManager.IdleGPU();

    CommandContext::DestroyAllContexts();
    g_CommandManager.Shutdown();
    // GpuTimeManager::Shutdown();
    PSO::DestroyAll();
    RootSignature::DestroyAll();
    DescriptorAllocator::DestroyAll();

    DestroyCommonState();
    DestroyRenderingBuffers();
    Display::Shutdown();


    // 显存泄漏检测 (VRAM Leak Detection / ビデオメモリリーク検出)。扫描并打印出所有尚未被正确释放的 COM 对象
#if defined(_GAMING_DESKTOP) && defined(_DEBUG)
    ID3D12DebugDevice* debugInterface;
    if (SUCCEEDED(g_Device->QueryInterface(&debugInterface)))
    {
        debugInterface->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugInterface->Release();
    }
#endif

    if (g_Device != nullptr)
    {
        g_Device->Release();
        g_Device = nullptr;
    }
}
