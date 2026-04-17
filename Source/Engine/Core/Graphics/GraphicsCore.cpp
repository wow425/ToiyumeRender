#include "PCH.h"
#include "GraphicsCore.h"
#include "../GameCore.h"
#include "../Graphics/GpuResource/BufferManager.h"
// [非核心渲染功能 - 性能分析] #include "GpuTimeManager.h"
// [非核心渲染功能 - 后处理链] #include "PostEffects.h"
// [非核心渲染功能 - 环境光遮蔽] #include "SSAO.h"
// [非核心渲染功能 - UI与文本] #include "TextRenderer.h"
#include "../Graphics/GpuResource/ColorBuffer.h"
#include "SystemTime.h"
#include "../Graphics/PipelineState/samplerManager.h"
#include "../Graphics/DescriptorAllocators/DescriptorHeap.h"
#include "../Graphics/Command/CommandContext.h"
#include "../Graphics/Command/CommandListManager.h"
#include "../Graphics/PipelineState/RootSignature.h"
#include "../Graphics/PipelineState/CommandSignature.h"
// [非核心渲染功能 - 粒子特效] #include "ParticleEffectManager.h"
// [非核心渲染功能 - 性能图表] #include "GraphRenderer.h"
// [非核心渲染功能 - 时间抗锯齿等] #include "TemporalEffects.h"
#include "Display.h"

#pragma comment(lib, "d3d12.lib") 

using namespace Math;

namespace Graphics
{
    // ?
#ifndef RELEASE
    const GUID WKPDID_D3DDebugObjectName = { 0x429b8c22,0x9188,0x4b0c, { 0x87,0x42,0xac,0xb0,0xbf,0x85,0xc2,0x00 } };
#endif

    // [非核心渲染功能 - 仅用于复杂后处理(如泛光)的Typed UAV Load硬件支持检测]
    // bool g_bTypedUAVLoadSupport_R11G11B10_FLOAT = false;
    // bool g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT = false;

    ID3D12Device* g_Device = nullptr;
    CommandListManager g_CommandManager;
    ContextManager g_ContextManager;

    D3D_FEATURE_LEVEL g_D3DFeatureLevel = D3D_FEATURE_LEVEL_11_0;

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
    // Default to true for debug builds
    useDebugLayers = 1;
#endif

    DWORD dxgiFactoryFlags = 0;

    // 开启Debug层
    if (useDebugLayers)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugInterface;
        if (SUCCEEDED(D3D12GetDebugInterface(TY_IID_PPV_ARGS(&debugInterface))))
        {
            debugInterface->EnableDebugLayer();

            uint32_t useGPUBasedValidation = 1;
            if (useGPUBasedValidation)
            {
                Microsoft::WRL::ComPtr<ID3D12Debug1> debugInterface1;
                if (SUCCEEDED((debugInterface->QueryInterface(TY_IID_PPV_ARGS(&debugInterface1)))))
                {
                    debugInterface1->SetEnableGPUBasedValidation(true);
                }
            }
        }
        else
        {
            Utility::Print("WARNING:  Unable to enable D3D12 debug validation layer\n");
        }

#if _DEBUG
        ComPtr<IDXGIInfoQueue> dxgiInfoQueue;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgiInfoQueue.GetAddressOf()))))
        {
            dxgiFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);

            DXGI_INFO_QUEUE_MESSAGE_ID hide[] =
            {
                80 /* IDXGISwapChain::GetContainingOutput: The swapchain's adapter does not control the output on which the swapchain's window resides. */,
            };
            DXGI_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = _countof(hide);
            filter.DenyList.pIDList = hide;
            dxgiInfoQueue->AddStorageFilterEntries(DXGI_DEBUG_DXGI, &filter);
        }
