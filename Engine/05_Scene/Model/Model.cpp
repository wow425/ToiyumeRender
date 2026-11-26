

#include "Model.h"
#include "04_Renderer/Renderer/Forward/ForwardRenderer.h"
#include "03_AssetSystem/Assets/Constants/ConstantBuffers.h"

using namespace Math;
using namespace Renderer;

void Scene::Model::Model::Destroy()
{
	if (m_ReleaseMaterialDescriptors)
	{
		m_ReleaseMaterialDescriptors();
		m_ReleaseMaterialDescriptors = nullptr;
	}
	m_DataBuffer.Destroy();
	m_MaterialConstants.Destroy();
	m_NumMeshes = 0;
	m_MeshData = nullptr;
}

// 把模型中的每个 Mesh 打包成一条“可渲染项”，提交给 MeshSorter，由后者统一排序并在之后阶段发出 DrawCall。
void Scene::Model::Model::GatherRenderables(::Renderer::MeshSorter& sorter, const GpuBuffer& meshConstants) const
{
	// 获取当前Mesh指针
	const uint8_t* pMesh = m_MeshData.get();

	for (uint32_t i = 0; i < m_NumMeshes; ++i)
	{
		const Scene::Model::Mesh& mesh = *(const Scene::Model::Mesh*)pMesh;
		const Scene::Material::Material& material = m_Materials[mesh.materialSlotIdx]; // 取出解耦的材质实例
		float distance = 0.0f; // 用于排序（透明排序 / front-to-back）暂时不用管，阉割掉
		// 提交到渲染队列，等待后续Drawcall录制。 目前阉割掉优化排序

		// 测试用。初始值不是0，是 Node 与 Mesh 解耦的必然结果
		// 本质上不是当前 Mesh 在 Mesh 数组中的索引，而是这个 Mesh 所挂载的 Node 在全局节点数组中的线性索引 (Linear Index)。
		// Node0是场景根节点， Node1是空节点，blender导出时自动添加。 Node2是第一个Mesh节点，Node3是第二个Mesh节点，以此类推。Node与Mesh的解耦允许更灵活的场景层级结构设计。

		// 提交到渲染队列，带上 Material 的引用以供策略判断
		sorter.AddMesh(mesh, material, 0.0f,
			meshConstants.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
			mesh.materialSlotIdx,
			m_DataBuffer.GetGpuVirtualAddress());

		// 偏移到下一个Mesh
		pMesh += sizeof(Scene::Model::Mesh) + (mesh.numDraws - 1) * sizeof(Scene::Model::Mesh::Draw);
	}
}

// 剔除，排序，打包添加到MeshSorter中
void Scene::Model::ModelInstance::GatherRenderables(::Renderer::MeshSorter& sorter) const
{
	if (m_Model != nullptr)
	{
		//const Frustum& frustum = sorter.GetWorldFrustum(); 剔除不在视锥体内的暂未搞 TODO
		m_Model->GatherRenderables(sorter, m_MeshConstantsGPU);
	}
}

Scene::Model::ModelInstance::ModelInstance() : m_Locator(kIdentity) {}

Scene::Model::ModelInstance::ModelInstance(std::shared_ptr<const Model> sourceModel)
	: m_Model(sourceModel), m_Locator(kIdentity)
{
	// 断言确保MeshConstants为256字节规格。按位与255等于256取余。对于 2 的幂次N，x & (N - 1) 等价于 x % N
	static_assert((_alignof(MeshConstants) & 255) == 0, "CBV need 256 byte alignment");
	// 允许空壳存在是为极致性能和异步架构做出的必要妥协。
	// 异步加载。模型实例对象池预分配以确保内存连续性。
	if (sourceModel == nullptr)
	{
		m_MeshConstantsGPU.Destroy();
		m_NodeWorldMatrices.clear();
	}
	else // 创建Mesh CB
	{
		m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));
		ComputeNodeWorldMatrices();
	}
}

Scene::Model::ModelInstance::ModelInstance(const ModelInstance& modelInstance)
	: ModelInstance(modelInstance.m_Model)
{
}

