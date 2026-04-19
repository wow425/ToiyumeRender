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
    extern D3D12_RASTERIZER_DESC RasterizerTwoSided;

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
        kMagenta2D,  // Useful for indicating missing textures 测试用
        kBlackOpaque2D, // 自发光Emission
        kWhiteOpaque2D, // 漫反射
        kDefaultNormalMap, // 法线

        kNumDefaultTextures
    };
    D3D12_CPU_DESCRIPTOR_HANDLE GetDefaultTexture(eDefaultTexture texID);

    extern RootSignature g_CommonRS;
}
