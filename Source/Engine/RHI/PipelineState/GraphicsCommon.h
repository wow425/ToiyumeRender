#pragma once


// 全局渲染状态预设


#include "SamplerManager.h"

class SamplerDesc;
class CommandSignature;
class RootSignature;
class ComputePSO;
class GraphicsPSO;


namespace Graphics
{
    void InitializeCommonState(void);
    void DestroyCommonState(void);

    extern SamplerDesc SamplerLinearWrapDesc;
    extern SamplerDesc SamplerLinearClampDesc;
    extern SamplerDesc SamplerPointClampDesc;


    extern D3D12_CPU_DESCRIPTOR_HANDLE SamplerLinearWrap;
    extern D3D12_CPU_DESCRIPTOR_HANDLE SamplerLinearClamp;
    extern D3D12_CPU_DESCRIPTOR_HANDLE SamplerPointClamp;


    extern D3D12_RASTERIZER_DESC RasterizerDefault;

    extern D3D12_BLEND_DESC BlendNoColorWrite;		// XXX
    extern D3D12_BLEND_DESC BlendDisable;			// 1, 0
    extern D3D12_BLEND_DESC BlendPreMultiplied;		// 1, 1-SrcA
    extern D3D12_BLEND_DESC BlendTraditional;		// SrcA, 1-SrcA
    extern D3D12_BLEND_DESC BlendAdditive;			// 1, 1
    extern D3D12_BLEND_DESC BlendTraditionalAdditive;// SrcA, 1

    extern D3D12_DEPTH_STENCIL_DESC DepthStateDisabled;
    extern D3D12_DEPTH_STENCIL_DESC DepthStateReadWrite;
    extern D3D12_DEPTH_STENCIL_DESC DepthStateReadOnly;
    extern D3D12_DEPTH_STENCIL_DESC DepthStateReadOnlyReversed;
    extern D3D12_DEPTH_STENCIL_DESC DepthStateTestEqual;


    enum eDefaultTexture
    {
        kMagenta2D,  // Useful for indicating missing textures错误提示(1, 0, 1, 1)
        kBlackOpaque2D, // 零贡献填充。用于叠加型属性（如 自发光 (Emissive)），确保不干扰最终颜色。(0, 0, 0, 1)
        kBlackTransparent2D, // 完全透明/遮罩。用于处理带有 Alpha 混合 (Alpha Blending) 的槽位。(0, 0, 0, 0)
        kWhiteOpaque2D, // 中性填充。用于乘法型属性（如 基础色 (Base Color)），乘以 1 后保持原色。(1, 1, 1, 1)
        kWhiteTransparent2D, // 较少见，通常用于特定的遮罩逻辑。(1, 1, 1, 0)
        kDefaultNormalMap, // 平滑表面。表示切线空间的法线朝向正上方 (0, 0, 1). (0.5, 0.5, 1, 1)
        kBlackCubeMap, // 环境反射占位。如果没有反射探针，就给一个纯黑的立方体贴图。(0, 0, 0, 1)

        kNumDefaultTextures
    };
    D3D12_CPU_DESCRIPTOR_HANDLE GetDefaultTexture(eDefaultTexture texID);

    extern RootSignature g_CommonRS;
}
