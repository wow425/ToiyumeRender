#include "PCH.h"
#include "GraphicsCommon.h"
#include "SamplerManager.h"
#include "../RHI/Command/CommandSignature.h"
#include "../Resource/Texture/Texture.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "../Resource/ResourceManager/BufferManager.h"

//#include "CompiledShaders/GenerateMipsLinearCS.h"
//#include "CompiledShaders/GenerateMipsLinearOddCS.h"
//#include "CompiledShaders/GenerateMipsLinearOddXCS.h"
//#include "CompiledShaders/GenerateMipsLinearOddYCS.h"
//#include "CompiledShaders/GenerateMipsGammaCS.h"
//#include "CompiledShaders/GenerateMipsGammaOddCS.h"
//#include "CompiledShaders/GenerateMipsGammaOddXCS.h"
//#include "CompiledShaders/GenerateMipsGammaOddYCS.h"
//
//#include "CompiledShaders/ScreenQuadCommonVS.h"
//#include "CompiledShaders/DownsampleDepthPS.h"

namespace Graphics
{
    SamplerDesc SamplerLinearWrapDesc;
    // [非必要功能 - 高级过滤] SamplerDesc SamplerAnisoWrapDesc;
    // [非必要功能 - 阴影计算] SamplerDesc SamplerShadowDesc;
    SamplerDesc SamplerLinearClampDesc;
    // [非必要功能 - 体积云/雾] SamplerDesc SamplerVolumeWrapDesc;
    SamplerDesc SamplerPointClampDesc;
    // [非必要功能 - 边界采样] SamplerDesc SamplerPointBorderDesc;
    // [非必要功能 - 边界采样] SamplerDesc SamplerLinearBorderDesc;

    D3D12_CPU_DESCRIPTOR_HANDLE SamplerLinearWrap;
    // D3D12_CPU_DESCRIPTOR_HANDLE SamplerAnisoWrap;
    // D3D12_CPU_DESCRIPTOR_HANDLE SamplerShadow;
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerLinearClamp;
    // D3D12_CPU_DESCRIPTOR_HANDLE SamplerVolumeWrap;
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerPointClamp;
    // D3D12_CPU_DESCRIPTOR_HANDLE SamplerPointBorder;
    // D3D12_CPU_DESCRIPTOR_HANDLE SamplerLinearBorder;

    Texture DefaultTextures[kNumDefaultTextures];
    D3D12_CPU_DESCRIPTOR_HANDLE GetDefaultTexture(eDefaultTexture texID)
    {
        ASSERT(texID < kNumDefaultTextures);
        return DefaultTextures[texID].GetSRV();
    }

    D3D12_RASTERIZER_DESC RasterizerDefault;    // Counter-clockwise (基础实心光栅化)
    // [非必要功能 - 抗锯齿] D3D12_RASTERIZER_DESC RasterizerDefaultMsaa;
    // [非必要功能 - 顺时针剔除] D3D12_RASTERIZER_DESC RasterizerDefaultCw; 
    // D3D12_RASTERIZER_DESC RasterizerDefaultCwMsaa;
    // [非必要功能 - 双面渲染(如草地)] D3D12_RASTERIZER_DESC RasterizerTwoSided;
    // D3D12_RASTERIZER_DESC RasterizerTwoSidedMsaa;
    // [非必要功能 - 阴影深度偏移] D3D12_RASTERIZER_DESC RasterizerShadow;
    // D3D12_RASTERIZER_DESC RasterizerShadowCW;
    // D3D12_RASTERIZER_DESC RasterizerShadowTwoSided;

    // [非必要功能 - 仅写入深度不写颜色] D3D12_BLEND_DESC BlendNoColorWrite;
    D3D12_BLEND_DESC BlendDisable; // (基础不透明物体渲染)
    // [非必要功能 - 半透明混合] D3D12_BLEND_DESC BlendPreMultiplied;
    // D3D12_BLEND_DESC BlendTraditional;
    // D3D12_BLEND_DESC BlendAdditive;
    // D3D12_BLEND_DESC BlendTraditionalAdditive;

    D3D12_DEPTH_STENCIL_DESC DepthStateDisabled;
    D3D12_DEPTH_STENCIL_DESC DepthStateReadWrite; // (基础深度测试与写入)
    // [非必要功能 - 仅测试不写入(半透明)] D3D12_DEPTH_STENCIL_DESC DepthStateReadOnly;
    // [非必要功能 - 反向Z] D3D12_DEPTH_STENCIL_DESC DepthStateReadOnlyReversed;
    // [非必要功能 - 相等测试(如贴花)] D3D12_DEPTH_STENCIL_DESC DepthStateTestEqual;

