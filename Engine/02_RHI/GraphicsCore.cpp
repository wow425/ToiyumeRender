#include "00_Core/PCH.h"
#include "02_RHI/GraphicsCore.h"
#include "02_RHI/GraphicsCaps.h"
// [TD-01a / 单向 RHI 架构红线] GraphicsCore 属于 Engine/02_RHI，严禁反向 include
// 01_Application/GameCore.h、01_Application/Display.h 或 04_Renderer/BufferManager.h！
// Display 与 BufferManager 的启停已统归 01_Application/GameCore.cpp 编排。
#include "02_RHI/Resource/ColorBuffer.h"
#include "00_Core/SystemTime.h"
#include "02_RHI/Pipeline/samplerManager.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Command/CommandListManager.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Command/CommandSignature.h"

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
	DeferredReleaseQueue g_DeferredReleaseQueue;

	DeferredReleaseQueue& GetDeferredReleaseQueue()
	{
		return g_DeferredReleaseQueue;
	}



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
		if (IsSimulatedNoDxrForTest())
		{
			cap.supported = false;
			cap.tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
			cap.inlineRaytracing = false;
			return cap;
		}
		cap.supported = g_bRaytracingSupported && (g_DeviceDXR != nullptr);
		cap.tier = g_RaytracingTier;
		cap.inlineRaytracing = (g_RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
		return cap;
	}
}

