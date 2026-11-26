#include "00_Core/PCH.h"
#include "04_Renderer/Renderer/RayTracing/RaytracingScene.h"

#include "02_RHI/GraphicsCore.h"
#include "02_RHI/Command/CommandContext.h"
//  R7锛氫负浜嗘寜 mesh.vertexFlags 鎺ㄥ UV0 鐨勫瓧鑺傚亸绉伙紝闇€瑕?Renderer::kVertex_* 浣嶆爣蹇椼€?
//  杩欎簺浣嶅畾涔夊湪 PipelineDesc.h 閲岋紙涓庡厜鏍?PSO 鐨勯《鐐瑰竷灞€鍏辩敤鍚屼竴浠界湡婧愶級銆?
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include "04_Renderer/UnifiedDescriptorHeap.h"
#include "05_Scene/Model/Model.h"

#include <algorithm>

using namespace Graphics;

// ============================================================================
//  R5 / D1锛欱LAS / TLAS / SBT 瀹炵幇
// ----------------------------------------------------------------------------
//  鏈枃浠跺彧鍋氥€屽姞閫熺粨鏋勩€嶏紝涓嶅惈浠讳綍缁樺埗閫昏緫銆?
// ============================================================================

namespace
{
	// D3D12 瑕佹眰 AS 缁撴灉涓?scratch 鐨勫湴鍧€鎸?256 瀛楄妭瀵归綈
	// 锛圖3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT锛夈€?
	constexpr uint64_t kASByteAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

	uint64_t AlignUp64(uint64_t value, uint64_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	// SBT 姣忎釜 section 鐨勮捣濮嬪湴鍧€瑕佹寜 D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT锛?4锛夊榻愶紝
	// 姣忔潯璁板綍 32 瀛楄妭锛圖3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES锛夈€?
	constexpr uint32_t kSbtRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
	constexpr uint32_t kSbtSectionAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64
}

// ============================================================================
//  RaytracingBuffer::CreateAccelerationStructure
// ----------------------------------------------------------------------------
//  涓轰粈涔堜笉鑳界洿鎺ョ敤 GpuBuffer::Create 鈥斺€?涓ゆ潯纭€х害鏉燂細
//    1) 鍔犻€熺粨鏋勮祫婧?*涓嶅厑璁?*鐢?ResourceBarrier 杞崲杩?
//       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE銆?
//       瀹冨繀椤诲湪 CreateCommittedResource 鐨?InitialState 鍙傛暟閲屽氨缁欏嚭璇ョ姸鎬併€?
//       鍚﹀垯 Debug Layer 鎶?
//         ERROR #526 RESOURCE_BARRIER_INVALID_COMBINATION
//       骞?break锛堝疄娴嬭繘绋嬩互 0x87a 寮傚父缁堟锛屾棩蹇楀垰濂藉仠鍦?RebuildGeometry锛夈€?
//    2) 璧勬簮鎻忚堪閲屽繀椤诲甫 D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE銆?
//  GpuBuffer::Create 鎶婅繖涓よ€呴兘鍐欐鎴?COMMON / ALLOW_UNORDERED_ACCESS锛?
//  鍥犳杩欓噷鍗曠嫭寮€涓€鏉″垱寤鸿矾寰勩€?
//
//  鍒涘缓鍚庤璧勬簮鍦ㄦ暣涓敓鍛藉懆鏈熷唴**涓嶅啀鍙戠敓浠讳綍鐘舵€佽浆鎹?* 鈥斺€?
//  BuildRaytracingAccelerationStructure 瑕佹眰瀹冨凡鏄?AS 鐘舵€侊紝
//  DispatchRays 浠?root SRV 鐩存帴璇诲彇瀹冿紙root SRV 涓嶉渶瑕佺姸鎬佽浆鎹級銆?
// ============================================================================
void Renderer::RayTracing::RaytracingBuffer::CreateAccelerationStructure(
	const std::wstring& name, uint64_t sizeInBytes)
{
	Destroy();

	m_ElementCount = 1;
	m_ElementSize = (uint32_t)sizeInBytes;
	m_BufferSize = (size_t)sizeInBytes;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = sizeInBytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	// 鏍囧織浣嶅繀椤绘槸 ALLOW_UNORDERED_ACCESS 鈥斺€?涓や釜閿欒閮藉疄娴嬭俯杩囷細
	//   (a) 鐢?D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE(0x100)锛?
	//       鏈満杩愯鏃舵槸 Windows 10 19041 鐨?D3D12锛?0.0.19041.5794锛夛紝
	//       涓嶈璇嗚浣嶏紝CreateCommittedResource 鎶?
	//         ERROR #719 CREATERESOURCE_UNRECOGNIZEDLAYOUT
	//         "Flags has unrecognized bits. The value is 0x100"
	//       璇ユ爣蹇楁槸 Agility SDK 1.7xx 涔嬪悗鎵嶅紩鍏ョ殑锛屼笉鏄?DXR 1.0 鐨勫繀闇€椤广€?
	//   (b) 鐢?D3D12_RESOURCE_FLAG_NONE锛氭姤
	//         ERROR #524 RESOURCE_BARRIER_MISMATCHING_MISC_FLAGS
	//         "D3D12_RESOURCE_STATES has bits that mismatch support
	//          required from D3D12_RESOURCE_FLAGS"
	//       鍗炽€屽垵濮嬬姸鎬佹槸 AS銆嶈繖浠朵簨瑕佹眰璧勬簮鍏峰 UAV 鑳藉姏銆?
	// 缁撹锛氫笌寰蒋瀹樻柟 D3D12 Raytracing 鏍蜂緥涓€鑷?鈥斺€?
	//       ALLOW_UNORDERED_ACCESS + 鍒濆鐘舵€?RAYTRACING_ACCELERATION_STRUCTURE銆?
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	// 鍏抽敭锛氬垵濮嬬姸鎬佺洿鎺ュ氨鏄?AS锛屼箣鍚庢案涓嶈浆鎹€?
	m_UsageState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

	ASSERT_SUCCEEDED(g_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
		&desc, m_UsageState, nullptr, TY_IID_PPV_ARGS(&m_pResource)));

	m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();

#ifdef RELEASE
	(name);
#else
	m_pResource->SetName(name.c_str());
#endif

	// AS 缂撳啿涓嶉渶瑕?SRV/UAV 鎻忚堪绗︼紙root SRV 鐩存帴缁?GPU 鍦板潃锛夈€?
	CreateDerivedViews();
}

