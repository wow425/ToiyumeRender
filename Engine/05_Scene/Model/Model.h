

#pragma once

#include "02_RHI/Command/CommandContext.h"
#include "02_RHI/Resource/GpuBuffer.h"
#include "02_RHI/Resource/Heap/UploadBuffer.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"
#include "00_Core/Math/VectorMath.h"
#include "05_Scene/Camera/Camera.h"
#include "04_Renderer/Pipeline/PipelineDesc.h" // 椤剁偣甯冨眬鏋氫妇锛屼笉鍖呭惈 PSO 瀹炰緥
#include "04_Renderer/Material/Material.h"     // 寮曞叆鏉愯川缁撴瀯

#include <cstdint>

namespace Renderer
{
	class MeshSorter;
}


namespace Scene::Model
{
	class MeshSorter;

	// 璧勬簮灞?Mesh锛氬彧淇濆瓨鍑犱綍缂撳啿銆佺粯鍒惰寖鍥淬€佽妭鐐?CB 鍜屾潗璐ㄦЫ绱㈠紩锛屼笉淇濆瓨 PSO/root signature銆?
	struct Mesh
	{
		uint32_t vbOffset;      // BufferLocation - Buffer.GpuVirtualAddress        VB鍋忕Щ
		uint32_t vbSize;        // SizeInBytes銆?                                    VB瀛楄妭澶у皬

		uint32_t vbDepthOffset; // BufferLocation - Buffer.GpuVirtualAddress
		uint32_t vbDepthSize;   // SizeInBytes

		uint32_t ibOffset;      // BufferLocation - Buffer.GpuVirtualAddress        IB鍋忕Щ
		uint32_t ibSize;        // SizeInBytes銆?                                    IB瀛楄妭澶у皬

		uint8_t  ibFormat;      // DXGI_FORMAT銆?                                    IB鏍煎紡
		uint8_t  vbStride;      // StrideInBytes銆?                                  VB瀛楄妭姝ラ暱
		uint8_t  vbDepthStride;  // Depth-only VB stride in bytes.                   娣卞害椤剁偣瀛楄妭姝ラ暱
		uint8_t  pad;

		uint16_t meshCBV;       // Index of mesh constant buffer銆?                  缃戞牸CB绱㈠紩
		uint16_t materialSlotIdx;   // Material slot index; material owns CBV/textures.  鏉愯川妲界储寮?

		uint16_t srvTable;      // Offset into SRV descriptor heap for textures銆?   绾圭悊SRV鍫嗕笂鍋忕Щ

		uint16_t vertexFlags;   // Geometry-only vertex attributes, from VertexLayoutFlags.
		uint16_t numDraws = 1;  // Number of draw groups銆俆ODO:鐩墠涓嶅垎缁勶紝鏈潵鍐嶆嫇灞?

		struct Draw
		{
			uint32_t primCount;   // Number of indices = 3 * number of triangles
			uint32_t startIndex;  // Offset to first index in index buffer 
			uint32_t baseVertex;  // Offset to first vertex in vertex buffer
		};
		Draw draw[1];           // Actually 1 or more draws
	};

	// 浼樺寲鎵嬫锛氬彉鎹㈣妭鐐规瀯寤哄満鏅浘銆?
	struct GraphNode
	{
		Math::Matrix4 xform; // 鍙樻崲鐭╅樀锛坱ranslation X rotation X scale锛?
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

		ByteAddressBuffer m_DataBuffer;         // 缃戞牸鏁版嵁
		ByteAddressBuffer m_MaterialConstants;  // 鏉愯川CB
		uint32_t m_NumNodes;                    // 鑺傜偣鎬绘暟锛岄厤鍚堝満鏅浘
		uint32_t m_NumMeshes;                   // 缃戞牸鎬绘暟
		std::unique_ptr<uint8_t[]> m_MeshData;  // 
		std::unique_ptr<GraphNode[]> m_SceneGraph; // 鍦烘櫙鍥?
		std::vector<TextureRef> textures;       // 绾圭悊绱㈠紩

		std::vector<Scene::Material::Material> m_Materials;  // 妯″瀷闇€鍦ㄥ姞杞芥椂涓€骞舵瀯寤洪€昏緫鏉愯川瀹炰緥鏁扮粍锛屼緵娓叉煋鏃舵煡璇€?

		// R7锛氭潗璐ㄦ爣閲忕殑 CPU 渚у壇鏈紙baseColorFactor / metallicFactor /
		// roughnessFactor / emissiveFactor / normalTextureScale / flags锛夈€?
		// ------------------------------------------------------------------
		// 涓轰粈涔堥渶瑕佸畠锛氬厜鏍呰矾寰勯€氳繃 MaterialCBV锛?56 瀛楄妭姝ラ暱鐨?GPU 甯搁噺缂撳啿锛?
		// 鍦ㄧ潃鑹插櫒閲岃杩欎簺鏍囬噺锛孋PU 渚т粠涓嶅洖璇汇€備絾鍏夎拷璺緞瑕佸湪**寤?BLAS 鏃?*
		// 鎶婅繖浜涙爣閲忕儤杩涙瘡瀹炰緥鏁版嵁锛圧aytracingInstanceData 鏄?ByteAddressBuffer锛?
		// 鐢?CPU 濉級锛屾鏃?GPU 甯搁噺缂撳啿鎵€鍦ㄧ殑鏄?DEFAULT 鍫嗭紝鏃犳硶 Map銆?
		// 鍥犳鍔犺浇鏃剁暀涓€浠?CPU 鍓湰 鈥斺€?鏁版嵁婧愪笌涓婅鍒?GPU 鐨勬槸鍚屼竴浠?
		// materialConstantData锛屼笉浼氬嚭鐜颁袱澶勪笉涓€鑷淬€?
		std::vector<Scene::Material::MaterialConstantData> m_MaterialConstantData;

