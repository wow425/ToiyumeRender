#pragma once


#include "Resource/Buffer/GpuBuffer.h"
#include "Math/VectorMath.h"
#include "Camera/Camera.h"
#include "RHI/Command/CommandContext.h"
#include "Resource/Buffer/UploadBuffer.h"
#include "Resource/ResourceManager/TextureManager.h"
#include <cstdint>
#include <vector>

#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
class ShadowCamera;
class ShadowBuffer;
struct GlobalConstants;
// Model
struct Mesh;
struct Joint;

namespace Renderer
{
    // 是否分离深度预渲染通道 (Z-Prepass)。开启后有助于利用 Early-Z 剔除，减少像素着色器的 Overdraw。
    // extern BoolVar SeparateZPass;

    using namespace Math;

    // PSO全局缓存池。
    extern std::vector<GraphicsPSO> sm_PSOs;
    extern RootSignature m_RootSig;
    // 存放CBV/SRV/UAV
    extern DescriptorHeap s_TextureHeap;
    extern DescriptorHeap s_SamplerHeap;
    // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。
    extern DescriptorHandle m_CommonTextures;

    // 根绑定槽位枚举
    enum RootBindings
    {
        kMeshConstants,     // 常量缓冲: 模型矩阵 (b0)
        kMaterialConstants, // 常量缓冲: 材质参数 (b1)
        kMaterialSRVs,      // 描述符表: 材质专属贴图 (t0-t9)
        kMaterialSamplers,  // 描述符表: 采样器 (s0-s9)
        kCommonSRVs,        // 描述符表: 全局通用贴图，如 IBL 贴图、阴影图 (t10+)
        kCommonCBV,         // 常量缓冲: 全局常量，如相机矩阵 (b2)
        kSkinMatrices,      // SRV (Buffer): 骨骼蒙皮矩阵数组
        kNumRootBindings    // 根参数总数，用于初始化 Root Signature
    };

    void Initialize(void);
    void Shutdown(void);

    // 根据传入的标志位（是否有法线、切线、蒙皮等）动态获取或编译 PSO。
    uint8_t GetPSO(uint16_t psoFlags);
    // 设置基于物理的图像照明 (Image-Based Lighting, IBL / イメージベースドライティング) 贴图
    void SetIBLTextures(TextureRef diffuseIBL, TextureRef specularIBL);
    void SetIBLBias(float LODBias);
    void UpdateGlobalDescriptors(void);
    void DrawSkybox(GraphicsContext& gfxContext, const Camera& camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);

    // 网格排序器：负责收集一帧内所有的渲染指令，并为了极致的性能进行排序
    class MeshSorter
    {
    public:
        // 批处理类型：主相机渲染，生成阴影贴图的深度渲染
        enum BatchType { kDefault, kShadows };
        // 绘制通道 (Draw Pass / ドローパス)：控制渲染顺序：深度处理，不透明，透明
        enum DrawPass { kZPass, kOpaque, kTransparent, kNumPasses };

        MeshSorter(BatchType type)
        {
            m_BatchType = type;
            m_Camera = nullptr;
            m_Viewport = {};
            m_Scissor = {};
            m_NumRTVs = 0;
            m_DSV = nullptr;
            m_SortObjects.clear();
            m_SortKeys.clear();
            std::memset(m_PassCounts, 0, sizeof(m_PassCounts));
            m_CurrentPass = kZPass;
            m_CurrentDraw = 0;
        }

        // --- 渲染目标 (Render Target) 绑定接口 ---
        void SetCamera(const BaseCamera& camera) { m_Camera = &camera; }
        void SetViewport(const D3D12_VIEWPORT& viewport) { m_Viewport = viewport; }
        void SetScissor(const D3D12_RECT& scissor) { m_Scissor = scissor; }
        void AddRenderTarget(ColorBuffer& RTV)
        {
            ASSERT(m_NumRTVs < 8); // DX12 硬件级限制：多重渲染目标 (MRT) 最多同时绑定 8个
            m_RTV[m_NumRTVs++] = &RTV;
        }
        void SetDepthStencilTarget(DepthBuffer& DSV) { m_DSV = &DSV; }

        // --- 空间视锥体接口，常用于视锥剔除 (Frustum Culling) ---
        const Frustum& GetWorldFrustum() const { return m_Camera->GetWorldSpaceFrustum(); }
        const Frustum& GetViewFrustum() const { return m_Camera->GetViewSpaceFrustum(); }
        const Matrix4& GetViewMatrix() const { return m_Camera->GetViewMatrix(); }

        // 将要渲染的模型投递到排序器中，注意这里直接传递了 GPU 虚拟地址 (GPU Virtual Address)
        void AddMesh(const Mesh& mesh, float distance,
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr,
            const Joint* skeleton = nullptr);

        void Sort(); // 触发排序逻辑

        // 正式向 CommandList 录制并提交绘制指令
        void RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals);

    private:

        // 核心设计：64位位段 (Bitfield) 排序键。
        struct SortKey
        {
            union
            {
                uint64_t value; // 直接对这个 64 位整数进行汇编级的数值比较
                struct
                {
                    uint64_t objectIdx : 16; // 1. 最低优先级：对象索引 (用于去重或稳定排序)
                    uint64_t psoIdx : 12;    // 2. 状态合并：相同的 PSO 会排在一起，大幅减少 SetPipelineState 调用
                    uint64_t key : 32;       // 3. 距离排序：不透明物体从前向后(Early-Z)，透明物体从后向前(Alpha Blend)
                    uint64_t passID : 4;     // 4. 最高优先级：Pass 顺序 (ZPass -> Opaque -> Transparent)
                };
            };
        };

        // 伴随的渲染数据负载 (Payload)
        struct SortObject
        {
            const Mesh* mesh;
            const Joint* skeleton;
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV;
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV;
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr; // 顶点/索引缓冲区的基地址
        };

        // 数据容器：分离 Key 和 Object 数组，完美契合 CPU Cache 的数据局部性 (Data Locality)
        std::vector<SortObject> m_SortObjects;
        std::vector<uint64_t> m_SortKeys;

        BatchType m_BatchType;
        uint32_t m_PassCounts[kNumPasses];
        DrawPass m_CurrentPass;
        uint32_t m_CurrentDraw;

        const BaseCamera* m_Camera;
        D3D12_VIEWPORT m_Viewport;
        D3D12_RECT m_Scissor;
        uint32_t m_NumRTVs;
        ColorBuffer* m_RTV[8];
        DepthBuffer* m_DSV;
    };

} // namespace Renderer
