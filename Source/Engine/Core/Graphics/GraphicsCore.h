#pragma once

// 没啃






#include "../Graphics/PipelineState/GraphicsCommon.h"
#include "../Graphics/DescriptorAllocators/DescriptorHeap.h"

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

		extern ID3D12Device* g_Device;
		extern CommandListManager g_CommandManager;
		extern ContextManager g_ContextManager;

		extern D3D_FEATURE_LEVEL g_D3DFeatureLevel;
		extern bool g_bTypedUAVLoadSupport_R11G11B10_FLOAT;
		extern bool g_bTypedUAVLoadSupport_R16G16B16A16_FLOAT;

		 extern DescriptorAllocator g_DescriptorAllocator[];
		 inline D3D12_CPU_DESCRIPTOR_HANDLE AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE Type, UINT Count = 1)
		{
			return g_DescriptorAllocator[Type].Allocate(Count);
		}
}



