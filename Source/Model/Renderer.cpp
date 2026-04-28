#include "PCH.h"
#include "Renderer.h"
#include "Model.h"
#include "../Resource/ResourceManager/TextureManager.h"
#include "../Resource/ResourceManager/BufferManager.h"
#include "ConstantBuffers.h"
#include "../RHI/PipelineState/RootSignature.h"
#include "../RHI/PipelineState/PipelineState.h"
#include "../RHI/PipelineState/GraphicsCommon.h"

#include "CompiledShaders/Model/DefaultVS.h"
#include "CompiledShaders/Model/DefaultPS.h"


#pragma warning(disable:4319) // 关闭警告：零扩展警告?

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
    m_RootSig.Reset(kNumRootBindings, 1); // 初始化分配根参数内存
    m_RootSig.InitStaticSampler(10, DefaultSamplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);
    m_RootSig[kMeshConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX); // 网格常量
    m_RootSig[kMaterialConstants].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_PIXEL); // 材质常量
    m_RootSig[kMaterialSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质SRV
    m_RootSig[kMaterialSamplers].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 材质采样器
    m_RootSig[kCommonSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 10, D3D12_SHADER_VISIBILITY_PIXEL); // 全局通用SRV
    m_RootSig[kCommonCBV].InitAsConstantBuffer(1);                                                                        // 全局通用CBV
    m_RootSig.Finalize(L"RootSig", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneDepthBuffer.GetFormat();
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

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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


    TextureManager::Initialize(L"");
    // 创建纹理资源堆
    s_TextureHeap.Create(L"Scene Texture Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
    // 创建采样器堆 Maybe only need 2 for wrap vs. clamp?  Currently we allocate 1 for 1 with textures
    s_SamplerHeap.Create(L"Scene Sampler Descriptors", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);
    // 创建Light资源堆

    // 创建common textures堆


    s_Initialized = true;
}

// 更新全局资源描述符堆，如SSAO,Shadow。目前不写
void Renderer::UpdateGlobalDescriptors(void)
{

}

void Renderer::Shutdown(void)
{
    TextureManager::Shutdown();
    s_TextureHeap.Destroy();
    s_SamplerHeap.Destroy();
}

// 阉割PSO动态选择shader和z-prepass延迟渲染用
uint8_t Renderer::GetPSO(uint16_t psoFlags)
{
    using namespace PSOFlags;

    GraphicsPSO ColorPSO = m_DefaultPSO;

    uint16_t Requirements = kHasPosition | kHasNormal;
    ASSERT((psoFlags & Requirements) == Requirements);

    std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout;
    if (psoFlags & kHasPosition)
        vertexLayout.push_back({ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT });
    if (psoFlags & kHasNormal)
        vertexLayout.push_back({ "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT });
    if (psoFlags & kHasTangent)
        vertexLayout.push_back({ "TANGENT",  0, DXGI_FORMAT_R10G10B10A2_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT });
    if (psoFlags & kHasUV0)
        vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT });
    else
        vertexLayout.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       1, D3D12_APPEND_ALIGNED_ELEMENT });
    if (psoFlags & kHasUV1)
        vertexLayout.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT });

    ColorPSO.SetInputLayout((uint32_t)vertexLayout.size(), vertexLayout.data());

    // TODO:阉割掉动态选择shader，以后添加功能时再加回来
    // TODO: 预编译shader带宏定义脚本
    ColorPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));
    ColorPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));




    if (psoFlags & kAlphaBlend)
    {
        ColorPSO.SetBlendState(BlendPreMultiplied);
        ColorPSO.SetDepthStencilState(DepthStateReadOnly);
    }
    ColorPSO.Finalize();

    // Look for an existing PSO
    for (uint32_t i = 0; i < sm_PSOs.size(); ++i)
    {
        if (ColorPSO.GetPipelineStateObject() == sm_PSOs[i].GetPipelineStateObject())
        {
            return (uint8_t)i;
        }
    }

    // If not found, keep the new one, and return its index
    sm_PSOs.push_back(ColorPSO);

    // Z-prepass阉割掉，延迟渲染时加回来

    ASSERT(sm_PSOs.size() <= 256, "Ran out of room for unique PSOs");

    return (uint8_t)sm_PSOs.size() - 1;
}