Scene::Model::ModelInstance& Scene::Model::ModelInstance::operator= (std::shared_ptr<const Model> sourceModel)
{
	m_Model = sourceModel;
	m_Locator = UniformTransform(kIdentity);

	// ============================================================
	// R3 修复：运行时换模型后模型不可见（整帧只剩天空/地面）
	// ------------------------------------------------------------
	// 下面会重建 m_MeshConstantsGPU（ByteAddressBuffer），新缓冲内容是未定义的。
	// 而 Update() 的第二个守卫是 if (!m_IsDirty) return;：
	//   第 1 帧 EndFrame 已经 ClearDirty -> m_IsDirty == false
	//   此时换模型 -> 缓冲重建但 Update 早退 -> MeshConstants 从未上传
	//   -> 每次 draw 的 WorldMatrix / WorldIT 全是垃圾 -> 顶点全被压成退化三角形
	// 现象：日志一切正常（sorter 里对象数没变、DrawIndexed 照常提交、无 Debug Layer 报错），
	//       但画面上模型凭空消失。与「只剩清屏色」的 Forward 分支是同一个根因。
	// 正确语义：换了模型数据就是脏的，必须重新上传。
	// ============================================================
	m_IsDirty = true;


	auto s = m_Locator.GetScale();
	auto t = m_Locator.GetTranslation();
	(void)s;
	(void)t;

	if (sourceModel == nullptr)
	{
		m_MeshConstantsGPU.Destroy();
		m_NodeWorldMatrices.clear();
	}
	else
	{
		m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));
		ComputeNodeWorldMatrices();
	}

	return *this;
}

void Scene::Model::ModelInstance::ComputeNodeWorldMatrices()
{
	if (m_Model == nullptr || m_Model->m_NumNodes == 0 || m_Model->m_SceneGraph == nullptr)
	{
		m_NodeWorldMatrices.clear();
		return;
	}

	m_NodeWorldMatrices.assign(m_Model->m_NumNodes, Math::Matrix4(Math::kIdentity));

	static const size_t kMaxStackDepth = 32;
	size_t stackIdx = 0;
	Matrix4 matrixStack[kMaxStackDepth];
	Matrix4 parentMatrix = Matrix4((AffineTransform)m_Locator);

	const GraphNode* sceneGraph = m_Model->m_SceneGraph.get();
	const GraphNode* sceneGraphEnd = sceneGraph + m_Model->m_NumNodes;

	// [TD-06c] 展平遍历 GraphNode 层级树，写入每个节点的真实世界矩阵（m_Locator * node->xform）。
	// 同步增加节点上界与 32 层矩阵栈守卫，防止深层级资产越界。
	for (const GraphNode* node = sceneGraph; node < sceneGraphEnd; ++node)
	{
		const Math::Matrix4 transform = parentMatrix * node->xform;
		if (node->matrixIdx < m_NodeWorldMatrices.size())
		{
			m_NodeWorldMatrices[node->matrixIdx] = transform;
		}

		if (node->hasChildren)
		{
			if (node->hasSibling)
			{
				ASSERT(stackIdx < kMaxStackDepth, "SceneGraph hierarchy depth exceeded 32");
				if (stackIdx < kMaxStackDepth)
					matrixStack[stackIdx++] = parentMatrix;
			}
			parentMatrix = transform;
		}
		else
		{
			if (!node->hasSibling)
			{
				if (stackIdx == 0)
					break;
				parentMatrix = matrixStack[--stackIdx];
			}
		}
	}
}

