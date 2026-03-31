#pragma once

#pragma warning(disable:4201) // 准许无名结构体/联合体 (Nameless struct/union)
#pragma warning(disable:4238) // 准许右值当左值用
#pragma warning(disable:4239) // 准许把一个非 const 引用绑定到了一个临时对象上。
#pragma warning(disable:4324) // 命令结构体因对齐而填充 (Structure was padded)

#include <winsdkver.h> // 引入SDK版本检查
#define _WIN32_WINNT 0x0A00 // 明确指定 Windows 10 SDK 版本 (0x0A00 = Windows 10.0//11)
#include <sdkddkver.h> // 根据目标版本自动配置 Windows SDK 版本

#define NOMINMAX // 空宏定义，布尔开关，禁止 Windows 头文件定义 min 和 max 宏

// 排除上古windows组件
#define NODRAWTEXT 
#define NOGDI
#define NOBITMAP
#define NOMCX 
#define NOSERVICE
#define NOHELP
#define WIN32_LEAN_AND_MEAN
// 排除上古windows组件

#include <Windows.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <d3d12.h>
#include <dxgi1_6.h> // 1.6dxgi
#include "d3dx12.h"
#ifdef _DEBUG
	#include <dxgidebug.h>
#endif

#define D3D12_GPU_VIRTUAL_ADDRESS_NULL ((D3D12_GPU_VIRTUAL_ADDRESS)0) // D3D12_GPU_VIRTUAL_ADDRESS_NULL定义为0
#define D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN ((D3D12_GPU_VIRTUAL_ADDRESS)-1) // D3D12_GPU_VIRTUAL_ADDRESS_NOKNOWN定义为-1
#define TY_IID_PPV_ARGS IID_PPV_ARGS

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <memory>
#include <string>
#include <cwctype>
#include <exception>

#include <ppltasks.h> // 并行任务库 (Parallel Patterns Library)，提供并行算法和异步任务支持
#include <functional> // 常用于命令录制的回调或者事件系统

#include "Utility.h"