#endif
    }

    // Obtain the DXGI factory
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory;
    ASSERT_SUCCEEDED(CreateDXGIFactory2(dxgiFactoryFlags, TY_IID_PPV_ARGS(&dxgiFactory)));

    // Create the D3D graphics device
    Microsoft::WRL::ComPtr<IDXGIAdapter1> pAdapter;

    SIZE_T MaxSize = 0;
    // 遍历所有适配器
    for (uint32_t Idx = 0; DXGI_ERROR_NOT_FOUND != dxgiFactory->EnumAdapters1(Idx, &pAdapter); ++Idx)
    {
        DXGI_ADAPTER_DESC1 desc;
        pAdapter->GetDesc1(&desc);

        // 排除掉微软基本的软件渲染适配器（Microsoft Basic Render Driver）
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        // 尝试创建设备，确保该显卡支持 D3D12 (Feature Level 11.0)
        if (FAILED(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, TY_IID_PPV_ARGS(&pDevice))))
            continue;

        // 在多显卡系统（如笔记本）中，通常 DedicatedVideoMemory 最大的就是独显 (dGPU)
        if (desc.DedicatedVideoMemory > MaxSize)
        {
            MaxSize = desc.DedicatedVideoMemory;

            if (g_Device != nullptr)
                g_Device->Release();

            // 绑定最终选定的硬件设备
            g_Device = pDevice.Detach();

            Utility::Printf(L"Selected GPU: %s (%u MB)\n", desc.Description, desc.DedicatedVideoMemory >> 20);
        }
    }
    // 如果循环结束 g_Device 仍为空，说明这台机器连一张支持 DX12 的物理显卡都没有
    ASSERT(g_Device != nullptr, "No Hardware DX12 Adapter found!");

    // 创建消息队列，屏蔽不重要警告,先搁置不学
#if _DEBUG
    // 获取消息队列接口
    ID3D12InfoQueue* pInfoQueue = nullptr;
    if (SUCCEEDED(g_Device->QueryInterface(TY_IID_PPV_ARGS(&pInfoQueue))))
    {
        // 定义消息严重级别黑名单
        D3D12_MESSAGE_SEVERITY Severities[] =
        {
            D3D12_MESSAGE_SEVERITY_INFO
        };

        // 定义具体错误 ID 黑名单
        D3D12_MESSAGE_ID DenyIds[] =
        {
            // This occurs when there are uninitialized descriptors in a descriptor table, even when a
            // shader does not access the missing descriptors.  I find this is common when switching
            // shader permutations and not wanting to change much code to reorder resources.
            // 当描述符表中有未初始化的槽位时触发
            D3D12_MESSAGE_ID_INVALID_DESCRIPTOR_HANDLE,

            // Triggered when a shader does not export all color components of a render target, such as
            // when only writing RGB to an R10G10B10A2 buffer, ignoring alpha.
            // 像素着色器输出的颜色组件与渲染目标不完全匹配（如只写 RGB 而忽略了 Alpha）。
            D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_PS_OUTPUT_RT_OUTPUT_MISMATCH,

            // This occurs when a descriptor table is unbound even when a shader does not access the missing
            // descriptors.  This is common with a root signature shared between disparate shaders that
            // don't all need the same types of resources.
            D3D12_MESSAGE_ID_COMMAND_LIST_DESCRIPTOR_TABLE_NOT_SET,

            // RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS资源屏障重复转换 (Duplicate Subresource Transitions)
            D3D12_MESSAGE_ID_RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS,

            // Suppress errors from calling ResolveQueryData with timestamps that weren't requested on a given frame.
            // 查询状态无效 (Invalid Query State)
            D3D12_MESSAGE_ID_RESOLVE_QUERY_INVALID_QUERY_STATE,

            // Ignoring InitialState D3D12_RESOURCE_STATE_COPY_DEST. Buffers are effectively created in state D3D12_RESOURCE_STATE_COMMON.
            // 资源创建时的初始状态被忽略（DX12 经常强制将 Buffer 置为 COMMON 状态）。
            D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
        };

        // 组装过滤器 (Filter)
        D3D12_INFO_QUEUE_FILTER NewFilter = {};
        //NewFilter.DenyList.NumCategories = _countof(Categories);
        //NewFilter.DenyList.pCategoryList = Categories;
        NewFilter.DenyList.NumSeverities = _countof(Severities);
        NewFilter.DenyList.pSeverityList = Severities;
        NewFilter.DenyList.NumIDs = _countof(DenyIds);
        NewFilter.DenyList.pIDList = DenyIds;

        // 应用与释放
        pInfoQueue->PushStorageFilter(&NewFilter);
        pInfoQueue->Release();
    }
#endif

    // 特征检查
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
    // CPU挂起，等GPU处理完命令
    g_CommandManager.IdleGPU();

    CommandContext::DestroyAllContexts();
    g_CommandManager.Shutdown();
    // GpuTimeManager::Shutdown();
    PSO::DestroyAll();
    RootSignature::DestroyAll();
    DescriptorAllocator::DestroyAll();

    DestroyCommonState();
    DestroyRenderingBuffers();
    //TemporalEffects::Shutdown();
    //PostEffects::Shutdown();
    //SSAO::Shutdown();
    //TextRenderer::Shutdown();
    //GraphRenderer::Shutdown();
    //ParticleEffectManager::Shutdown();
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
