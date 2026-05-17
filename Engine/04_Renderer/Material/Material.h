#pragma once

// “资源”（model）和“渲染策略”（pso，rootsig）解耦
// 资源导入层负责保存几何信息，材质层负责保存材质参数和纹理绑定，渲染策略层在运行时根据前两者的状态决定使用哪个 PSO 来渲染



#include <cstdint>
#include <d3d12.h>

namespace Renderer
{
	// 仅描述材质的逻辑/物理属性
	enum MaterialFlags : uint32_t
	{
		kMaterial_None = 0,
		kMaterial_AlphaBlend = 1 << 0,
		kMaterial_AlphaTest = 1 << 1,
		kMaterial_DoubleSided = 1 << 2,
	};

	// 材质实例层：保存参数、纹理绑定，并提供状态查询
	struct Material
	{
		uint32_t Flags = 0;   // blend状态

		uint32_t SRVTable = 0; // 纹理SRV
		uint32_t SamplerTable = 0; // 采样器

		D3D12_GPU_VIRTUAL_ADDRESS MaterialCBV = 0; // PBR材质数据

		// 供渲染策略层在分类 Mesh 时调用
		inline bool IsAlphaBlend() const { return (Flags & Renderer::kMaterial_AlphaBlend) != 0; }
		inline bool IsAlphaTest() const { return (Flags & Renderer::kMaterial_AlphaTest) != 0; }
		inline bool IsDoubleSided() const { return (Flags & Renderer::kMaterial_DoubleSided) != 0; }
	};

	// Unaligned mirror of MaterialConstants
	struct MaterialConstantData
	{
		float baseColorFactor[4]; // default=[1,1,1,1]
		float emissiveFactor[3]; // default=[0,0,0]
		float normalTextureScale; // default=1
		float metallicFactor; // default=1
		float roughnessFactor; // default=1
		uint32_t flags;
	};
} // Renderer