    // [非必要功能 - GPU驱动渲染(Compute Shader生成绘制参数)]
    // CommandSignature DispatchIndirectCommandSignature(1);
    // CommandSignature DrawIndirectCommandSignature(1);

    RootSignature g_CommonRS;
}

// 初始化全局渲染状态
void Graphics::InitializeCommonState(void)
{
    // --- 1. 基础采样器 ---
    SamplerLinearWrapDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerLinearWrap = SamplerLinearWrapDesc.CreateDescriptor();

    SamplerLinearClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerLinearClampDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    SamplerLinearClamp = SamplerLinearClampDesc.CreateDescriptor();

    SamplerPointClampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    SamplerPointClampDesc.SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    SamplerPointClamp = SamplerPointClampDesc.CreateDescriptor();

    // --- 2. 基础占位纹理 ---
    uint32_t MagentaPixel = 0xFFFF00FF;
    DefaultTextures[kMagenta2D].Create2D(4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &MagentaPixel);
    uint32_t WhiteOpaqueTexel = 0xFFFFFFFF;
    DefaultTextures[kWhiteOpaque2D].Create2D(4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &WhiteOpaqueTexel);

    // [非必要纹理注释] kBlackOpaque2D, kBlackTransparent2D, kDefaultNormalMap, kBlackCubeMap...

    // --- 3. 基础光栅化状态 ---
    RasterizerDefault.FillMode = D3D12_FILL_MODE_SOLID;
    RasterizerDefault.CullMode = D3D12_CULL_MODE_BACK;
    RasterizerDefault.FrontCounterClockwise = TRUE;
    RasterizerDefault.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    RasterizerDefault.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    RasterizerDefault.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    RasterizerDefault.DepthClipEnable = TRUE;
    RasterizerDefault.MultisampleEnable = FALSE;
    RasterizerDefault.AntialiasedLineEnable = FALSE;
    RasterizerDefault.ForcedSampleCount = 0;
    RasterizerDefault.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // --- 4. 基础深度状态 ---
    DepthStateDisabled.DepthEnable = FALSE;
    DepthStateDisabled.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DepthStateDisabled.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    DepthStateDisabled.StencilEnable = FALSE;
    DepthStateDisabled.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    DepthStateDisabled.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    DepthStateDisabled.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    DepthStateDisabled.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    DepthStateDisabled.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    DepthStateDisabled.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    DepthStateDisabled.BackFace = DepthStateDisabled.FrontFace;

    DepthStateReadWrite = DepthStateDisabled;
    DepthStateReadWrite.DepthEnable = TRUE;
    DepthStateReadWrite.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    DepthStateReadWrite.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // 注意：配合反向Z使用

    // --- 5. 基础混合状态 ---
    D3D12_BLEND_DESC alphaBlend = {};
    alphaBlend.IndependentBlendEnable = FALSE;
    alphaBlend.RenderTarget[0].BlendEnable = FALSE;
    alphaBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    alphaBlend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    alphaBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    alphaBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    alphaBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    alphaBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    alphaBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    BlendDisable = alphaBlend; // 禁用混合，直接覆盖写入颜色

    // [非必要功能 - 间接绘制参数初始化]
    // DispatchIndirectCommandSignature[0].Dispatch(); ...

    // --- 6. 全局根签名 ---
    g_CommonRS.Reset(4, 2); // 删减了无用的边界采样器，改为2个静态采样器
    g_CommonRS[0].InitAsConstants(0, 4);
    g_CommonRS[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10);
    g_CommonRS[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 10);
    g_CommonRS[3].InitAsConstantBuffer(1);
    g_CommonRS.InitStaticSampler(0, SamplerLinearClampDesc);
    g_CommonRS.InitStaticSampler(1, SamplerLinearWrapDesc); // 替换为基础绘制常用的 Wrap
    g_CommonRS.Finalize(L"GraphicsCommonRS");
}

void Graphics::DestroyCommonState(void)
{
    DefaultTextures[kMagenta2D].Destroy();
    DefaultTextures[kWhiteOpaque2D].Destroy();

    // [非必要功能] DispatchIndirectCommandSignature.Destroy(); ...
}