void Scene::Model::ModelInstance::Update(GraphicsContext& gfxContext, float deltaTime)
{
	(void)deltaTime;
	if (m_Model == nullptr || m_Model->m_NumNodes == 0) return;
	if (!m_IsDirty) return;

	// [TD-06c] 统一在 CPU 展平 GraphNode 世界矩阵，供光栅 MeshConstants 与 DXR TLAS 共用。
	ComputeNodeWorldMatrices();

	// [TD-04b / DX12 三时钟安全上传]
	// 原实现每帧 Map 覆写单一持久 UploadBuffer（m_MeshConstantsCPU），若连续两帧移动模型，
	// CPU 会覆写仍在 GPU 命令队列执行 CopyBufferRegion 的同一块上传内存（数据竞态/撕裂）。
	// 改用 CommandContext::ReserveUploadMemory 从 2 MiB 帧瞬态 LinearAllocator 分配上传区，
	// 随命令列表提交 Fence 自动退休回收，零跨帧竞态且无需持有多余常驻 UPLOAD 内存。
	const size_t totalBytes = static_cast<size_t>(m_Model->m_NumNodes) * sizeof(MeshConstants);
	DynAlloc upload = gfxContext.ReserveUploadMemory(totalBytes);
	MeshConstants* cb = reinterpret_cast<MeshConstants*>(upload.DataPtr);

	for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
	{
		const Math::Matrix4& transform = m_NodeWorldMatrices[i];
		cb[i].World = transform;
		cb[i].WorldIT = InverseTranspose(transform.Get3x3());
	}

	gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfxContext.GetCommandList()->CopyBufferRegion(
		m_MeshConstantsGPU.GetResource(), 0,
		upload.Buffer.GetResource(), upload.Offset,
		totalBytes);
	gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
}


void Scene::Model::ModelInstance::Resize(float newRadius)
{
	if (m_Model == nullptr)
		return;

	m_Locator.SetScale(newRadius / m_Model->m_BoundingSphere.GetRadius());
	ComputeNodeWorldMatrices();

	m_IsDirty = true;
}

// ============================================================
// R4.6：把模型中心摆到指定世界坐标（支持 UI 里摆放多个模型）
// ------------------------------------------------------------
// 语义注意：设置的是**包围球中心**的位置，不是模型原点。
// m_Locator 作用在模型的本地空间里，而本地空间的原点与包围球中心
// 通常并不重合（美术导出时模型常带偏移）。若直接把 translation 设成
// 目标点，模型会偏离目标一个"本地中心偏移 × 缩放"的量。
//
// 正确关系：  世界中心 = translation + scale * 本地中心
//              => translation = 目标位置 - scale * 本地中心
// ============================================================
void Scene::Model::ModelInstance::SetWorldPosition(const Vector3& position)
{
	if (m_Model == nullptr)
		return;

	const Scalar scale = m_Locator.GetScale();
	const Vector3 localCenter = m_Model->m_BoundingSphere.GetCenter();
	m_Locator.SetTranslation(position - localCenter * scale);
	// [TD-06c] 同步刷新展平节点世界矩阵，确保不走光栅 Update() 的 DXR 路径也能立即读到最新世界坐标。
	ComputeNodeWorldMatrices();

	// 必须置脏：Update() 只处理 dirty 的实例，否则新的 translation
	// 永远到不了 MeshConstants，表现为"改了位置但模型没动"。
	m_IsDirty = true;
}

Vector3 Scene::Model::ModelInstance::GetWorldPosition() const
{
	if (m_Model == nullptr)
		return Vector3(kOrigin);

	return m_Locator.GetTranslation() + m_Model->m_BoundingSphere.GetCenter() * m_Locator.GetScale();
}

Vector3 Scene::Model::ModelInstance::GetCenter() const
{
	if (m_Model == nullptr)
		return Vector3(kOrigin);

	return m_Locator * m_Model->m_BoundingSphere.GetCenter();
}

Scalar Scene::Model::ModelInstance::GetRadius() const
{
	if (m_Model == nullptr)
		return Scalar(kZero);

	// return m_Locator.GetScale() * m_Model->m_BoundingSphere.GetRadius();
	return  m_Model->m_BoundingSphere.GetRadius();
}


Math::OrientedBox Scene::Model::ModelInstance::GetBoundingBox() const
{
	if (m_Model == nullptr)
		return AxisAlignedBox(Vector3(kZero), Vector3(kZero));

	//Utility::Printf(L"m_Locator: [%f, %f, %f]\n", m_Locator.GetRotation(), m_Locator.GetScale(), m_Locator.GetTranslation());
	//return m_Locator * m_Model->m_BoundingBox;

	return  m_Model->m_BoundingBox;
}
