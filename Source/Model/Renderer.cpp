#include "PCH.h"
#include "Renderer.h"
#include "Model.h"
#include "../Resource/ResourceManager/TextureManager.h"
#include "../Resource/ResourceManager/BufferManager.h"
#include "ConstantBuffers.h"
#include "../RHI/PipelineState/RootSignature.h"
#include "../RHI/PipelineState/PipelineState.h"
#include "../RHI/PipelineState/GraphicsCommon.h"


#pragma warnin(disable:4319) // 关闭警告：零扩展警告?

using namespace Math;
using namespace Graphics;
using namespace Renderer;

namespace Renderer
{
    bool s_Initialized = false; // 初始化标志位

    DescriptorHeap s_TextureHeap; // 纹理堆
    DescriptorHeap s_SamplerHeap; // 采样器堆
    std::vector<GraphicsPSO> sm_PSOs; // 全局PSO池

    RootSignature m_RootSig;
    GraphicsPSO m_onlySaberModelPSO(L"Renderer: onlySaberModelPSO"); // 测试初始功能用SaberPSO
    GraphicsPSO m_DefaultPSO(L"Renderer: Default PSO"); //  Not finalized.  Used as a template.

    DescriptorHandle m_CommonTextures; // 通用纹理描述符句柄
}

void Renderer::Initialize(void)
{
    if (s_Initialized) return;

    SamplerDesc DefaultSamplerDesc;
    DefaultSamplerDesc.MaxAnisotropy = 8;

    SamplerDesc onlySaberModelSamplerDesc = DefaultSamplerDesc; // 测试初始功能用Saber采样器

    // 根签名设置并finalize
    m_RootSig.Reset(kNumRootBindings, 3); // 初始化分配根参数内存
    m_RootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
    m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量
    m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量
    m_RootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
    m_RootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
    m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
    m_RootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV
    m_RootSig.Finalize(L"RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneColorBuffer.GetFormat();
    // 构建输入布局
    D3D12_INPUT_ELEMENT_DESC posOnly[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_INPUT_ELEMENT_DESC posAndUV[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    ASSERT(sm_PSOs.size() == 0, "sm_PSOs初始化失败");

    // 构建PSO
        // default PSO
    m_DefaultPSO.SetRootSignature(m_RootSig);                                           // 根签名
    m_DefaultPSO.SetRasterizerState(RasterizerDefault);                                 // 光栅状态
    m_DefaultPSO.SetBlendState(BlendDisable);                                           // 混合模式
    m_DefaultPSO.SetDepthStencilState(DepthStateReadWrite);                             // 深度模板状态
    m_DefaultPSO.SetInputLayout(0, nullptr);                                            // 输入布局
    m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
    m_DefaultPSO.SetRenderTargetFormats(1, &ColorFormat, DepthFormat);                  // RTV绑定
    m_DefaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));                   // VS绑定
    m_DefaultPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));                    // PS绑定


}

void Renderer::UpdateGlobalDescriptors(void)
{

}

void Renderer::Shutdown(void)
{

}

uint8_t Renderer::GetPSO(uint16_t psoFlags)
{

}

void MeshSorter::AddMesh(
    const Mesh& mesh,
    D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
    D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
    D3D12_GPU_VIRTUAL_ADDRESS bufferPtr)
{

}

void MeshSorter::RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals)
{

}

