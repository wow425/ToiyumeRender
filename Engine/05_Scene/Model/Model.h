

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

		void Update(GraphicsContext& gfxContext, float deltaTime);
		void GatherRenderables(::Renderer::MeshSorter& sorter) const;

		void Resize(float newRadius);
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