/**
*  该方法暂时搁置没啃，未来啃
 * @brief 什么是 MeshSorter::AddMesh：
 * 将当前帧场景中的网格实体压入一个待排序队列的核心方法。
 * @why 为什么需要它：
 * 在提交给 GPU 之前按特定规则排序，能最小化 管线状态对象 (Pipeline State Object / パイプラインステートオブジェクト, PSO)
 * 切换导致的流水线停顿 (Pipeline Flush)，并最大化硬件的 提前深度测试 (Early-Z / アーリZ) 效率。
 * @paper 进阶连结：
 * 此种利用 64-bit 整型位操作实现极速排序的架构，源自 Frostbite (寒霜引擎) 团队的经典分享
 * 《Destiny's Multithreaded Rendering Architecture》。
 */
void MeshSorter::AddMesh(const Mesh& mesh, float distance,
    D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
    D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
    D3D12_GPU_VIRTUAL_ADDRESS bufferPtr)
{
    SortKey key;
    key.value = m_SortObjects.size();

    bool alphaBlend = (mesh.psoFlags & PSOFlags::kAlphaBlend) == PSOFlags::kAlphaBlend;
    bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;

    uint64_t depthPSO = alphaTest ? 1 : 0;

    union float_or_int { float f; uint32_t u; } dist;
    dist.f = Max(distance, 0.0f);

    if (m_BatchType == kShadows)
    {
        if (alphaBlend)
            return;

        key.passID = kZPass;
        key.psoIdx = depthPSO + 4;
        key.key = dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kZPass]++;
    }
    else if (mesh.psoFlags & PSOFlags::kAlphaBlend)
    {
        key.passID = kTransparent;
        key.psoIdx = mesh.pso;
        key.key = ~dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kTransparent]++;
    }
    else if (alphaTest)
    {
        key.passID = kZPass;
        key.psoIdx = depthPSO;
        key.key = dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kZPass]++;

        key.passID = kOpaque;
        key.psoIdx = mesh.pso + 1;
        key.key = dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kOpaque]++;
    }
    else
    {
        key.passID = kOpaque;
        key.psoIdx = mesh.pso;
        key.key = dist.u;
        m_SortKeys.push_back(key.value);
        m_PassCounts[kOpaque]++;
    }

    SortObject object = { &mesh, meshCBV, materialCBV, bufferPtr };
    m_SortObjects.push_back(object);
}

