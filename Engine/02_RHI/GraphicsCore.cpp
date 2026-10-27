#include "00_Core/PCH.h"
#include "02_RHI/GraphicsCore.h"
#include "01_Application/GameCore.h"
#include "04_Renderer/BufferManager.h"
// [非核心渲染功能 - 性能分析] #include "GpuTimeManager.h"
// [非核心渲染功能 - 后处理链] #include "PostEffects.h"
// [非核心渲染功能 - 环境光遮蔽] #include "SSAO.h"
// [非核心渲染功能 - UI与文本] #include "TextRenderer.h"
#include "02_RHI/Resource/ColorBuffer.h"
#include "00_Core/SystemTime.h"
#include "02_RHI/Pipeline/samplerManager.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Command/CommandSignature.h"
// [非核心渲染功能 - 粒子特效] #include "ParticleEffectManager.h"
// [非核心渲染功能 - 性能图表] #include "GraphRenderer.h"
// [非核心渲染功能 - 时间抗锯齿等] #include "TemporalEffects.h"
#include "01_Application/Display.h"

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
	Microsoft::WRL::ComPtr<ID3D12Device5> g_DeviceDXR;
	bool g_bRaytracingSupported = false;
	D3D12_RAYTRACING_TIER g_RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
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

	RaytracingCapability QueryRaytracingCapability()
	{
		RaytracingCapability cap;
		cap.supported = g_bRaytracingSupported && (g_DeviceDXR != nullptr);
		cap.tier = g_RaytracingTier;
		cap.inlineRaytracing = (g_RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
		return cap;
	}
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

	//
	// ===================== 创建 D3D12 Device =====================
	//
	if (SUCCEEDED(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_12_1, TY_IID_PPV_ARGS(&pDevice))))
		g_Device = pDevice.Detach();
	else
	{
		Utility::Printf(L"FAILED to create D3D12 device on this adapter");
		return;
	}

	Utility::Printf(L"Selected GPU:  %s (%u MB)\n", desc.Description, desc.DedicatedVideoMemory >> 20);
	LOG_INFO(L"[Boot] Graphics: device OK, RequireDXRSupport=%d\n", (int)RequireDXRSupport);

	//
	// ===================== DXR Feature Check（R5 / D0） =====================
	//
	// 旧实现：整个查询包在 `if (RequireDXRSupport)` 里，且不支持时直接 return
	// —— 于是「App 没请求」等于「设备能力完全没记录」，而「App 请求了但不支持」
	// 等于「函数中途返回、CommandManager/Display 都没建，后续必然崩」。
	//
	// 新实现分两件事：
	//   1) **无条件**查询并记录真实能力（日志 + g_RaytracingTier）；
	//   2) 只有 App 显式 require 且设备不支持时才中止。
	// 这样 RendererDemo 里可以「有 DXR 就列出 RayTracingRenderer，没有就禁用 UI 项」，
	// 光栅路径与 DXR 支持完全解耦。
	//
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
		const HRESULT hrFeature = g_Device->CheckFeatureSupport(
			D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));

		if (SUCCEEDED(hrFeature))
		{
			g_RaytracingTier = options5.RaytracingTier;
			g_bRaytracingSupported = (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);

			if (g_bRaytracingSupported)
			{
				// DXR Device Interface（CommandList4 / Device5 系列接口都靠它）
				const HRESULT hrQI = g_Device->QueryInterface(TY_IID_PPV_ARGS(&g_DeviceDXR));
				if (FAILED(hrQI))
				{
					// 理论上 Tier>=1.0 一定能拿到 Device5；拿到不了一律降级为不支持，
					// 避免后面拿到空指针再崩。
					g_bRaytracingSupported = false;
					g_DeviceDXR.Reset();
					LOG_INFO(L"[DXR] QueryInterface(ID3D12Device5) 失败 hr=0x%08X，按不支持处理\n", (unsigned)hrQI);
				}
			}

			// 统一的能力日志：无论 App 是否请求，都能在 tooiyume_log.txt 里看到真实 Tier。
			LOG_INFO(L"[DXR] capability: supported=%d tier=%d.%d inline=%d\n",
				(int)g_bRaytracingSupported,
				(int)(options5.RaytracingTier >> 4),
				(int)(options5.RaytracingTier & 0xF),
				(int)(options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1));
		}
		else
		{
			g_bRaytracingSupported = false;
			g_RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
			LOG_INFO(L"[DXR] CheckFeatureSupport(OPTIONS5) 失败 hr=0x%08X，DXR 不可用\n", (unsigned)hrFeature);
		}

		// App 显式要求 DXR 但设备给不了 —— 这时才中止（保持原有语义）。
		if (RequireDXRSupport && !g_bRaytracingSupported)
		{
			LOG_INFO(L"[DXR] 本程序要求 DXR 支持，但当前设备不满足，启动中止。\n");
			return;
		}
	}


	LOG_TRACE(L"[Boot] Graphics: after feature query\n");

	//
	// ===================== InfoQueue（精简版） =====================
	//
#if _DEBUG
	{
		Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(g_Device->QueryInterface(TY_IID_PPV_ARGS(&infoQueue))))
		{
			// R0：WARNING 级别默认不中断。
			// 原因：Debug Layer 会对大量「不影响结果但会刷屏」的情况报 WARNING
			// （如优化提示、资源重叠），一旦设为 break 会让程序在合法路径上被调试器打断，
			// 反而掩盖真正的 ERROR / CORRUPTION。
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
			LOG_INFO(L"[Boot] Graphics: InfoQueue break on ERROR/CORRUPTION only\n");

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
	LOG_TRACE(L"[Boot] Graphics: CommandManager.Create\n");
	g_CommandManager.Create(g_Device);

	// 初始化全局渲染状态
	LOG_TRACE(L"[Boot] Graphics: InitializeCommonState\n");
	InitializeCommonState();
	// 初始化显示模块 （交换链，后台缓冲区及描述符)
	LOG_TRACE(L"[Boot] Graphics: Display::Initialize\n");
	Display::Initialize();
	LOG_TRACE(L"[Boot] Graphics: Display::Initialize done\n");
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

	DestroyCommonState();
	// R2：顺序修正 —— BufferManager 的 DestroyAll 会通过
	// ColorBuffer::Destroy 归还 RTV/SRV 描述符，这要求堆池仍然存在。
	// 原顺序把 DescriptorAllocator::DestroyAll() 放在前面，堆池已被清空，
	// 之后的 Free 定位不到任何堆（ASSERT 在 Release 下被忽略，
	// 直接静默失败）。必须先释放资源，最后拆分配器。
	BufferManager::DestroyAll();
	Display::Shutdown();
	DescriptorAllocator::DestroyAll();


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