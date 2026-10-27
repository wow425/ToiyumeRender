#pragma once


#include "02_RHI/Pipeline/PipelineState.h"
#include "02_RHI/Descriptor/DescriptorHeap.h"
#include "02_RHI/Pipeline/RootSignature.h"
#include "02_RHI/Pipeline/GraphicsCommon.h"

class CommandListManager;
class ContextManager;

namespace Graphics
{
#ifndef  RELEASE
    extern const GUID WKPDID_D3DDebugObjectName; // 资源取名便于调试
#endif 
    // g_全局变量
    using namespace Microsoft::WRL;

    void Initialize(bool RequireDXRsupport = false);
    void Shutdown(void);

    // GPU检测
    bool IsDeviceNvidia(ID3D12Device* pDevice);

    extern ID3D12Device* g_Device; // 设备
    extern Microsoft::WRL::ComPtr<IDXGIFactory6> g_DXGIFactory; // 工厂
    extern CommandListManager g_CommandManager; // 命令管理器
    extern ContextManager g_ContextManager;    // 上下文管理器

    extern bool g_bTypedUAVLoadSupport_R11G11B10_FLOAT;
    extern bool g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT;

    // ============================================================
    // R5 / D0：DXR 设备能力（显式导出，供 Renderer 层查询）
    // ------------------------------------------------------------
    // 语义拆分（这是 R5 修的核心缺陷之一）：
    //   g_bRaytracingSupported —— **设备是否真的支持** DXR。
    //       每次启动都无条件查询一次，与 App 是否请求无关。
    //   Initialize(RequireDXRsupport) 的参数 —— **App 是否强制要求** DXR。
    //       只有它为 true 且设备不支持时才中止启动。
    // 旧实现把两者绑在一起：RequireDXRSupport==false 时根本不查询，
    // 于是 RayTracingApp 的请求（因签名不匹配被基类吞掉）不仅没生效，
    // 连日志里都看不到设备真实能力，属于「静默失败」。
    // ============================================================
    extern Microsoft::WRL::ComPtr<ID3D12Device5> g_DeviceDXR;   // DXR 用 device（Tier >= 1.0 时有效）
    extern bool g_bRaytracingSupported;                          // 设备是否支持 DXR
    extern D3D12_RAYTRACING_TIER g_RaytracingTier;               // 实际查询到的 Tier（不支持时为 NONE）

    // 供 Renderer / UI 层使用的能力快照（不依赖 g_DeviceDXR 是否为空）。
    struct RaytracingCapability
    {
        bool supported = false;
        D3D12_RAYTRACING_TIER tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        bool inlineRaytracing = false;   // Tier 1.1 的 Inline Raytracing（本阶段不使用，仅上报）
    };
    RaytracingCapability QueryRaytracingCapability();

    extern DescriptorAllocator g_DescriptorAllocator[];
    inline D3D12_CPU_DESCRIPTOR_HANDLE AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE Type, UINT Count = 1)
    {
        return g_DescriptorAllocator[Type].Allocate(Count);
    }
}



