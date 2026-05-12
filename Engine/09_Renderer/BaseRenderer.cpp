#include "BaseRenderer.h"

namespace Renderer
{
	// 根绑定槽位枚举
	enum RootBindings
	{
		kMeshConstants,     // 常量缓冲: 模型矩阵 (b0) D3D12_SHADER_VISIBILITY_VERTEX     
		kMaterialConstants, // 常量缓冲: 材质参数 (b1) D3D12_SHADER_VISIBILITY_PIXEL      
		kMaterialSRVs,      // 描述符表: 材质纹理贴图 (t0-t9)
		kMaterialSamplers,  // 描述符表: 采样器 (s0-s9)
		kCommonSRVs,        // 描述符表: 全局通用贴图，如 IBL 贴图、阴影图 (t10+)
		kCommonCBV,         // 常量缓冲: 全局常量，如相机矩阵 (b2)

		kNumRootBindings    // 根参数总数，用于初始化 Root Signature
	};
} // namespace Renderer