// RenderScene
void MeshSorter::RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals)
{
    ASSERT(m_DSV != nullptr);

    Renderer::UpdateGlobalDescriptors();

    context.SetRootSignature(m_RootSig);
    context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, s_TextureHeap.GetHeapPointer());
    context.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, s_SamplerHeap.GetHeapPointer());

    // Set common textures
    // context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);

    // Set common shader constants
    globals.ViewProjMatrix = m_Camera->GetViewProjMatrix();
    globals.CameraPos = m_Camera->GetPosition();

    context.SetDynamicConstantBufferView(kCommonCBV, sizeof(GlobalConstants), &globals);

    //
    if (m_BatchType == kShadows)
    {
        context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE, true); // DSV转换深度写入，立刻执行
        context.ClearDepth(*m_DSV);
        context.SetDepthStencilTarget(m_DSV->GetDSV());

        if (m_Viewport.Width == 0)
        {
            m_Viewport.TopLeftX = 0.0f;
            m_Viewport.TopLeftY = 0.0f;
            m_Viewport.Width = (float)m_DSV->GetWidth();
            m_Viewport.Height = (float)m_DSV->GetHeight();
            m_Viewport.MaxDepth = 1.0f;
            m_Viewport.MinDepth = 0.0f;

            // 阴影缩小一圈，舍弃最外一圈，为了消除百分比靠近滤波 (Percentage-Closer Filtering, PCF / パーセンテージ・クロージャー・フィルタリング) 时
            // 产生的越界采样伪影 (Out-of-bounds Artifacts / 範囲外アクセスによるアーティファクト)。
            // TODO:做PCF时来理解原理
            m_Scissor.left = 1;
            m_Scissor.right = m_DSV->GetWidth() - 2;
            m_Scissor.top = 1;
            m_Scissor.bottom = m_DSV->GetHeight() - 2;
        }
    }
    else
    {
        // 检查各RTV
        for (uint32_t i = 0; i < m_NumRTVs; ++i)
        {
            ASSERT(m_RTV[i] != nullptr);
            ASSERT(m_DSV->GetWidth() == m_RTV[i]->GetWidth());
            ASSERT(m_DSV->GetHeight() == m_RTV[i]->GetHeight());
        }
        // 视口，裁剪矩阵设置
        if (m_Viewport.Width == 0)
        {
            m_Viewport.TopLeftX = 0.0f;
            m_Viewport.TopLeftY = 0.0f;
            m_Viewport.Width = (float)m_DSV->GetWidth();
            m_Viewport.Height = (float)m_DSV->GetHeight();
            m_Viewport.MaxDepth = 1.0f;
            m_Viewport.MinDepth = 0.0f;

            m_Scissor.left = 0;
            m_Scissor.right = m_DSV->GetWidth();
            m_Scissor.top = 0;
            m_Scissor.bottom = m_DSV->GetHeight();
        }
    }

    // Render Pass
    for (; m_CurrentPass <= pass; m_CurrentPass = (DrawPass)(m_CurrentPass + 1))
    {
        const uint32_t passCount = m_PassCounts[m_CurrentPass];
        if (passCount == 0) continue;
        // pass批次选择
        if (m_BatchType == kDefault)
        {
            switch (m_CurrentPass)
            {
            case kZPass:
                context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                context.SetDepthStencilTarget(m_DSV->GetDSV());
            case kOpaque:
                context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
                context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV());
                break;
            case kTransparent:
                context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_DEPTH_READ);
                context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
                context.SetRenderTarget(g_SceneColorBuffer.GetRTV(), m_DSV->GetDSV_DepthReadOnly());
                break;
            }
        }

        context.SetViewportAndScissor(m_Viewport, m_Scissor);
        context.FlushResourceBarriers(); // 提交屏障数组

        const uint32_t lastDraw = m_CurrentDraw + passCount; // 该pass需绘制物体的最大数量

        // 本Pass 
        while (m_CurrentDraw < lastDraw)
        {
            SortKey key;
            key.value = m_SortKeys[m_CurrentDraw]; // union用法巧妙
            const SortObject& object = m_SortObjects[key.objectIdx]; // 获取当前绘制物体
            const Mesh& mesh = *object.mesh;

            context.SetConstantBuffer(kMeshConstants, object.meshCBV);
            context.SetConstantBuffer(kMaterialConstants, object.materialCBV);
            context.SetDescriptorTable(kMaterialSRVs, s_TextureHeap[mesh.srvTable]);
            context.SetDescriptorTable(kMaterialSamplers, s_SamplerHeap[mesh.samplerTable]);

            context.SetPipelineState(sm_PSOs[key.psoIdx]);
            // 设置VB
            if (m_CurrentPass == kZPass)
            {
                bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;
                uint32_t stride = alphaTest ? 16u : 12u;

                context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbDepthOffset, mesh.vbDepthSize, stride });
            }
            else
            {
                context.SetVertexBuffer(0, { object.bufferPtr + mesh.vbOffset, mesh.vbSize, mesh.vbStride });
            }
            // 设置IB
            context.SetIndexBuffer({ object.bufferPtr + mesh.ibOffset, mesh.ibSize, (DXGI_FORMAT)mesh.ibFormat });
            // DrawCall
            for (uint32_t i = 0; i < mesh.numDraws; ++i)
            {
                context.DrawIndexed(mesh.draw[i].primCount, mesh.draw[i].startIndex, mesh.draw[i].baseVertex);
            }
            ++m_CurrentDraw;
        }
    }

    if (m_BatchType == kShadows)
    {
        context.TransitionResource(*m_DSV, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

