

#pragma once

#include "../RHI/Command/CommandContext.h"
#include "../Resource/Buffer/GpuBuffer.h"
#include "../Resource/Buffer/UploadBuffer.h"
#include "../Resource/ResourceManager/TextureManager.h"
#include "../Math/VectorMath.h"
#include "../Camera/Camera.h"

#include <cstdint>


namespace Renderer
{
    class MeshSorter;
}


//
// To request a PSO index, provide flags that describe the kind of PSO
// you need.  If one has not yet been created, it will be created.
// 为申请PSO索引，提供描述所需PSO种类的flags，若未被创建，将被创建
namespace PSOFlags
{
    enum : uint16_t
    {
        kHasPosition = 0x001,  // Required
        kHasNormal = 0x002,  // Required
        kHasUV0 = 0x008,  // Required (for now)
        kHasUV1 = 0x010,
    };
}

// VB,IB,网格CB，材质CB,纹理堆，采样器堆，PSO，绘制信息。
struct Mesh
{
    uint32_t vbOffset;      // BufferLocation - Buffer.GpuVirtualAddress        VB偏移
    uint32_t vbSize;        // SizeInBytes。                                     VB字节大小

    uint32_t ibOffset;      // BufferLocation - Buffer.GpuVirtualAddress        IB偏移
    uint32_t ibSize;        // SizeInBytes。                                     IB字节大小

    uint8_t  ibFormat;      // DXGI_FORMAT。                                     IB格式
    uint8_t  vbStride;      // StrideInBytes。                                   VB字节步长

    uint16_t meshCBV;       // Index of mesh constant buffer。                   网格CB索引
    uint16_t materialCBV;   // Index of material constant buffer。               材质CB索引

    uint16_t srvTable;      // Offset into SRV descriptor heap for textures。    纹理SRV堆上偏移
    uint16_t samplerTable;  // Offset into sampler descriptor heap for samplers。采样器堆上偏移

    uint16_t psoFlags;      // Flags needed to request a PSO。                   PSOFlags
    uint16_t pso;           // Index of pipeline state object。                  PSO索引
    uint16_t numDraws = 1;      // Number of draw groups。                           绘制组数量。TODO:目前不分组，未来再拓展                  

    struct Draw
    {
        uint32_t primCount;   // Number of indices = 3 * number of triangles
        uint32_t startIndex;  // Offset to first index in index buffer 
        uint32_t baseVertex;  // Offset to first vertex in vertex buffer
    };
    Draw draw[1];           // Actually 1 or more draws
};

// 优化手段：变换节点构建场景图。目前只是占位用
struct GraphNode
{
    Math::Matrix4 transform; // 变换矩阵（translation X rotation X scale）
    Math::Quaternion rotation;
    Math::XMFLOAT3 scale;

    uint32_t matrixIdx : 28; // ?
    uint32_t hasSibling : 1;
    uint32_t hasChildren : 1;
    uint32_t staleMatrix : 1;
    uint32_t skeletonRoot : 1;
};

class Model
{
public:
    ~Model() { Destroy(); }

    void Render(Renderer::MeshSorter& sorter, const GpuBuffer& meshConstants) const;


    ByteAddressBuffer m_DataBuffer;         // 几何数据BUFFER
    ByteAddressBuffer m_MaterialConstants;  // 材质CB
    uint32_t m_NumNodes;                    // 节点总数，配合场景图
    uint32_t m_NumMeshes;                   // 网格总数
    std::unique_ptr<uint8_t[]> m_MeshData;  // 
    std::unique_ptr<GraphNode[]> m_SceneGraph; // 场景图
    std::vector<TextureRef> textures;       // 纹理索引

protected:
    void Destroy();
};

class ModelInstance
{
public:
    ModelInstance() {}
    ModelInstance(std::shared_ptr<const Model> sourceModel);
    ModelInstance(const ModelInstance& modelInstance);

    ~ModelInstance()
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
    }

    ModelInstance& operator=(std::shared_ptr<const Model> sourceModel);

    bool IsNull(void) const { return m_Model == nullptr; }

    void Update(GraphicsContext& gfxContext, float deltaTime);
    void Render(Renderer::MeshSorter& sorter) const;


private:
    std::shared_ptr<const Model> m_Model;
    UploadBuffer m_MeshConstantsCPU;
    ByteAddressBuffer m_MeshConstantsGPU;

    Math::UniformTransform m_Locator;
};
