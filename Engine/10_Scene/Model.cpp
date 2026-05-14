

#include "Model.h"
#include "09_Renderer/forwardRenderer.h"
#include "05_ResourceSystem/00_Buffers/ConstantBuffers.h"

using namespace Math;
using namespace Renderer;

void Model::Destroy()
{
	m_DataBuffer.Destroy();
	m_MaterialConstants.Destroy();
	m_NumMeshes = 0;
	m_MeshData = nullptr;
}

// 把模型中的每个 Mesh 打包成一条“可渲染项”，提交给 MeshSorter，由后者统一排序并在之后阶段发出 DrawCall。
void Model::GatherRenderables(MeshSorter& sorter, const GpuBuffer& meshConstants) const
{
	// 获取当前Mesh指针
	const uint8_t* pMesh = m_MeshData.get();

	for (uint32_t i = 0; i < m_NumMeshes; ++i)
	{
		const Mesh& mesh = *(const Mesh*)pMesh;

		float distance = 0.0f; // 用于排序（透明排序 / front-to-back）暂时不用管，阉割掉
		// 提交到渲染队列，等待后续Drawcall录制。 目前阉割掉优化排序

		// 测试用。初始值不是0，是 Node 与 Mesh 解耦的必然结果
		// 本质上不是当前 Mesh 在 Mesh 数组中的索引，而是这个 Mesh 所挂载的 Node 在全局节点数组中的线性索引 (Linear Index)。
		// Node0是场景根节点， Node1是空节点，blender导出时自动添加。 Node2是第一个Mesh节点，Node3是第二个Mesh节点，以此类推。Node与Mesh的解耦允许更灵活的场景层级结构设计。
		auto t1 = mesh.meshCBV;
		auto t2 = mesh.materialCBV;

		sorter.AddMesh(mesh, distance,
			meshConstants.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
			m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * mesh.materialCBV,
			m_DataBuffer.GetGpuVirtualAddress());

		// 偏移到下一个Mesh
		pMesh += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
	}
}

// 剔除，排序，打包添加到MeshSorter中
void ModelInstance::GatherRenderables(MeshSorter& sorter) const
{
	if (m_Model != nullptr)
	{
		//const Frustum& frustum = sorter.GetWorldFrustum(); 剔除不在视锥体内的暂未搞 TODO
		m_Model->GatherRenderables(sorter, m_MeshConstantsGPU);
	}
}

ModelInstance::ModelInstance() : m_Locator(kIdentity) {}

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
	else // 创建Mesh CB
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


	auto s = m_Locator.GetScale();
	auto t = m_Locator.GetTranslation();


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

	// 2. Map CPU 侧上传缓冲（通常是 Upload Heap）映射到CPU内存(Write-Combined)
	MeshConstants* cb = (MeshConstants*)m_MeshConstantsCPU.Map();
	const GraphNode* sceneGraph = m_Model->m_SceneGraph.get(); // 取静态场景图。

	// 3. 层级遍历，目前不做优化。
	for (const GraphNode* Node = sceneGraph; ; ++Node) // for里不写终止条件，硬件预取友好，分支预测优化，未吃透逻辑
	{
		// 当前节点的局部变换（local transform）
		Math::Matrix4 transform = ParentMaterix * Node->xform;

		MeshConstants& cbv = cb[Node->matrixIdx]; // 根据节点 index 找到对应 CB 位置 !!!!!!!!!!!!!!!!
		cbv.World = transform;
		cbv.WorldIT = InverseTranspose(transform.Get3x3());

		//m_BoundingSphereTransforms[Node->matrixIdx] = AffineTransform(
		//    (Vector3)transform.GetX(),
		//    (Vector3)transform.GetY(),
		//    (Vector3)transform.GetZ(),
		//    (Vector3)transform.GetW());

		// 维护矩阵栈以正确处理树状层级
		if (Node->hasChildren) // 有子（下一层）0
		{
			if (Node->hasSibling) // 有兄弟姐妹（同层级）
			{
				matrixStack[stackIdx++] = ParentMaterix; // 如果有兄弟节点，保存当前父矩阵（用于回溯）
			}
			ParentMaterix = transform; // 进入子节点：当前 transform 成为新的 parent
		}
		else if (!Node->hasChildren)
		{
			// 没有子节点 (叶子节点)
			if (!Node->hasSibling)
			{
				// 既没有子，也没有兄弟，说明当前分支彻底走完，需要回溯
				if (stackIdx == 0)
				{
					break; // 栈空且没有兄弟，说明整个树遍历完成，安全退出
				}
				ParentMaterix = matrixStack[--stackIdx]; // 回溯到上一个保存的父矩阵
			}
			// 【注意】：如果 (!Node->hasChildren) 但 (Node->hasSibling) 是 True
			// 说明下一个要处理的就是兄弟节点。它应该继续使用当前的 ParentMaterix，
			// 所以我们什么都不做，直接进入下一次 for 循环。
		}
	}

	// 4. 解除映射并异步拷贝至GPU
	m_MeshConstantsCPU.Unmap();

	gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true); // 立即执行资源转换
	// 每帧更新，直接拷贝整个缓冲区。对于大场景或频繁更新的情况，后续可优化为部分更新（CopyBufferRegion）或双缓冲/环形缓冲。
	gfxContext.GetCommandList()->CopyBufferRegion(m_MeshConstantsGPU.GetResource(), 0, m_MeshConstantsCPU.GetResource(), 0, m_MeshConstantsCPU.GetBufferSize());
	gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
}


void ModelInstance::Resize(float newRadius)
{
	if (m_Model == nullptr)
		return;

	m_Locator.SetScale(newRadius / m_Model->m_BoundingSphere.GetRadius());
}

Vector3 ModelInstance::GetCenter() const
{
	if (m_Model == nullptr)
		return Vector3(kOrigin);

	return m_Locator * m_Model->m_BoundingSphere.GetCenter();
}

Scalar ModelInstance::GetRadius() const
{
	if (m_Model == nullptr)
		return Scalar(kZero);

	// return m_Locator.GetScale() * m_Model->m_BoundingSphere.GetRadius();
	return  m_Model->m_BoundingSphere.GetRadius();
}


Math::OrientedBox ModelInstance::GetBoundingBox() const
{
	if (m_Model == nullptr)
		return AxisAlignedBox(Vector3(kZero), Vector3(kZero));

	//Utility::Printf(L"m_Locator: [%f, %f, %f]\n", m_Locator.GetRotation(), m_Locator.GetScale(), m_Locator.GetTranslation());
	//return m_Locator * m_Model->m_BoundingBox;

	return  m_Model->m_BoundingBox;
}





