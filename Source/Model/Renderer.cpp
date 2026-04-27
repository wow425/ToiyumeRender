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
    m_DefaultPSO.SetInputLayout(5, layout);                                            // 输入布局
    m_DefaultPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);      // 图元拓扑
    m_DefaultPSO.SetRenderTargetFormats(1, &ColorFormat, DepthFormat);                  // RTV绑定
    m_DefaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));                   // VS绑定
    m_DefaultPSO.SetPixelShader(g_pDefaultPS, sizeof(g_pDefaultPS));                    // PS绑定

    // onlySaberModel PSO
    m_onlySaberModelPSO = m_DefaultPSO;
    m_onlySaberModelPSO.Finalize();

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
    ColorPSO.SetPixelShader(g_pDefaultVS, sizeof(g_pDefaultVS));




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
void MeshSorter::AddMesh(
    const Mesh& mesh, float distance,           // distance: 通常是物体包围盒中心到相机的平方距离
    D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
    D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
    D3D12_GPU_VIRTUAL_ADDRESS bufferPtr)
{
    // 排序键 (Sort Key / ソートキー)。
    // 【硬件溯源】：这是一个 64-bit 的结构体/联合体。在成百上千个物体排序时，
    // 将排序用的 Key 与实际臃肿的 Mesh 数据分离，能极其有效地利用 CPU 的 L1/L2 缓存，避免 Cache Miss。
    SortKey key;

    // 低 32 位存储物体在数组中的真实索引 (Index)。排序完成后，根据这个索引去取实际数据。
    key.value = m_SortObjects.size();

    // 提取管线标志位。
    // 术语：半透明混合 (Alpha Blending / アルファブレンド)
    bool alphaBlend = (mesh.psoFlags & PSOFlags::kAlphaBlend) == PSOFlags::kAlphaBlend;
    // 术语：透明度测试 (Alpha Test / アルファテスト) - 常用于树叶、铁丝网。
    bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;

    // 【底层推导】：Alpha Test 会在片段着色器中执行 `discard` (HLSL) 或 `clip` 指令。
    // 这会导致 GPU 无法在光栅化阶段提前判定深度（破坏 Early-Z），进而增加 寄存器压力 (Register Pressure)。
    // 所以必须给 Alpha Test 的网格单独分配 depthPSO (值为 1)，在渲染队列中把它们和普通不透明物体隔离开来。
    uint64_t depthPSO = alphaTest ? 1 : 0;

    // 【核心黑魔法】：浮点数位强转 (Float Bitcast / 浮動小数点ビットキャスト)
    // 根据 IEEE 754 标准，正浮点数的内存二进制位（符号位 0，阶码，尾数），
    // 如果被直接当作无符号整数 (uint32_t) 读取，其大小的单调递增性是绝对不变的！
    // 为什么要这么做？因为 CPU 的 ALU 执行整数非比较排序（如 基数排序 Radix Sort）的速度，
    // 远远碾压基于分支预测的浮点数快速排序。
    union float_or_int { float f; uint32_t u; } dist;
    dist.f = Max(distance, 0.0f); // 钳制为 >= 0，保证最高符号位永远为 0。

    // --- 路由分配：阴影通道 (Shadow Pass / シャドウパス) ---
    if (m_BatchType == kShadows)
    {
        // 物理光学逻辑：纯半透明物体（如玻璃）透光率高，在简化的实时渲染中通常不投射深度阴影。
        if (alphaBlend)
            return;

        key.passID = kZPass;
        // 巧妙的偏移：depthPSO(0或1) + 4，保证 Alpha Test 物体在纯不透明物体之后渲染。
        key.psoIdx = depthPSO + 4;

        // 阴影贴图由近及远渲染，dist.u 越小（越近），Key 值越小，排在越前面。
        key.key = dist.u;

        m_SortKeys.push_back(key.value);
        m_PassCounts[kZPass]++;
    }
    // --- 路由分配：透明通道 (Transparent Pass / トランスペアレントパス) ---
    else if (mesh.psoFlags & PSOFlags::kAlphaBlend)
    {
        key.passID = kTransparent;
        key.psoIdx = mesh.pso;

        // 【数学推导】：半透明混合基于画家算法：C_out = C_src * Alpha + C_dst * (1 - Alpha)。
        // 物理上必须严格 由远及近 (Back-to-Front) 绘制，否则颜色混合结果全错。
        // `~dist.u` 是按位取反 (Bitwise NOT)。
        // 距离 dist 越大 -> 原始比特位的值越大 -> 按位取反后的无符号整数值越 *小*。
        // 这样在同一套升序排序算法下，远处的物体自然就排到了数组最前面，极度优雅且无分支。
        key.key = ~dist.u;

        m_SortKeys.push_back(key.value);
        m_PassCounts[kTransparent]++;
    }
    // --- 路由分配：不透明通道 (Opaque Pass / オペークパス) ---
    // 注释提到了“阉割 separatezpass”，意味着当前架构取消了 Pre-Z 预渲染阶段，直接走 Forward 或 Deferred。
    else
    {
        key.passID = kOpaque;
        // 把具有相同 PSO 的物体紧凑排在一起。
        // 这在 DX12 的 CommandList 录制中至关重要：减少 `SetPipelineState` 的调用频率，
        // 避免 GPU 中的 Wavefront (AMD) / Warp (NVIDIA) 产生执行气泡 (Execution Bubbles)。
        key.psoIdx = mesh.pso;

        // 不透明物体 由近及远 (Front-to-Back) 渲染。
        // 先画近处的，远处的像素通不过 Z-Test，硬件层面直接丢弃 (Early-Z)，极大减少 Overdraw (过度绘制)。
        key.key = dist.u;

        m_SortKeys.push_back(key.value);
        m_PassCounts[kOpaque]++;
    }

    // 将实际的网格数据和描述符地址打包并存入数组。
    // 【避坑与优化 (DX12 Nightmare)】：
    // 这里的 meshCBV 等地址是 GPU 显存虚拟地址 (GPU Virtual Address)。
    // 在多帧并发 (Frame Buffering) 的 DX12 引擎中，极其容易出现 “常量缓冲区被覆盖” 的问题。
    // 如果没有使用 Ring Buffer 机制或未能正确下达 资源屏障 (Resource Barrier / リソースバリア)，
    // GPU 在读取此地址时，数据可能已被 CPU 写入了下一帧的值，导致画面剧烈闪烁甚至 Device Lost (TDR 崩溃)。
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
    context.SetDescriptorTable(kCommonSRVs, m_CommonTextures);

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