// 初始化DX运行所需的资源Initialize the DirectX resources required to run.
// [TD-07a] 返回 bool：关键步骤失败时立即写 LOG_ERROR 并返回 false，禁止半初始化态继续执行。
bool Graphics::Initialize(bool RequireDXRSupport)
{
	if (g_Device != nullptr)
		return true;

	// [TD-08b] 每次初始化前显式复位全局能力标志，确保同进程多轮 Init/Shutdown 无残留状态。
	g_bTypedUAVLoadSupport_R11G11B10_FLOAT = false;
	g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT = false;
	g_bRaytracingSupported = false;
	g_RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
	g_DeviceDXR.Reset();
	g_DXGIFactory.Reset();

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
			// ============================================================
			//  GPU Validation（GBV）—— 默认关闭，按需开启
			// ------------------------------------------------------------
			// 为什么默认关闭：
			//   Agility SDK 620 的 d3d12SDKLayers.dll 在「GBV 开 + DXR 全屏
			//   DispatchRays」这条组合下会让 GPU 侧执行时间突破 TDR 上限，
			//   触发 DXGI_ERROR_DEVICE_HUNG：
			//     D3D12 ERROR: ID3D12Device::RemoveDevice ... DEVICE_HUNG
			//   实测（RTX 4060 Ti / Win10 19045）：同一份 Debug 构建、
			//   同一条 -StartRenderer RayTracingRenderer 命令，
			//     · GBV=on  → 首帧 DispatchRays 提交后挂死，0 张截图，exit=2170
			//     · GBV=off → 5 张 1920x1080 截图正常导出，画面与基线一致
			//   而 Deferred/Forward 路径在 GBV=on 时无问题 —— 这是 GBV 对
			//   RT 管线（BVH 遍历 + 内联光线）的额外校验开销，属工具层行为，
			//   不是本工程的资源状态错误。
			//
			// 为什么不彻底删掉：
			//   L1–L5 验收的 L5 核心是 InfoQueue 的 ERROR/CORRUPTION 检查，
			//   它由上面的 EnableDebugLayer() 提供，与 GBV 无关。
			//   但 GBV 能抓到「越界访问 / 未初始化资源 / 描述符非法」这类
			//   InfoQueue 看不见的问题，排查 RHI 缺陷时非常有用，所以保留
			//   为一个显式开关而不是删掉。
			//
			// 怎么开：
			//   设置环境变量 TY_ENABLE_GBV=1 再启动。
			//   排查 GBV 时才开；开之前请避开 DXR 管线，否则会 TDR。
			//
			// DX12 不变量：
			//   SetEnableGPUBasedValidation 必须在创建 Device **之前**调用，
			//   且在进程内只生效一次；这里的位置（D3D12CreateDevice 之前）
			//   就是这个约束决定的，不要挪到后面。
			// ============================================================
			Microsoft::WRL::ComPtr<ID3D12Debug1> debugInterface1;
			if (SUCCEEDED(debugInterface->QueryInterface(TY_IID_PPV_ARGS(&debugInterface1))))
			{
				wchar_t gbvEnv[8] = {};
				const DWORD gbvLen = ::GetEnvironmentVariableW(L"TY_ENABLE_GBV", gbvEnv, 8);
				const bool enableGBV = (gbvLen > 0 && gbvEnv[0] != L'0');

				debugInterface1->SetEnableGPUBasedValidation(enableGBV);

				if (enableGBV)
				{
					LOG_INFO(L"[Boot] Graphics: GPU-based validation ENABLED (TY_ENABLE_GBV=1)。"
						L"注意：GBV + DXR 全屏派发会触发 TDR，排查 RT 时请勿开启。\n");
				}
				else
				{
					LOG_INFO(L"[Boot] Graphics: GPU-based validation off (默认；TY_ENABLE_GBV=1 开启)。"
						L"InfoQueue 的 ERROR/CORRUPTION 检查仍然生效。\n");
				}
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
	const HRESULT hrFactory = CreateDXGIFactory2(dxgiFactoryFlags, TY_IID_PPV_ARGS(&g_DXGIFactory));
	if (FAILED(hrFactory))
	{
		LOG_ERROR(L"[Boot] Graphics: CreateDXGIFactory2 failed hr=0x%08X\n", (unsigned)hrFactory);
		return false;
	}

	//
	// ===================== 获取唯一高性能GPU =====================
	//
	Microsoft::WRL::ComPtr<IDXGIAdapter1> pAdapter;

	uint32_t desiredVendor = GetDesiredGPUVendor();
	if (desiredVendor)
	{
		Utility::Printf(L"Looking for a %s GPU\n", GPUVendorToString(desiredVendor));
	}
	const HRESULT hrAdapter = g_DXGIFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, TY_IID_PPV_ARGS(&pAdapter));
	if (FAILED(hrAdapter) || pAdapter == nullptr)
	{
		LOG_ERROR(L"[Boot] Graphics: EnumAdapterByGpuPreference failed hr=0x%08X\n", (unsigned)hrAdapter);
		g_DXGIFactory.Reset();
		return false;
	}

	DXGI_ADAPTER_DESC1 desc = {};
	pAdapter->GetDesc1(&desc);

	//
	// ===================== 创建 D3D12 Device =====================
	//
	const HRESULT hrDevice = D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_12_1, TY_IID_PPV_ARGS(&pDevice));
	if (SUCCEEDED(hrDevice))
		g_Device = pDevice.Detach();
	else
	{
		LOG_ERROR(L"[Boot] Graphics: D3D12CreateDevice failed hr=0x%08X\n", (unsigned)hrDevice);
		g_DXGIFactory.Reset();
		return false;
	}

	Utility::Printf(L"Selected GPU:  %s (%u MB)\n", desc.Description, desc.DedicatedVideoMemory >> 20);
	LOG_INFO(L"[Boot] Graphics: device OK, RequireDXRSupport=%d\n", (int)RequireDXRSupport);

	//
	// ===================== Agility SDK 运行时自检 =====================
	//
	// 【为什么必须在这里打日志，而不是靠"构建成功"就算数】
	//
	// Agility SDK 的接入是**运行期生效**的，编译期完全看不出来：
	//   - 头文件换成了新的（D3D12_SDK_VERSION=620），只能说明"能写新代码"；
	//   - 真正的核心版本取决于 exe 旁边的 D3D12\D3D12Core.dll 是否被加载，
	//     而这一步由 AgilitySdkOptIn.cpp 导出的 D3D12SDKVersion/D3D12SDKPath
	//     两个符号驱动。符号写错名字、路径写错、DLL 没部署 —— 以上任何一种，
	//     程序都**照常启动、照常渲染、不报任何错**，只是悄悄回退到系统旧运行时
	//     （本机 Win10 19045 自带 d3d12.dll 10.0.19041.5794，只支持到 SM 6.5）。
	// 静默降级的后果极隐蔽：WP-3A 要建 SM 6.6 的 PSO，
	// CreatePipelineState 在 Debug Layer 下才会报 "shader model not supported"，
	// 而 Release 下可能只是渲染结果不对。所以必须有一行运行期证据。
	//
	// 【DX12 不变量】
	//   CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL) **总是返回 S_OK**，
	//   真正的答案在出参 HighestShaderModel 里。
	//   只判 HRESULT 会把"只支持 6.5"误判成"支持 6.6"，这是本项目 2026-11-21
	//   实测踩到的坑（见 docs/AI/worklogs/TR-20261121-01_wp-0a-baseline.md §2.2）。
	//
	// 【ResourceBindingTier == 3 的意义】
	//   Tier 3 = 完整 bindless（ResourceDescriptorHeap[]/SamplerDescriptorHeap[]
	//   可在任意 shader 里动态索引）。WP-3A 门禁 2 的 SM 6.6 Bindless 全局根签名
	//   要求 SM 6.6 **且** Tier 3 同时成立；缺一项就只能退回传统根签名路径。
	//
	{
		// ---- 1) 编译期头版本（证明 include 路径已指向 Agility 版本） ----
		LOG_INFO(L"[Agility] 编译期 D3D12_SDK_VERSION=%u（期望 %u）\n",
			(unsigned)D3D12_SDK_VERSION,
			(unsigned)TOOIYUME_EXPECTED_AGILITY_SDK_VERSION);

		// ---- 2) 运行期实际加载的核心版本（证明 opt-in 真的生效） ----
		// 只在 Windows 下取模块版本：这里用 GetModuleHandle 查 D3D12Core.dll，
		// 拿到说明 Agility 运行时已被进程加载；拿不到 = 回退到系统 d3d12.dll。
		// 不引入 version.dll 的完整版本查询（那是 Windows 专属且此处只需"有没有"），
		// 够用即可，真正的版本一致性由 CMake 从同一个 d3d12.h 解析并写进编译宏保证。
		if (HMODULE agilityCore = ::GetModuleHandleW(L"D3D12Core.dll"))
		{
			wchar_t corePath[MAX_PATH] = {};
			::GetModuleFileNameW(agilityCore, corePath, MAX_PATH);
			LOG_INFO(L"[Agility] 运行时已加载 Agility D3D12Core.dll: %s\n", corePath);
		}
		else
		{
			// 这行如果出现，说明 vcpkg 的 directx12-agility 提供的
			// D3D12Core.dll（vcpkg_installed/x64-windows/bin/）没有被部署到
			// <exe>/D3D12/，或 Engine/20_cmake/AgilitySdkOptIn.cpp 未编进本 exe。
			// 此时 SM 6.6 一定不可用，WP-3A 必须走传统根签名回退路径。
			LOG_INFO(L"[Agility] 警告：未检测到 Agility D3D12Core.dll，"
				L"运行时为系统版本（SM 6.6 不可用）\n");
		}

		// ---- 3) 真实的 Shader Model 上限 ----
		D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
		shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_6;
		const HRESULT hrSM = g_Device->CheckFeatureSupport(
			D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));

		// ---- 4) 绑定模型层级 ----
		D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
		const HRESULT hrOpt = g_Device->CheckFeatureSupport(
			D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));

		const bool sm66 = SUCCEEDED(hrSM)
			&& shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
		const bool tier3 = SUCCEEDED(hrOpt)
			&& options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3;

		// 高位/低位分别打印，读日志时不用心算 0x66 是哪个版本。
		LOG_INFO(L"[Agility] HighestShaderModel=0x%X (%u.%u)  SM6.6=%d  "
			L"ResourceBindingTier=%u  Tier3=%d  SM6.6+Tier3=%d\n",
			(unsigned)shaderModel.HighestShaderModel,
			(unsigned)(shaderModel.HighestShaderModel >> 4),
			(unsigned)(shaderModel.HighestShaderModel & 0xF),
			(int)sm66,
			SUCCEEDED(hrOpt) ? (unsigned)options.ResourceBindingTier : 0u,
			(int)tier3,
			(int)(sm66 && tier3));

		// 【WP-3A 门禁 2 的判据落点】
		// 这里不 return、不崩 —— 因为光栅路径在没有 SM 6.6 时仍应能跑。
		// 决策交给上层（Renderer 选择 bindless 还是传统根签名），
		// 本函数只负责把真相记录到日志里，供 L4/L5 验收取证。
	}

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

		// App 显式要求 DXR 但设备给不了 —— 清理已创建的半成品设备并返回 false。
		if (RequireDXRSupport && !g_bRaytracingSupported)
		{
			LOG_ERROR(L"[DXR] 本程序要求 DXR 支持，但当前设备不满足，启动中止。\n");
			if (g_Device != nullptr)
			{
				g_Device->Release();
				g_Device = nullptr;
			}
			g_DXGIFactory.Reset();
			return false;
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

	// [WP-3A / 契约 2] 一次性探测完整只读 GpuCaps 快照（含 SM 6.6/Tier3、WaveOps、FP16 与本地显存预算接口）
	ProbeGpuCaps(g_Device, pAdapter.Get(),
		g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT,
		g_bTypedUAVLoadSupport_R11G11B10_FLOAT);

	// 创建命令管理器
	LOG_TRACE(L"[Boot] Graphics: CommandManager.Create\n");
	g_CommandManager.Create(g_Device);

	// 初始化全局渲染状态
	LOG_TRACE(L"[Boot] Graphics: InitializeCommonState\n");
	InitializeCommonState();

	// [TD-01a] Display::Initialize() 已移出 02_RHI，交由上层 GameCore::InitializeApplication 调用，
	// 彻底切断 RHI 对 Application/Display 的反向依赖。
	return true;
}

