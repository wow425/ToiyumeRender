

#pragma once

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/Heap/UploadBuffer.h"
#include "03_AssetSystem/Importers/Texture/TextureManager.h"
#include "00_Core/Math/VectorMath.h"
#include "05_Scene/Camera/Camera.h"
#include "04_Renderer/Pipeline/PipelineDesc.h" // 顶点布局枚举，不包含 PSO 实例
#include "04_Renderer/Material/Material.h"     // 引入材质结构

#include <cstdint>

namespace Renderer
{
	class MeshSorter;
}


namespace Scene::Model
{
	class MeshSorter;

	// 资源层 Mesh：只保存几何缓冲、绘制范围、节点 CB 和材质槽索引，不保存 PSO/root signature。
	struct Mesh
	{
		uint32_t vbOffset;      // BufferLocation - Buffer.GpuVirtualAddress        VB偏移
		uint32_t vbSize;        // SizeInBytes。                                     VB字节大小

		uint32_t vbDepthOffset; // BufferLocation - Buffer.GpuVirtualAddress
		uint32_t vbDepthSize;   // SizeInBytes

		uint32_t ibOffset;      // BufferLocation - Buffer.GpuVirtualAddress        IB偏移
		uint32_t ibSize;        // SizeInBytes。                                     IB字节大小

		uint8_t  ibFormat;      // DXGI_FORMAT。                                     IB格式
		uint8_t  vbStride;      // StrideInBytes。                                   VB字节步长
		uint8_t  vbDepthStride;  // Depth-only VB stride in bytes.                   深度顶点字节步长
		uint8_t  pad;

		uint16_t meshCBV;       // Index of mesh constant buffer。                   网格CB索引
		uint16_t materialSlotIdx;   // Material slot index; material owns CBV/textures.  材质槽索引

		uint16_t srvTable;      // Offset into SRV descriptor heap for textures。    纹理SRV堆上偏移

		uint16_t vertexFlags;   // Geometry-only vertex attributes, from VertexLayoutFlags.
		uint16_t numDraws = 1;  // Number of draw groups。TODO:目前不分组，未来再拓展

		struct Draw
		{
			uint32_t primCount;   // Number of indices = 3 * number of triangles
			uint32_t startIndex;  // Offset to first index in index buffer 
			uint32_t baseVertex;  // Offset to first vertex in vertex buffer
		};
		Draw draw[1];           // Actually 1 or more draws
	};

	// 优化手段：变换节点构建场景图。
	struct GraphNode
	{
		Math::Matrix4 xform; // 变换矩阵（translation X rotation X scale）
		Math::Quaternion rotation;
		Math::XMFLOAT3 scale;

		uint32_t matrixIdx : 28;
		uint32_t hasSibling : 1;
		uint32_t hasChildren : 1;
		uint32_t staleMatrix : 1;
		uint32_t skeletonRoot : 1;
	};

	class Model
	{
	public:
		~Model() { Destroy(); }

		void GatherRenderables(::Renderer::MeshSorter& sorter, const GpuBuffer& meshConstants) const;

		Math::BoundingSphere m_BoundingSphere; // Object-space bounding sphere
		Math::AxisAlignedBox m_BoundingBox;

		ByteAddressBuffer m_DataBuffer;         // 网格数据
		ByteAddressBuffer m_MaterialConstants;  // 材质CB
		uint32_t m_NumNodes;                    // 节点总数，配合场景图
		uint32_t m_NumMeshes;                   // 网格总数
		std::unique_ptr<uint8_t[]> m_MeshData;  // 
		std::unique_ptr<GraphNode[]> m_SceneGraph; // 场景图
		std::vector<TextureRef> textures;       // 纹理索引

		std::vector<Scene::Material::Material> m_Materials;  // 模型需在加载时一并构建逻辑材质实例数组，供渲染时查询。

	protected:
		void Destroy();
	};

	class ModelInstance
	{
	public:
		ModelInstance();
		ModelInstance(std::shared_ptr<const Model> sourceModel);
		ModelInstance(const ModelInstance& modelInstance);

		~ModelInstance()
		{
			m_MeshConstantsCPU.Destroy();
			m_MeshConstantsGPU.Destroy();
		}

		ModelInstance& operator=(std::shared_ptr<const Model> sourceModel);

		bool IsNull(void) const { return m_Model == nullptr; }

		// R5：只读访问底层 Model。
		// ------------------------------------------------------------------
		// 光追路径需要遍历 Mesh 表来建 BLAS（m_NumMeshes / m_MeshData /
		// m_DataBuffer 都是 Model 的公开成员），但 ModelInstance 一直把
		// m_Model 藏在 private 里。这里只暴露 const 指针，不暴露可变句柄，
		// 保证外部无法绕过 m_Locator 去改实例状态（那是 R3/R4 踩过的坑）。
		const Model* GetModel(void) const { return m_Model.get(); }

		// R5：实例的 local-to-world 矩阵（TLAS 的 InstanceTransform 来源）。
		// ------------------------------------------------------------------
		// m_Locator 是实例变换的唯一真源，Update() 也是用它算 MeshConstants。
		// 光追的 TLAS 必须与光栅的 MeshConstants.World 完全一致，
		// 否则两条路径的几何会错位（诊断画面里表现为「模型不在原位」）。
		// 这里只读地暴露同一份变换，不提供 setter。
		Math::Matrix4 GetLocalToWorld(void) const { return Math::Matrix4((Math::AffineTransform)m_Locator); }

		void Update(GraphicsContext& gfxContext, float deltaTime);
		void GatherRenderables(::Renderer::MeshSorter& sorter) const;

		void Resize(float newRadius);

		// R4.6：设置/读取模型中心的世界坐标位置（支持 UI 里摆放多个模型）。
		// ------------------------------------------------------------
		// 为什么放在 ModelInstance 内部，而不是外部直接改矩阵：
		//   m_Locator 是实例内部维护的**唯一变换真源**，Update() 用它算
		//   MeshConstants。外部绕过去改矩阵，下一帧 Update 就覆盖回来了。
		//
		// 为什么必须 MarkDirty()：
		//   Update() 有 IsDirty 门控（见 Model.cpp），不置脏的话位置改动
		//   不会重新上传到 GPU —— 这是 R3 期间踩过的坑
		//   （ModelInstance::operator= 忘置脏导致"赋了值但模型没动"）。
		void SetWorldPosition(const Math::Vector3& position);
		Math::Vector3 GetWorldPosition() const;

		Math::Vector3 GetCenter() const;
		Math::Scalar GetRadius() const;
		Math::BoundingSphere GetBoundingSphere() const;
		Math::OrientedBox GetBoundingBox() const;

		bool IsDirty() const { return m_IsDirty; }
		void MarkDirty() { m_IsDirty = true; }
		void ClearDirty() { m_IsDirty = false; }


	private:
		std::shared_ptr<const Model> m_Model;
		UploadBuffer m_MeshConstantsCPU;
		ByteAddressBuffer m_MeshConstantsGPU;
		std::unique_ptr<Math::AffineTransform[]> m_BoundingSphereTransforms;
		Math::UniformTransform m_Locator;

		bool m_IsDirty = true;

	};

}

