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
struct GlobalConstants;
struct Mesh; // Model

namespace Renderer
{
    using namespace Math;

    // PSO全局缓存池。
    extern std::vector<GraphicsPSO> sm_PSOs;
    extern RootSignature m_RootSig;
    // 存放CBV/SRV/UAV描述符堆
    extern DescriptorHeap s_TextureHeap;
    extern DescriptorHeap s_SamplerHeap;
    // 通用纹理的描述符句柄 (如阴影贴图、SSAO 结果等全局共享的贴图)。
    extern DescriptorHandle m_CommonTextures;

    // 根绑定槽位枚举
    enum RootBindings
    {
        kMeshConstants,     // 常量缓冲: 模型矩阵 (b0)
        kMaterialConstants, // 常量缓冲: 材质参数 (b1)
        kMaterialSRVs,      // 描述符表: 材质纹理贴图 (t0-t9)
        kMaterialSamplers,  // 描述符表: 采样器 (s0-s9)
        kCommonSRVs,        // 描述符表: 全局通用贴图，如 IBL 贴图、阴影图 (t10+)
        kCommonCBV,         // 常量缓冲: 全局常量，如相机矩阵 (b2)

        kNumRootBindings    // 根参数总数，用于初始化 Root Signature
    };

    void Initialize(void);
    void Shutdown(void);

    // 根据传入的标志位（是否有法线、切线、蒙皮等）动态获取或编译 PSO。
    uint8_t GetPSO(uint16_t psoFlags);

    void UpdateGlobalDescriptors(void);

    class MeshSorter
    {
    public:
        enum BatchType { kDefault, kShadows };
        enum DrawPass { kZPass, kOpaque, kTransparent, kNumPasses };

        MeshSorter(BatchType type)
        {

            m_Camera = nullptr;
            m_Viewport = {};
            m_Scissor = {};
            m_NumRTVs = 0;
            m_DSV = nullptr;
            m_SortObjects.clear();
            std::memset(m_PassCounts, 0, sizeof(m_PassCounts));
            m_CurrentPass = kZPass;
            m_CurrentDraw = 0;
        }

        void SetCamera(const BaseCamera& camera) { m_Camera = &camera; }
        void SetViewport(const D3D12_VIEWPORT& viewport) { m_Viewport = viewport; }
        void SetScissor(const D3D12_RECT& scissor) { m_Scissor = scissor; }
        void AddRenderTarget(ColorBuffer& RTV)
        {
            ASSERT(m_NumRTVs < 8);
            m_RTV[m_NumRTVs++] = &RTV;
        }


        const Matrix4& GetViewMatrix() const { return m_Camera->GetViewMatrix(); }

        void AddMesh(const Mesh& mesh, float distance,
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr);

        void RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals);

    private:

        struct SortKey
        {
            union
            {
                uint64_t value;
                struct
                {
                    uint64_t objectIdx : 16;
                    uint64_t psoIdx : 12;
                    uint64_t key : 32;
                    uint64_t passID : 4;
                };
            };
        };

        struct SortObject
        {
            const Mesh* mesh;
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV;
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV;
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr;
        };

        std::vector<SortObject> m_SortObjects;
        std::vector<uint64_t> m_SortKeys;
        BatchType m_BatchType; // 批次类型（Main pass， shadow pass
        uint32_t m_PassCounts[kNumPasses]; // pass计数（render pass含多少个需要绘制的物体）
        DrawPass m_CurrentPass;            // 当前pass
        uint32_t m_CurrentDraw;            // 当前draw

        const BaseCamera* m_Camera;
        D3D12_VIEWPORT m_Viewport;
        D3D12_RECT m_Scissor;
        uint32_t m_NumRTVs;
        ColorBuffer* m_RTV[8];
        DepthBuffer* m_DSV;
    };

} // namespace Renderer