void Graphics::Shutdown(void)
{
	if (g_Device == nullptr)
		return;

	// 1) CPU 挂起，等待所有已就绪队列执行完毕（TD-07a：IdleGPU 内部判 IsReady()）
	g_CommandManager.IdleGPU();

	// 2) GPU 已完全空闲，安全排空帧尾延迟释放队列
	g_DeferredReleaseQueue.DrainAll();

	// 3) 依次释放命令上下文/线性页/动态堆、命令管理器、PSO/RS/公共状态
	CommandContext::DestroyAllContexts();
	g_CommandManager.Shutdown();
	PSO::DestroyAll();
	RootSignature::DestroyAll();
	DestroyCommonState();

	// 4) [TD-08b] 必须在全部资源释放之后再复位并清空 CPU 描述符分配器池
	for (uint32_t i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i)
		g_DescriptorAllocator[i].Reset();
	DescriptorAllocator::DestroyAll();

	// 5) 释放辅助接口（DXR Device5 与 GraphicsCaps 持有的 IDXGIAdapter3 必须在 ReportLiveDeviceObjects 前释放）
	ShutdownGpuCaps();
	g_DeviceDXR.Reset();
	g_DXGIFactory.Reset();
	g_bRaytracingSupported = false;
	g_RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
	g_bTypedUAVLoadSupport_R11G11B10_FLOAT = false;
	g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT = false;

	// 6) [WP-1A / L5 显存泄漏检测] 启用标准 _DEBUG 下的 ReportLiveDeviceObjects（移除死宏 _GAMING_DESKTOP）
#if defined(_DEBUG)
	{
		Microsoft::WRL::ComPtr<ID3D12DebugDevice> debugInterface;
		if (SUCCEEDED(g_Device->QueryInterface(TY_IID_PPV_ARGS(&debugInterface))))
		{
			debugInterface->ReportLiveDeviceObjects(D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
		}
	}
#endif

	const ULONG remainingRefs = g_Device->Release();
	g_Device = nullptr;
	LOG_INFO(L"[RHI] Graphics::Shutdown complete (final Device::Release refcount=%lu)\n", remainingRefs);
}