		// [WP-1B / TD-01b] 鏉愯川鎻忚堪绗﹀綊杩樺洖璋冿細Model 閿€姣佹椂鎶婃潗璐ㄥ湪 UnifiedDescriptorHeap Zone A
		// 占用的 5 槽块经 DeferredReleaseQueue 安全回收（内部自带 weak_ptr 存活令牌保护）。
		std::function<void()> m_ReleaseMaterialDescriptors;

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
			m_MeshConstantsGPU.Destroy();
		}

		ModelInstance& operator=(std::shared_ptr<const Model> sourceModel);

		bool IsNull(void) const { return m_Model == nullptr; }

		// R5锛氬彧璇昏闂簳灞?Model銆?
		// ------------------------------------------------------------------
		// 鍏夎拷璺緞闇€瑕侀亶鍘?Mesh 琛ㄦ潵寤?BLAS锛坢_NumMeshes / m_MeshData /
		// m_DataBuffer 閮芥槸 Model 鐨勫叕寮€鎴愬憳锛夛紝浣?ModelInstance 涓€鐩存妸
		// m_Model 钘忓湪 private 閲屻€傝繖閲屽彧鏆撮湶 const 鎸囬拡锛屼笉鏆撮湶鍙彉鍙ユ焺锛?
		// 淇濊瘉澶栭儴鏃犳硶缁曡繃 m_Locator 鍘绘敼瀹炰緥鐘舵€侊紙閭ｆ槸 R3/R4 韪╄繃鐨勫潙锛夈€?
		const Model* GetModel(void) const { return m_Model.get(); }

		// R5锛氬疄渚嬬殑 local-to-world 鐭╅樀锛堟牴绾у彉鎹級銆?
		Math::Matrix4 GetLocalToWorld(void) const { return Math::Matrix4((Math::AffineTransform)m_Locator); }

		// [TD-06c / 濂戠害 4锛氬厜鏍呬笌 DXR 鍏变韩鑺傜偣涓栫晫鐭╅樀鐪熸簮]
		// 按 Model::m_SceneGraph 层级树展平计算每个 GraphNode 的世界矩阵（m_Locator * node->xform 链）。
		// 光栅路径的 MeshConstants.World 与 DXR 的 TLAS InstanceTransform 消费同一份数组，
		// 彻底消除多节点/非单位局部变换模型在 Deferred 与 DXR 间的几何错位。
		const std::vector<Math::Matrix4>& GetNodeWorldMatrices(void) const { return m_NodeWorldMatrices; }
		Math::Matrix4 GetNodeWorldMatrix(uint32_t matrixIdx) const
		{
			if (matrixIdx < m_NodeWorldMatrices.size())
				return m_NodeWorldMatrices[matrixIdx];
			return GetLocalToWorld();
		}

		void Update(GraphicsContext& gfxContext, float deltaTime);
		void GatherRenderables(::Renderer::MeshSorter& sorter) const;

		void Resize(float newRadius);

		// R4.6锛氳缃?璇诲彇妯″瀷涓績鐨勪笘鐣屽潗鏍囦綅缃紙鏀寔 UI 閲屾憜鏀惧涓ā鍨嬶級銆?
		// ------------------------------------------------------------
		// 涓轰粈涔堟斁鍦?ModelInstance 鍐呴儴锛岃€屼笉鏄閮ㄧ洿鎺ユ敼鐭╅樀锛?
		//   m_Locator 鏄疄渚嬪唴閮ㄧ淮鎶ょ殑**鍞竴鍙樻崲鐪熸簮**锛孶pdate() 鐢ㄥ畠绠?
		//   MeshConstants銆傚閮ㄧ粫杩囧幓鏀圭煩闃碉紝涓嬩竴甯?Update 灏辫鐩栧洖鏉ヤ簡銆?
		//
		// 涓轰粈涔堝繀椤?MarkDirty()锛?
		//   Update() 鏈?IsDirty 闂ㄦ帶锛堣 Model.cpp锛夛紝涓嶇疆鑴忕殑璇濅綅缃敼鍔?
		//   涓嶄細閲嶆柊涓婁紶鍒?GPU 鈥斺€?杩欐槸 R3 鏈熼棿韪╄繃鐨勫潙
		//   锛圡odelInstance::operator= 蹇樼疆鑴忓鑷?璧嬩簡鍊间絾妯″瀷娌″姩"锛夈€?
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
		// [TD-06c] 閬嶅巻 m_Model->m_SceneGraph 鍒锋柊灞曞钩涓栫晫鐭╅樀鏁扮粍锛堟瀯閫?璧嬪€?绉诲姩/缂╂斁/Update 鏃剁淮鎶わ級銆?
		void ComputeNodeWorldMatrices();

		std::shared_ptr<const Model> m_Model;
		// [TD-04b] 绉婚櫎鎸佷箙 m_MeshConstantsCPU锛氭敼鍦?Update() 涓粡 CommandContext::ReserveUploadMemory
		// 浠庡抚鐬€?LinearAllocator 鍒嗛厤涓婁紶鍐呭瓨锛岀敱鎻愪氦 Fence 鑷姩閫€浼戯紝鏉滅粷璺ㄥ抚瑕嗗啓绔炴€併€?
		ByteAddressBuffer m_MeshConstantsGPU;
		std::vector<Math::Matrix4> m_NodeWorldMatrices;
		std::unique_ptr<Math::AffineTransform[]> m_BoundingSphereTransforms;
		Math::UniformTransform m_Locator;

		bool m_IsDirty = true;

	};

}