// ============================================================================
//  鍑犱綍鑱氬悎
// ----------------------------------------------------------------------------
//  涓轰粈涔堣鑱氬悎锛欻LSL 鏃犳硶瑙ｅ紩鐢ㄣ€岃繍琛屾椂鎵嶇煡閬撶殑 GPU 铏氭嫙鍦板潃銆嶃€?
//  ClosestHit 鍙嬁鍒?InstanceID()/PrimitiveIndex()锛屽繀椤昏兘鎸夊亸绉荤储寮曞埌椤剁偣銆?
//  鍥犳鎶婃墍鏈夊疄渚嬬敤鍒扮殑妯″瀷鍑犱綍锛屽悇澶嶅埗涓€浠藉埌**涓€鍧?*澶?ByteAddressBuffer锛?
//  姣忎釜瀹炰緥璁板綍鑷繁閭ｄ竴娈电殑瀛楄妭鍋忕Щ銆?
//
//  涓轰粈涔堢敤 GPU鈫扜PU 澶嶅埗鑰屼笉鏄洖璇伙細
//    Mesh 鍏冩暟鎹紙vbOffset/vbSize/ibOffset/ibSize/vbStride/ibFormat/
//    draw.startIndex/baseVertex/primCount锛夊凡缁忓畬鏁存弿杩颁簡姣忎釜鍖烘鐨勪綅缃笌闀垮害锛?
//    鎴戜滑涓嶉渶瑕佺煡閬撻《鐐瑰瓧鑺傚唴瀹瑰氨鑳界畻鍋忕Щ銆傛墍浠ュ彧瑕?CopyBufferRegion 灏卞锛?
//    鐪佹帀涓€娆″悓姝ュ洖璇伙紙鍥炶蹇呴』 Finish(true) 绛?GPU锛屼唬浠峰ぇ涓旀墦鏂綍鍒讹級銆?
//
//  涓轰粈涔堟寜 Model 鎸囬拡鍘婚噸锛?
//    鍚屼竴涓ā鍨嬪彲浠ヨ澶氫釜 ModelInstance 鍏变韩锛圧endererDemoApp 鍏佽鍔犲涓疄渚嬶級銆?
//    鍑犱綍鏄潤鎬佺殑锛屽鍒朵竴浠藉嵆鍙紱涓嶅悓瀹炰緥鍙湪 ObjectToWorld 涓婁笉鍚屻€?
// ============================================================================
bool Renderer::RayTracing::RaytracingScene::RebuildGeometry(
	GraphicsContext& gfxContext,
	const std::vector<Scene::Model::ModelInstance*>& models)
{
	if (g_DeviceDXR == nullptr)
	{
		LOG_INFO(L"[DXR] RebuildGeometry 璺宠繃锛歡_DeviceDXR 涓虹┖锛堣澶囦笉鏀寔 DXR锛塡n");
		return false;
	}

	// [TD-04a / 鏃堕挓 1 瀹夊叏閾佸緥] 鑻ュ凡鏈夋棫鐨?BLAS/TLAS/Scratch 姝ｅ湪琚笂涓€甯х殑 DispatchRays
	// 浣跨敤锛堜緥濡傝繍琛屾湡鍔ㄦ€佸鍒犳ā鍨嬭Е鍙?RebuildGeometry锛夛紝閿€姣佸墠蹇呴』鍏堟帓绌?GPU 闃熷垪锛?
	// 闃叉瑙﹀彂 Debug Layer #921 OBJECT_DELETED_WHILE_STILL_IN_USE锛圵P-1A 鍚庡皢杩佽嚦 DeferredReleaseQueue锛夈€?
	if (!m_BlasBuffers.empty() || m_TopLevel != nullptr || m_Scratch != nullptr || m_TlasScratch != nullptr)
	{
		g_CommandManager.IdleGPU();
	}

	DestroyBlasBuffers();
	DestroySceneBuffers();

	// ------------------------------------------------------------------
	//  1. 鏀堕泦鍞竴妯″瀷锛岀畻姣忓潡鍑犱綍鍦ㄥ悎骞剁紦鍐查噷鐨勫熀鍧€
	// ------------------------------------------------------------------
	struct ModelPlacement
	{
		const Scene::Model::Model* Model;
		uint64_t BaseOffset;
		uint64_t ByteSize;
	};

	std::vector<ModelPlacement> placements;
	std::vector<Scene::Model::ModelInstance*> liveModels;

	uint64_t totalBytes = 0;
	for (Scene::Model::ModelInstance* instance : models)
	{
		if (instance == nullptr || instance->IsNull())
			continue;

		const Scene::Model::Model* model = instance->GetModel();
		if (model == nullptr || model->m_NumMeshes == 0)
			continue;

		const uint64_t size = model->m_DataBuffer.GetBufferSize();
		if (size == 0)
			continue;

		liveModels.push_back(instance);

		const bool alreadyPlaced = std::any_of(placements.begin(), placements.end(),
			[model](const ModelPlacement& p) { return p.Model == model; });
		if (alreadyPlaced)
			continue;

		// 姣忓潡鍑犱綍鎸?256 瀛楄妭瀵归綈锛屼繚璇?BLAS 鐨勯《鐐?绱㈠紩鍦板潃澶╃劧婊¤冻瀵归綈瑕佹眰銆?
		ModelPlacement placement;
		placement.Model = model;
		placement.BaseOffset = AlignUp64(totalBytes, kASByteAlignment);
		placement.ByteSize = size;
		placements.push_back(placement);
		totalBytes = placement.BaseOffset + size;
	}

	if (placements.empty())
	{
		LOG_INFO(L"[DXR] RebuildGeometry锛氬満鏅噷娌℃湁鍙敤鍑犱綍锛堟ā鍨嬫暟=%zu锛塡n", models.size());
		return false;
	}

	totalBytes = AlignUp64(totalBytes, kASByteAlignment);

	// ------------------------------------------------------------------
	//  2. 寤哄悎骞跺嚑浣曠紦鍐插苟閫愭ā鍨嬪鍒?
	// ------------------------------------------------------------------
	m_SceneGeometry = std::make_unique<RaytracingBuffer>();
	m_SceneGeometry->Create(L"Raytracing Scene Geometry", (uint32_t)totalBytes, 1);

	for (const ModelPlacement& placement : placements)
	{
		// 婧愮紦鍐叉槸妯″瀷鑷繁鐨?m_DataBuffer锛圖EFAULT 鍫嗭紝鍒涘缓鍚庝负 GENERIC_READ锛夈€?
		// CopyBufferRegion 鍐呴儴浼氫复鏃舵妸婧愬垏鍒?COPY_SOURCE 鍐嶆仮澶嶏紝瑙?CommandContext.cpp:647-660銆?
		Scene::Model::Model* mutableModel = const_cast<Scene::Model::Model*>(placement.Model);
		gfxContext.CopyBufferRegion(*m_SceneGeometry, (size_t)placement.BaseOffset,
			mutableModel->m_DataBuffer, 0, (size_t)placement.ByteSize);
	}

	// 鍚堝苟缂撳啿琚?BLAS 鏋勫缓涓庡懡涓潃鑹插櫒璇诲彇锛岀粺涓€鍋滃湪 NON_PIXEL_SHADER_RESOURCE銆?
	gfxContext.TransitionResource(*m_SceneGeometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// ------------------------------------------------------------------
	//  3. 閫?(瀹炰緥, mesh, draw) 寤哄崟鍑犱綍 BLAS 鎻忚堪
	//     鍚屾椂濉厖姣忓疄渚嬫暟鎹紙渚涘懡涓潃鑹插櫒绱㈠紩椤剁偣锛?
	// ------------------------------------------------------------------
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
	std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> blasInputs;
	std::vector<Renderer::RayTracing::RaytracingInstanceData> instanceData;

	geometries.reserve(64);
	blasInputs.reserve(64);
	instanceData.reserve(64);

	auto findBaseOffset = [&placements](const Scene::Model::Model* model) -> uint64_t
	{
		for (const ModelPlacement& p : placements)
			if (p.Model == model)
				return p.BaseOffset;
		return 0;
	};

	uint32_t triangleCount = 0;

	for (Scene::Model::ModelInstance* instance : liveModels)
	{
		const Scene::Model::Model* model = instance->GetModel();
		const uint64_t modelBase = findBaseOffset(model);
		const uint64_t geometryVA = m_SceneGeometry->GetGpuVirtualAddress();

		const uint8_t* pMesh = model->m_MeshData.get();
		for (uint32_t meshIndex = 0; meshIndex < model->m_NumMeshes; ++meshIndex)
		{
			const Scene::Model::Mesh& mesh = *(const Scene::Model::Mesh*)pMesh;

			const uint32_t indexStride = (mesh.ibFormat == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
			const uint32_t indexFormatCode = (mesh.ibFormat == DXGI_FORMAT_R32_UINT) ? 1u : 0u;
			const uint32_t vertexCount = mesh.vbStride > 0 ? (mesh.vbSize / mesh.vbStride) : 0u;

			for (uint16_t drawIndex = 0; drawIndex < mesh.numDraws; ++drawIndex)
			{
				const Scene::Model::Mesh::Draw& draw = mesh.draw[drawIndex];
				const uint32_t primitiveCount = draw.primCount / 3;
				if (primitiveCount == 0 || vertexCount == 0)
					continue;

				// 鎶?startIndex / baseVertex 鐑樼剻杩涘湴鍧€锛岃繖鏍锋瘡涓?BLAS 鏄嚜鍖呭惈鐨勶紝
				// 鍛戒腑鐫€鑹插櫒鍙渶 PrimitiveIndex() 灏辫兘瀹氫綅涓夎褰紙Tier 1.0 鏃?GeometryIndex锛夈€?
				const uint64_t indexAddress = geometryVA + modelBase + mesh.ibOffset
					+ (uint64_t)draw.startIndex * indexStride;
				const uint64_t vertexAddress = geometryVA + modelBase + mesh.vbOffset
					+ (uint64_t)draw.baseVertex * mesh.vbStride;

				D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
				geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				geometry.Triangles.Transform3x4 = 0;
				geometry.Triangles.IndexFormat = (DXGI_FORMAT)mesh.ibFormat;
				geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				geometry.Triangles.IndexCount = draw.primCount;
				geometry.Triangles.VertexCount = vertexCount;
				geometry.Triangles.IndexBuffer = indexAddress;
				geometry.Triangles.VertexBuffer.StartAddress = vertexAddress;
				geometry.Triangles.VertexBuffer.StrideInBytes = mesh.vbStride;
				geometries.push_back(geometry);

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
				inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				inputs.NumDescs = 1;
				// [TD-05a] 涓ョ鍦?push_back 寰幆鍐呭彇 &geometries.back()锛氬綋鎬?draw 鏁拌秴杩?reserve 瀹归噺瑙﹀彂
				// vector 鎵╁鎼鏃讹紝姝ゅ墠鎵€鏈?inputs.pGeometryDescs 鍏ㄩ儴娌︿负鎮┖鎸囬拡锛?
				// 姝ゅ鏆傜疆 nullptr锛屽緟鎵€鏈?geometries.push_back 缁撴潫鍚庣粺涓€鎸夌储寮曞洖濉€?
				inputs.pGeometryDescs = nullptr;
				blasInputs.push_back(inputs);

				Renderer::RayTracing::RaytracingInstanceData data = {};
				data.VertexOffset = (uint32_t)(modelBase + mesh.vbOffset + (uint64_t)draw.baseVertex * mesh.vbStride);
				data.IndexOffset = (uint32_t)(modelBase + mesh.ibOffset + (uint64_t)draw.startIndex * indexStride);
				data.VertexStride = mesh.vbStride;
				data.IndexFormat = indexFormatCode;
				data.StartIndex = 0;
				data.BaseVertex = 0;
				data.PrimitiveCount = primitiveCount;
				data.InstanceId = (uint32_t)instanceData.size();

				// ------------------------------------------------------------
				//  R7 / D4锛氭潗璐ㄤ笌 UV 瀹氫綅
				// ------------------------------------------------------------
				//  UV0 鐨勫瓧鑺傚亸绉荤敱椤剁偣甯冨眬**鎺ㄥ**锛屼笉棰濆瀛樺偍锛?
				//    POSITION  R32G32B32_FLOAT  @ 0   (12 B)
				//    NORMAL    R10G10B10A2_UNORM @ 12  ( 4 B)
				//    TANGENT   R10G10B10A2_UNORM @ 16  ( 4 B, 鍙€?
				//    TEXCOORD0 R16G16_FLOAT      @ 16 鎴?20 ( 4 B)
				//  瑙?Engine/03_AssetSystem/Assets/Importers/Mesh/MeshConvert.cpp 鐨?
				//  OutputElements 椤哄簭銆傚疄娴?merlin 鍏ㄩ儴 15 涓?mesh 閮芥槸
				//  vbStride=24 + vertexFlags=0x0f 鈫?UV0 @ 20銆?
				//  娌℃湁 UV0 鐨?mesh 鏍囪涓?0xFFFFFFFF锛屽懡涓潃鑹插櫒閫€鍥炵函鍥犲瓙鑹层€?
				const bool hasTangent = (mesh.vertexFlags & Renderer::kVertex_Tangent) != 0;
				const bool hasUV0 = (mesh.vertexFlags & Renderer::kVertex_UV0) != 0;
				data.UvByteOffset = hasUV0 ? (uint32_t)(12 + 4 + (hasTangent ? 4 : 0)) : 0xFFFFFFFFu;
				data.VertexFlags = (uint32_t)mesh.vertexFlags;

				//  鏉愯川锛歮esh 鍙瓨 materialSlotIdx锛岀湡姝ｇ殑鎻忚堪绗﹀亸绉讳笌鏍囬噺閮借
				//  浠?model 渚у彇銆傛敞鎰?Mesh::srvTable 鏄?*姝诲瓧娈?*锛堝叏浠撻浂璧嬪€硷紝
				//  Debug 涓嬪疄娴嬫亽涓?0xCDCD锛夛紝涓嶈鐢ㄥ畠銆?
				const uint32_t materialIndex = (uint32_t)mesh.materialSlotIdx;
				data.MaterialIndex = materialIndex;
				// [WP-1D / TD-06f] 鏃犳晥鏉愯川鏄惧紡鎸囧悜 UnifiedDescriptorHeap Zone A 涓嶅彲鍙樺摠鍏垫Ы 0锛坘SentinelWhite2D锛夛細
				// 鏃犻渶鍦ㄧ儹璺緞 RayTracing.hlsl 澧炲姞鍒嗘敮鍗冲彲闆惰秺鐣岄噰鏍峰埌 (1,1,1,1)锛屼娇 albedo *= 1 閫€鍖栦负绾?baseColorFactor 鐧芥ā銆?
				data.SrvTableIndex = Renderer::UnifiedDescriptorHeap::kSentinelWhite2D;
				if (materialIndex < model->m_Materials.size())
				{
					const Scene::Material::Material& material = model->m_Materials[materialIndex];
					if (material.SRVTable != Renderer::kInvalidBindlessIndex)
						data.SrvTableIndex = material.SRVTable;
				}
				if (materialIndex < model->m_MaterialConstantData.size())
				{
					const Scene::Material::MaterialConstantData& constants = model->m_MaterialConstantData[materialIndex];
					data.BaseColorFactor[0] = constants.baseColorFactor[0];
					data.BaseColorFactor[1] = constants.baseColorFactor[1];
					data.BaseColorFactor[2] = constants.baseColorFactor[2];
					data.BaseColorFactor[3] = constants.baseColorFactor[3];
					data.EmissiveFactor[0] = constants.emissiveFactor[0];
					data.EmissiveFactor[1] = constants.emissiveFactor[1];
					data.EmissiveFactor[2] = constants.emissiveFactor[2];
					data.NormalTextureScale = constants.normalTextureScale;
					data.MetallicFactor = constants.metallicFactor;
					data.RoughnessFactor = constants.roughnessFactor;
					data.MaterialFlags = constants.flags;
				}
				else
				{
					//  娌℃湁 CPU 鏉愯川鍓湰鏃堕€€鍖栦负鐧芥ā锛堜笌 R6 瑙傛劅涓€鑷达級锛?
					//  閬垮厤姣忓疄渚嬫暟鎹噷鍑虹幇鏈垵濮嬪寲鐨?0锛堝叏榛戞ā鍨嬶級銆?
					data.BaseColorFactor[0] = data.BaseColorFactor[1] = data.BaseColorFactor[2] = data.BaseColorFactor[3] = 1.0f;
					data.NormalTextureScale = 1.0f;
					data.MetallicFactor = 0.0f;
					data.RoughnessFactor = 0.5f;
				}

				instanceData.push_back(data);

				triangleCount += primitiveCount;
			}

			pMesh += sizeof(Scene::Model::Mesh) + (mesh.numDraws - 1) * sizeof(Scene::Model::Mesh::Draw);
		}
	}

	if (blasInputs.empty())
	{
		LOG_INFO(L"[DXR] RebuildGeometry锛氭ā鍨嬪瓨鍦ㄤ絾娌℃湁鍙瀯寤虹殑涓夎褰紙涓夎鏁?0锛塡n");
		return false;
	}

	// [TD-05a] 鎵€鏈?geometries.push_back 宸插畬鎴愩€佸簳灞傚唴瀛樺湴鍧€涓嶅啀鍙樺姩锛屾鏃舵寜绱㈠紩鍥炲～ pGeometryDescs銆?
	ASSERT(blasInputs.size() == geometries.size());
	for (size_t i = 0; i < blasInputs.size(); ++i)
	{
		blasInputs[i].pGeometryDescs = &geometries[i];
	}

	// ------------------------------------------------------------------
	//  4. 鍒嗛厤 BLAS 缁撴灉缂撳啿涓?scratch
	//     scratch 鍚勬浜掍笉閲嶅彔 鈥斺€?鍚屼竴鏉″懡浠ゅ垪琛ㄩ噷杩炵画 Build 鏃犻渶涓棿 UAV barrier
	//     锛堥噸鍙犳墠鏈夊啓鍚庡啓鍐茬獊锛夈€?
	// ------------------------------------------------------------------
	m_BlasBuffers.resize(blasInputs.size());
	m_BlasScratchOffsets.resize(blasInputs.size());

	uint64_t scratchCursor = 0;
	for (size_t i = 0; i < blasInputs.size(); ++i)
	{
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		g_DeviceDXR->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs[i], &info);

		m_BlasBuffers[i] = std::make_unique<RaytracingBuffer>();
		m_BlasBuffers[i]->CreateAccelerationStructure(L"Raytracing BLAS", info.ResultDataMaxSizeInBytes);

		scratchCursor = AlignUp64(scratchCursor, kASByteAlignment);
		m_BlasScratchOffsets[i] = scratchCursor;
		scratchCursor += AlignUp64(info.ScratchDataSizeInBytes, kASByteAlignment);
	}

	// ------------------------------------------------------------------
	//  5. 鎻愪氦 BLAS 鏋勫缓
	// ------------------------------------------------------------------
	//  娉ㄦ剰锛氳繖閲?*娌℃湁**瀵?BLAS 缁撴灉缂撳啿鍋氫换浣?TransitionResource銆?
	//  AS 璧勬簮鍦?CreateAccelerationStructure 閲屽氨宸茬粡鏄?
	//  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE锛?
	//  鑰?D3D12 绂佹鐢?ResourceBarrier 鎶婅祫婧?杞崲杩?璇ョ姸鎬?
	//  锛圖ebug Layer ERROR #526锛夈€傝瑙?RaytracingBuffer::CreateAccelerationStructure銆?
	m_Scratch = std::make_unique<RaytracingBuffer>();
	// scratch 鍏堟寜涓存椂澶у皬寤猴紝鍚庨潰 TLAS 闇€瑕佹洿澶氭椂鍐嶉噸寤猴紙瑙佷笅锛夈€?
	m_Scratch->Create(L"Raytracing Scratch", (uint32_t)std::max<uint64_t>(scratchCursor, 256), 1);
	gfxContext.TransitionResource(*m_Scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 鍏抽敭锛欱uildRaytracingAccelerationStructure **涓嶄細**鍍?DrawInstanced 閭ｆ牱
	// 鑷姩鎻愪氦鎸傝捣鐨勫睆闅溿€俆ransitionResource 鍙槸鎶婂睆闅滃帇杩?
	// m_ResourceBarrierBuffer[16]锛屼笉鍒?16 涓氨涓?flush銆傝嫢杩欓噷涓嶆墜鍔?flush锛?
	// 鍚堝苟鍑犱綍缂撳啿杩樺仠鍦?COPY_DEST锛圕opyBufferRegion 鐣欎笅鐨勶級锛?
	// Build 浼氱洿鎺ユ姤
	//   ERROR #538 INVALID_SUBRESOURCE_STATE:
	//   "'Raytracing Scene Geometry' ... is invalid for use as a
	//    acceleration structure geometry vertex/index data.
	//    Expected: NON_PIXEL_SHADER_RESOURCE, Actual: COPY_DEST"
	// 骞?break锛堝疄娴嬭繘绋?0x87a 寮傚父缁堟锛夈€?
	gfxContext.FlushResourceBarriers();

	for (size_t i = 0; i < blasInputs.size(); ++i)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
		buildDesc.Inputs = blasInputs[i];
		buildDesc.ScratchAccelerationStructureData = m_Scratch->GetGpuVirtualAddress() + m_BlasScratchOffsets[i];
		buildDesc.DestAccelerationStructureData = m_BlasBuffers[i]->GetGpuVirtualAddress();
		buildDesc.SourceAccelerationStructureData = 0;
		gfxContext.GetCommandList4()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	}

	// [TD-04a / BLAS -> TLAS 鍚屾閾佸緥]
	// 鍚屼竴鍛戒护鍒楄〃闅忓悗绔嬪嵆鎵ц UpdateInstances -> RebuildTopLevel 鏋勫缓 TLAS 骞惰鍙栧叏閮?BLAS銆?
	// 鍔犻€熺粨鏋勬案杩滀笉鍙?Transition锛屽繀椤婚€氳繃 UAV 灞忛殰纭繚鍏ㄩ儴 BLAS 鍐欏叆瀵瑰悗缁?TLAS Build 鍙锛?
	// 锛堥厤鍚?TD-04e锛欼nsertUAVBarrier 婊?16 椤硅嚜鍔?flush锛屼换鎰忔暟閲?BLAS 鍧囦笉浼氳秺鐣屻€傦級
	for (size_t i = 0; i < m_BlasBuffers.size(); ++i)
	{
		gfxContext.InsertUAVBarrier(*m_BlasBuffers[i]);
	}
	gfxContext.FlushResourceBarriers();

	// ------------------------------------------------------------------
	//  6. 姣忓疄渚嬫暟鎹?鈫?GPU锛坮aw buffer锛孯7 璧锋瘡瀹炰緥 96 瀛楄妭 = 24 涓?DWORD锛?
	//     HLSL 渚х敤 ByteAddressBuffer + 鎵嬬畻鍋忕Щ璇诲彇锛堟牴绾?SRV 涓嶅甫
	//     StructureByteStride锛屽０鏄庢垚 StructuredBuffer<T> 浼氳閿欒法璺濓級銆?
	// ------------------------------------------------------------------
	const uint32_t instanceBytes = (uint32_t)(instanceData.size() * sizeof(Renderer::RayTracing::RaytracingInstanceData));
	m_InstanceDataBuffer = std::make_unique<RaytracingBuffer>();
	m_InstanceDataBuffer->Create(L"Raytracing Instance Data", instanceBytes, 1);

	// 娉ㄦ剰锛欳ommandContext::WriteBuffer 鍐呴儴鏈?ASSERT(Math::IsAligned(BufferData, 16))锛?
	// 鑰?std::vector<T>::data() 鍙繚璇?alignof(T)锛堣繖閲屾槸 4锛夊榻愶紝涓嶈兘渚濊禆銆?
	// 鍥犳璧?UploadBuffer + Map 鐨勮矾寰勩€備笂浼犵紦鍐插繀椤绘槸鎴愬憳锛堟椿鍒?GPU 鎵ц瀹岋級锛?
	// 涓嶈兘鏄眬閮ㄥ彉閲?鈥斺€?鍚﹀垯鍛戒护鍒楄〃杩樻病鎻愪氦锛屾簮璧勬簮灏辫閿€姣佷簡銆?
	// [TD-04b] 璧?CommandContext::ReserveUploadMemory 甯х灛鎬佺嚎鎬т笂浼犲尯锛堟寜鎻愪氦 Fence 鑷姩閫€浼戯級锛?
	// 涓嶅啀鎸佹湁鎸佷箙 UploadBuffer銆?
	DynAlloc instanceUpload = gfxContext.ReserveUploadMemory(instanceBytes);
	std::memcpy(instanceUpload.DataPtr, instanceData.data(), instanceBytes);
	gfxContext.CopyBufferRegion(*m_InstanceDataBuffer, 0, instanceUpload.Buffer, instanceUpload.Offset, instanceBytes);
	gfxContext.TransitionResource(*m_InstanceDataBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_InstanceCount = (uint32_t)instanceData.size();
	m_TriangleCount = triangleCount;
	m_TopLevelBuilt = false;

	LOG_INFO(L"[DXR] RebuildGeometry 瀹屾垚锛氭ā鍨?%zu 鍞竴鍑犱綍=%zu BLAS=%zu 涓夎=%u scratch=%llu B\n",
		liveModels.size(), placements.size(), m_BlasBuffers.size(), triangleCount,
		(unsigned long long)scratchCursor);

	return true;
}

// ============================================================================
//  姣忓抚鏇存柊瀹炰緥鍙樻崲 + 閲嶅缓 TLAS
// ----------------------------------------------------------------------------
//  BLAS 鍙湪鍑犱綍鍙樺寲鏃堕噸寤猴紙RebuildGeometry锛夛紱
//  杩欓噷姣忓抚鍙敼 TLAS 鐨?instance desc 鈥斺€?杩欏氨鏄矾绾垮浘 D1 瑕佹眰鐨?
//  銆岄潤鎬佸嚑浣曞鐢?BLAS锛屽彧鍦ㄥ疄渚嬪彉鎹㈠彉鍖栨椂鏇存柊 TLAS銆嶃€?
// ============================================================================
void Renderer::RayTracing::RaytracingScene::UpdateInstances(
	GraphicsContext& gfxContext,
	const std::vector<Scene::Model::ModelInstance*>& models)
{
	// 娉ㄦ剰锛氳繖閲?*涓嶈兘**鐢?IsValid() 褰撳墠缃潯浠?鈥斺€?IsValid() 瑕佹眰
	// m_TopLevel != nullptr锛岃€?m_TopLevel 姝ｆ槸鏈嚱鏁帮紙缁忕敱 RebuildTopLevel锛?
	// 绗竴娆″垱寤虹殑銆傜敤 IsValid() 浼氬鑷淬€岄甯ф案杩滃缓涓嶅嚭 TLAS銆嶇殑楦＄敓铔嬮棶棰橈紝
	// 琛ㄧ幇涓?Render 閲?GetTopLevelBuffer() 杩斿洖 nullptr 鍚庤В寮曠敤宕╂簝
	// 锛堝疄娴?c0000005 @ GpuResource::GetGpuVirtualAddress锛夈€?
	// 鍞竴鐪熸闇€瑕佺殑鍓嶇疆鏉′欢鏄?BLAS 宸茬粡寤哄ソ锛堟瘡瀹炰緥鏁版嵁涔熼殢 BLAS 涓€璧峰缓锛夈€?
	if (m_BlasBuffers.empty() || m_InstanceDataBuffer == nullptr)
		return;

	// 鎸夈€屽疄渚?脳 mesh 脳 draw銆嶇殑椤哄簭閲嶅缓 instance desc锛?
	// 椤哄簭蹇呴』涓?RebuildGeometry 閲屽～鍏?instanceData 鐨勯『搴忓畬鍏ㄤ竴鑷达紝
	// 鍚﹀垯 InstanceID 浼氭寚鍚戦敊璇殑鍑犱綍锛堣〃鐜颁负娉曠嚎棰滆壊瀵逛笉涓婃ā鍨嬶級銆?
	uint32_t cursor = 0;
	uint32_t instanceIndex = 0;

	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
	descs.reserve(m_InstanceCount);

	for (Scene::Model::ModelInstance* instance : models)
	{
		if (instance == nullptr || instance->IsNull())
			continue;

		const Scene::Model::Model* model = instance->GetModel();
		if (model == nullptr || model->m_NumMeshes == 0)
			continue;

		// 涓?RebuildGeometry 鐩稿悓锛氱┖鍑犱綍鐨勬ā鍨嬪湪閭ｈ竟琚烦杩囷紝杩欓噷涔熻璺宠繃銆?
		if (model->m_DataBuffer.GetBufferSize() == 0)
			continue;

		const uint8_t* pMesh = model->m_MeshData.get();
		for (uint32_t meshIndex = 0; meshIndex < model->m_NumMeshes; ++meshIndex)
		{
			const Scene::Model::Mesh& mesh = *(const Scene::Model::Mesh*)pMesh;
			const uint32_t vertexCount = mesh.vbStride > 0 ? (mesh.vbSize / mesh.vbStride) : 0u;

			// [TD-06c / 鍏夋爡涓?DXR 鍑犱綍瀵归綈涓嶅彉閲廬
			// 蹇呴』娑堣垂灞曞钩鍚庣殑 GraphNode 涓栫晫鐭╅樀锛坢esh.meshCBV 鍗冲搴?GraphNode::matrixIdx锛夛紝
			// 涓庡厜鏍呰矾寰?MeshConstants.World 瀹屽叏鍚屾簮銆傚師浠ｇ爜浠呭彇鏍圭骇 GetLocalToWorld()锛?
			// 涓㈠け浜嗘墍鏈夐潪鍗曚綅灞€閮ㄨ妭鐐瑰彉鎹紙骞崇Щ/鏃嬭浆/缂╂斁锛夛紝瀵艰嚧澶氳妭鐐规ā鍨嬪湪 DXR 涓弗閲嶉敊浣嶃€?
			const Math::Matrix4 nodeWorld = instance->GetNodeWorldMatrix(mesh.meshCBV);
			XMFLOAT4X4 rows;
			XMStoreFloat4x4(&rows, nodeWorld);

			for (uint16_t drawIndex = 0; drawIndex < mesh.numDraws; ++drawIndex)
			{
				const uint32_t primitiveCount = mesh.draw[drawIndex].primCount / 3;
				if (primitiveCount == 0 || vertexCount == 0)
					continue;

				if (cursor >= m_BlasBuffers.size())
					break;

				D3D12_RAYTRACING_INSTANCE_DESC desc = {};
				// ------------------------------------------------------------
				// 銆愬叧閿€慏XR 鐨勫疄渚嬪彉鎹㈡槸**鍒楀悜閲?*绾﹀畾锛?
				//   worldPos = mul(InstanceTransform, localPos)
				// 鍏朵腑 Transform 鏄?3x4 琛屼富搴忕煩闃碉紝**骞崇Щ鍦ㄧ 3 鍒?*
				// 锛圱ransform[i][3] = 骞崇Щ鍒嗛噺锛夛紝绾挎€ч儴鍒?Transform[i][j]銆?
				//
				// 鑰屾湰椤圭洰鐨?Math::Matrix4 鏄?*琛屽悜閲?*绾﹀畾锛坴 * M锛夛紝
				// 骞崇Щ鍦ㄧ 3 琛岋紙m[3][0..2]锛夈€備袱鑰呬簰涓鸿浆缃細
				//   Transform[i][j] = M.m[j][i]   (j < 3)
				//   Transform[i][3] = M.m[3][i]
				//
				// 鏇剧粡杩欓噷鐩存帴鎸夎鎷疯礉锛圱ransform[i][j] = m[i][j]锛夛紝鍚庢灉鏄?
				// **骞崇Щ琚暣琛屼涪鎺?*锛坢[i][3] 瀵逛豢灏勭煩闃垫亽涓?0锛夛紝妯″瀷鍋滅暀鍦?
				// 鏈钩绉荤殑灞€閮ㄥ潗鏍囩郴閲屻€傝〃鐜版瀬鍏锋楠楁€э細BLAS/TLAS 鍏ㄩ儴鏋勫缓鎴愬姛銆?
				// Debug Layer 闆舵姤閿欍€丏ispatchRays 鐓у父鎻愪氦銆丷ayGen 涔熺‘瀹炲湪鍐?UAV
				// 锛堟暣灞忔槸 miss 鑳屾櫙鑹诧級锛屼絾鎵€鏈夊皠绾块兘鎵撲笉鍒版ā鍨?鈥斺€?鍥犱负妯″瀷
				// 鏍规湰涓嶅湪鐩告満鐪嬬殑閭ｄ釜浣嶇疆锛堝疄娴嬭俯杩囷紝鎺掓煡浜嗗緢涔咃級銆?
				// ------------------------------------------------------------
				desc.Transform[0][0] = rows.m[0][0];
				desc.Transform[0][1] = rows.m[1][0];
				desc.Transform[0][2] = rows.m[2][0];
				desc.Transform[0][3] = rows.m[3][0];
				desc.Transform[1][0] = rows.m[0][1];
				desc.Transform[1][1] = rows.m[1][1];
				desc.Transform[1][2] = rows.m[2][1];
				desc.Transform[1][3] = rows.m[3][1];
				desc.Transform[2][0] = rows.m[0][2];
				desc.Transform[2][1] = rows.m[1][2];
				desc.Transform[2][2] = rows.m[2][2];
				desc.Transform[2][3] = rows.m[3][2];

				desc.InstanceID = cursor;             // == instanceData 鐨勪笅鏍?
				desc.InstanceMask = 0xFF;
				desc.InstanceContributionToHitGroupIndex = 0;
				// 鍙岄潰锛欳ULL_DISABLE銆傚懡涓潃鑹插櫒閲屽凡鎸夊叆灏勬柟鍚戠炕杞硶绾匡紝
				// 鍥犳浠庤儗闈㈢湅涔熸槸鍚屼竴绉嶇潃鑹茬粨鏋滐紝涓嶄細鍑虹幇鏁寸墖姝婚粦銆?
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				desc.AccelerationStructure = m_BlasBuffers[cursor]->GetGpuVirtualAddress();

				descs.push_back(desc);
				++cursor;
				++instanceIndex;
			}

			pMesh += sizeof(Scene::Model::Mesh) + (mesh.numDraws - 1) * sizeof(Scene::Model::Mesh::Draw);
		}
	}

	(void)instanceIndex;

	if (descs.empty())
		return;

	// [TD-04b / 娑堥櫎璺ㄥ抚鎸佷箙 UploadBuffer 瑕嗗啓]
	// 姣忓抚浣跨敤 CommandContext::ReserveUploadMemory 浠?2 MiB UPLOAD 绾挎€ч〉鍒嗛厤鏈鏋勫缓涓撶敤鐨?
	// InstanceDescs锛堥粯璁?256B 瀵归綈锛屾弧瓒?D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT = 16锛夛紝
	// 椤电敓鍛藉懆鏈熷彈鍛戒护鍒楄〃鎻愪氦 Fence 淇濇姢锛岀粷涓嶄細瑕嗗啓涓婁竴甯у湪閫?TLAS Build 姝ｅ湪璇诲彇鐨勬弿杩扮銆?
	const uint32_t bytes = (uint32_t)(descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
	DynAlloc descUpload = gfxContext.ReserveUploadMemory(bytes);
	std::memcpy(descUpload.DataPtr, descs.data(), bytes);

	RebuildTopLevel(gfxContext, descUpload.GpuAddress, static_cast<uint32_t>(descs.size()));
}

void Renderer::RayTracing::RaytracingScene::RebuildTopLevel(
	GraphicsContext& gfxContext,
	D3D12_GPU_VIRTUAL_ADDRESS instanceDescsGpuAddress,
	uint32_t instanceCount)
{
	if (g_DeviceDXR == nullptr || m_BlasBuffers.empty() || instanceCount == 0 || instanceDescsGpuAddress == 0)
		return;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = instanceCount;
	inputs.InstanceDescs = instanceDescsGpuAddress;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	g_DeviceDXR->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

	// TLAS 缁撴灉缂撳啿灏哄鍙樹簡灏遍噸寤猴紙绗竴甯ф垨瀹炰緥鏁板彉鍖栵級銆?
	if (m_TopLevel == nullptr || m_TopLevel->GetBufferSize() < info.ResultDataMaxSizeInBytes)
	{
		if (m_TopLevel != nullptr && m_TopLevelBuilt)
			g_CommandManager.IdleGPU();
		m_TopLevel = std::make_unique<RaytracingBuffer>();
		m_TopLevel->CreateAccelerationStructure(L"Raytracing TLAS", info.ResultDataMaxSizeInBytes);
		m_TopLevelBuilt = false;
	}

	// scratch 闇€瑕佸绾?TLAS 閭ｄ竴娈点€侭LAS 娈靛凡鍦?RebuildGeometry 鏃剁敤瀹岋紝
	// 杩欓噷鍙互鎶?TLAS 娈电洿鎺ユ斁鍦?0 鍙蜂綅缃紙涓よ€呬笉鍚屾椂浣跨敤锛夈€?
	// [TD-04a] 浣跨敤涓?BLAS 瀹屽叏鐙珛鐨?m_TlasScratch锛屼弗绂佸鐢?BLAS 鐨?m_Scratch锛?
	// 鍚屽懡浠ゅ垪琛ㄥ唴 RebuildGeometry 鍒氬彂瀹?BLAS Build锛岄殢鍚?RebuildTopLevel 鑻ヨ鍐欐垨閲嶅缓
	// m_Scratch 浼氶€犳垚鍐欏悗鍐欏啿绐佷箖鑷抽攢姣佸湪閫?Scratch锛圖ebug Layer #921锛夈€?
	const uint64_t tlasScratchSize = AlignUp64((std::max)(static_cast<uint64_t>(info.ScratchDataSizeInBytes), static_cast<uint64_t>(256)), kASByteAlignment);
	if (m_TlasScratch == nullptr || m_TlasScratch->GetBufferSize() < tlasScratchSize)
	{
		if (m_TlasScratch != nullptr && m_TopLevelBuilt)
			g_CommandManager.IdleGPU();
		m_TlasScratch = std::make_unique<RaytracingBuffer>();
		m_TlasScratch->Create(L"Raytracing TLAS Scratch", (uint32_t)tlasScratchSize, 1);
		gfxContext.TransitionResource(*m_TlasScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// TLAS 琚噸寤?/ 閲嶅鏋勫缓 鈫?涓婁竴甯х殑 Build 鎴?DispatchRays 鍙兘浠嶅湪璇诲畠銆?
	// 杩欐槸璺嚎鍥?D1銆屾槑纭?AS 杈撳嚭 buffer 鐨?UAV barrier銆嶇殑钀界偣銆?
	// UAV barrier 鏄悎娉曠殑锛堝畠涓嶆槸銆岀姸鎬佽浆鎹€嶏紝涓嶈Е鍙?#526锛夈€?
	if (m_TopLevelBuilt)
	{
		gfxContext.InsertUAVBarrier(*m_TopLevel);
		gfxContext.InsertUAVBarrier(*m_TlasScratch);
	}

	// 鍚屾牱**娌℃湁** TransitionResource锛歍LAS 鍒涘缓鏃跺嵆涓?AS 鐘舵€併€?

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs = inputs;
	buildDesc.ScratchAccelerationStructureData = m_TlasScratch->GetGpuVirtualAddress();
	buildDesc.DestAccelerationStructureData = m_TopLevel->GetGpuVirtualAddress();
	buildDesc.SourceAccelerationStructureData = 0;
	// 鍚?BLAS锛欱uild 涓嶄細鑷姩鎻愪氦鎸傝捣鐨勫睆闅滐紙scratch 鐨?UNORDERED_ACCESS
	// 杞崲銆乮nstance desc 鐨?COPY_DEST 绛夐兘鍙兘杩樺帇鍦ㄧ紦鍐查噷锛夈€?
	gfxContext.FlushResourceBarriers();
	gfxContext.GetCommandList4()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	// [TD-04a] TLAS 鏋勫缓瀹屾垚鍚庣珛鍒绘彃鍏?UAV 灞忛殰锛岀‘淇濋殢鍚?DispatchRays 璇诲埌宸叉彁浜ゅ畬鎴愮殑瀹屾暣 TLAS銆?
	gfxContext.InsertUAVBarrier(*m_TopLevel, true);

	m_TopLevelBuilt = true;

	// DispatchRays 鐨勫昂瀵稿湪 Renderer 閲屾瘡甯у～锛岃繖閲屽彧淇濊瘉鍦板潃姝ｇ‘銆?
	m_DispatchRaysDesc.Width = 0;
	m_DispatchRaysDesc.Height = 0;
	m_DispatchRaysDesc.Depth = 1;
}

// ============================================================================
//  Shader Binding Table
// ----------------------------------------------------------------------------
//  甯冨眬锛堟瘡 section 璧风偣鎸?64 瀛楄妭瀵归綈锛岃褰?32 瀛楄妭锛夛細
//    [0]   RayGeneration  32 B
//    [64]  Miss[0]        32 B   鈫?涓诲皠绾?/ 鍙嶅皠灏勭嚎鎵撶┖
//    [96]  Miss[1]        32 B   鈫?闃村奖灏勭嚎鎵撶┖锛圫hadowMiss锛?
//    [128] HitGroup       32 B
//  鎬?192 B銆?
//
//  娉ㄦ剰 miss 琛ㄦ槸**杩炵画涓ゆ潯璁板綍**锛歔kMissOffset, kMissOffset + 2*32)锛?
//  SizeInBytes = 64銆丼trideInBytes = 32銆俆raceRay 鐨勭 6 涓疄鍙?
//  MissShaderIndex 灏辨槸杩欏紶琛ㄧ殑涓嬫爣锛? 鈫?Miss锛? 鈫?ShadowMiss锛夛紝椤哄簭鍗冲绾︺€?
// ============================================================================
bool Renderer::RayTracing::RaytracingScene::BuildShaderBindingTable(
	ID3D12StateObjectProperties* stateObjectProperties,
	const wchar_t* rayGenExport, const wchar_t* missExport,
	const wchar_t* shadowMissExport, const wchar_t* hitGroupName)
{
	if (stateObjectProperties == nullptr)
	{
		LOG_INFO(L"[DXR] BuildShaderBindingTable 澶辫触锛歋tateObjectProperties 涓虹┖\n");
		return false;
	}

	constexpr uint32_t kRayGenOffset = 0;
	constexpr uint32_t kMissOffset = kRayGenOffset + kSbtSectionAlignment;       // 64
	constexpr uint32_t kShadowMissOffset = kMissOffset + kSbtRecordSize;         // 96
	constexpr uint32_t kHitGroupOffset = kMissOffset + kSbtSectionAlignment * 2; // 128
	constexpr uint32_t kTotalBytes = kHitGroupOffset + kSbtSectionAlignment;     // 192

	// [TD-03b] 鍚屾妫€鏌?GetResource() == nullptr锛岄厤鍚?UploadBuffer::Destroy() 褰掗浂 m_BufferSize
	// 褰诲簳鏉滅粷 DestroySceneBuffers() 鍚庝簩娆?BuildShaderBindingTable 璺宠繃閲嶅缓骞剁┖鎸囬拡 Map 宕╂簝銆?
	if (m_SbtUpload.GetResource() == nullptr || m_SbtUpload.GetBufferSize() != kTotalBytes)
		m_SbtUpload.Create(L"Raytracing SBT", kTotalBytes);

	uint8_t* sbt = (uint8_t*)m_SbtUpload.Map();
	memset(sbt, 0, kTotalBytes);

	void* rayGenId = stateObjectProperties->GetShaderIdentifier(rayGenExport);
	if (rayGenId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 澶辫触锛氬鍑?%s 娌℃湁鐫€鑹插櫒鏍囪瘑绗n", rayGenExport);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kRayGenOffset, rayGenId, kSbtRecordSize);

	void* missId = stateObjectProperties->GetShaderIdentifier(missExport);
	if (missId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 澶辫触锛氬鍑?%s 娌℃湁鐫€鑹插櫒鏍囪瘑绗n", missExport);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kMissOffset, missId, kSbtRecordSize);

	// 闃村奖灏勭嚎涓撶敤鐨?miss銆傛嬁涓嶅埌鏍囪瘑绗︽剰鍛崇潃 State Object 鐨?Exports 閲?
	// 婕忎簡瀹?鈥斺€?杩欑鎯呭喌涓?*涓嶈兘**闈欓粯闄嶇骇锛堜緥濡傛妸 index 1 涔熷～鎴愪富 Miss锛夛紝
	// 鍥犱负涓?Miss 鐨勮浇鑽风被鍨嬫槸 RayPayload(32B)锛岃褰?ShadowPayload(8B) 瑙ｉ噴
	// 浼氳秺鐣屽啓銆傚繀椤荤洿鎺ュけ璐ワ紝璁╅棶棰樺湪鍚姩鏃ュ織閲屾毚闇层€?
	void* shadowMissId = stateObjectProperties->GetShaderIdentifier(shadowMissExport);
	if (shadowMissId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 失败：导出 %s 没有着色器标识符"
			L"锛堟鏌?RayTracingRenderer::BuildStateObject 鐨?desc.Exports锛塡n",
			shadowMissExport);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kShadowMissOffset, shadowMissId, kSbtRecordSize);

	void* hitGroupId = stateObjectProperties->GetShaderIdentifier(hitGroupName);
	if (hitGroupId == nullptr)
	{
		LOG_INFO(L"[DXR] SBT 澶辫触锛氬懡涓粍 %s 娌℃湁鐫€鑹插櫒鏍囪瘑绗n", hitGroupName);
		m_SbtUpload.Unmap(0, kTotalBytes);
		return false;
	}
	memcpy(sbt + kHitGroupOffset, hitGroupId, kSbtRecordSize);

	m_SbtUpload.Unmap(0, kTotalBytes);

	const D3D12_GPU_VIRTUAL_ADDRESS base = m_SbtUpload.GetGpuVirtualAddress();
	m_DispatchRaysDesc.RayGenerationShaderRecord.StartAddress = base + kRayGenOffset;
	m_DispatchRaysDesc.RayGenerationShaderRecord.SizeInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.MissShaderTable.StartAddress = base + kMissOffset;
	m_DispatchRaysDesc.MissShaderTable.SizeInBytes = kSbtRecordSize * 2;   // 涓ゆ潯璁板綍
	m_DispatchRaysDesc.MissShaderTable.StrideInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.HitGroupTable.StartAddress = base + kHitGroupOffset;
	m_DispatchRaysDesc.HitGroupTable.SizeInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.HitGroupTable.StrideInBytes = kSbtRecordSize;
	m_DispatchRaysDesc.CallableShaderTable = {};
	m_DispatchRaysDesc.Depth = 1;

	LOG_INFO(L"[DXR] SBT 鏋勫缓瀹屾垚锛歊ayGen=%s Miss=%s ShadowMiss=%s HitGroup=%s "
		L"璁板綍=%u B锛宮iss 琛?2 鏉n",
		rayGenExport, missExport, shadowMissExport, hitGroupName, kSbtRecordSize);
	return true;
}

void Renderer::RayTracing::RaytracingScene::DestroyBlasBuffers()
{
	for (auto& blas : m_BlasBuffers)
		blas.reset();
	m_BlasBuffers.clear();
	m_BlasScratchOffsets.clear();
}

void Renderer::RayTracing::RaytracingScene::DestroySceneBuffers()
{
	m_TopLevel.reset();
	m_Scratch.reset();
	m_TlasScratch.reset();
	m_SceneGeometry.reset();
	m_InstanceDataBuffer.reset();
	m_SbtUpload.Destroy();

	m_DispatchRaysDesc = {};
	m_InstanceCount = 0;
	m_TriangleCount = 0;
	m_ScratchSize = 0;
	m_TlasScratchOffset = 0;
	m_TopLevelBuilt = false;
}

void Renderer::RayTracing::RaytracingScene::Destroy()
{
	DestroyBlasBuffers();
	DestroySceneBuffers();
}
