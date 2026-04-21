

#include "Model.h"
#include "Renderer.h"
#include "ConstantBuffers.h"

using namespace Math;
using namespace Renderer;

void Model::Destroy()
{
    m_DataBuffer.Destroy();
    m_MaterialConstants.Destroy();
    m_NumMeshes = 0;
    m_MeshData = nullptr;
}

void Model::Render(MeshSorter& sorter, const GpuBuffer& meshConstants) const
{
    // 获取当前Mesh指针
    const uint8_t* pMesh = m_MeshData.get();

    for (uint32_t i = 0; i < m_NumMeshes; ++i)
    {
        // const Mesh& mesh = *(const Mesh*)pMesh; 现代化
        const Mesh& mesh = *reinterpret_cast<const Mesh*>(pMesh);

        // 提交到渲染队列，等待后续Drawcall录制。 目前阉割掉优化排序
        sorter.AddMesh(mesh,
            meshConstants.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
            m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * mesh.materialCBV,
            m_DataBuffer.GetGpuVirtualAddress());

        // 偏移到下一个Mesh
        pMesh += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

void ModelInstance::Render(MeshSorter& sorter) const
{
    if (m_Model != nullptr)
    {
        //const Frustum& frustum = sorter.GetWorldFrustum();
        m_Model->Render(sorter, m_MeshConstantsGPU);
    }
}

ModelInstance::ModelInstance(std::shared_ptr<const Model> sourceModel)
    : m_Model(sourceModel), m_Locator(kIdentity)
{
    // 断言确保MeshConstants为256字节规格。按位与255等于256取余。对于 2 的幂次N，x & (N - 1) 等价于 x % N
    static_assert((_alignof(MeshConstants) & 255) == 0, "CBV need 256 byte alignment");
    // 允许空壳存在是为极致性能和异步架构做出的必要妥协。
    // 异步加载。模型实例对象池预分配以确保内存连续性。
    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
    }
    else // 为sourceModel分配网格材质(世界矩阵）的上传堆，默认堆，并资源上传
    {
        m_MeshConstantsCPU.Create(L"Mesh Constant Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));
    }
}

ModelInstance::ModelInstance(const ModelInstance& modelInstance)
    : ModelInstance(modelInstance.m_Model)
{
}

ModelInstance& ModelInstance::operator= (std::shared_ptr<const Model> sourceModel)
{
    m_Model = sourceModel;
    m_Locator = UniformTransform(kIdentity);
    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
    }
    else
    {
        m_MeshConstantsCPU.Create(L"Mesh Constans Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));
    }

    return *this;
}

void ModelInstance::Update(GraphicsContext& gfxContext, float deltaTime)
{
    if (m_Model == nullptr) return;

    static const size_t kMaxStackDepth = 32;
    size_t stackIdx = 0;
    Matrix4 matrixStack[kMaxStackDepth];
    Matrix4 ParentMaterix = Matrix4((AffineTransform)m_Locator);

    // 2. 映射到CPU内存(Write-Combined)
    MeshConstants* cb = (MeshConstants*)m_MeshConstantsCPU.Map();
    const GraphNode* sceneGraph = m_Model->m_SceneGraph.get(); // 取静态场景图。

    // 3. 层级遍历，目前不做优化。
    for (const GraphNode* Node = sceneGraph; ; ++Node) // for里不写终止条件，硬件预取友好，分支预测优化，未吃透逻辑
    {
        Matrix4 transform = Node->transform;

        // 静态模型直接叠加父节点矩阵。局部坐标空间叠加，如右手腕跟右小臂变换继承关系
        transform = ParentMaterix * transform;

        // world跟worldIT写入CB
        cb[Node->matrixIdx].World = transform;
        cb[Node->matrixIdx].WorldIT = InverseTranspose(transform.Get3x3());

        // 维护矩阵栈以正确处理树状层级
        if (Node->hasChildren) // 有子（下一层）
        {
            if (Node->hasSibling) // 有兄弟姐妹（同层级）
            {
                matrixStack[stackIdx++] = ParentMaterix;
            }
            ParentMaterix = transform;
        }
        else if (!Node->hasChildren)
        {
            if (stackIdx == 0) break;
            ParentMaterix = matrixStack[--stackIdx];
        }
    }

    // 4. 解除映射并异步拷贝至GPU
    m_MeshConstantsCPU.Unmap();

    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true); // 立即执行资源转换
    gfxContext.GetCommandList()->CopyBufferRegion(m_MeshConstantsGPU.GetResource(), 0,      // CPU端CB上传堆资源写入到CB默认堆中
        m_MeshConstantsCPU.GetResource(), 0,
        m_MeshConstantsCPU.GetBufferSize());
    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ); // 延迟资源转换
}






