

#include "Model.h"
#include "Renderer.h"
#include "../Resource/Buffer/ConstantBuffers.h"

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
    : m_Model(sourceModel)
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


