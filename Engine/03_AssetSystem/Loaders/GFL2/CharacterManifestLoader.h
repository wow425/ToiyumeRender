// ============================================================================
// 妯″潡锛欳haracterManifestLoader锛圓K-12 璧勪骇 Manifest / EID 渚ц溅 JSON 鍔犺浇鍣級
// 鏋舵瀯灞傦細Engine/03_AssetSystem/Loaders/GFL2锛圡0锛?// 鏍稿績鑱岃矗锛?//   1. 瑙ｆ瀽 docs/AI/灏戝墠2澶嶅埢/璧勪骇/AK12/ak12_manifest.json 涓?55 浠?materials/EID*.json锛?//   2. 鎸夌簿纭?sRGB/Linear 鏍囪杞藉叆 52 寮犳牳蹇?.dds 璐村浘鑷?UnifiedDescriptorHeap Zone A锛?//   3. 灏?55 缁?DrawCall 鍙樻崲涓?L2 SH 鎸?GFL2MathConvert 姝ｄ氦缃崲涓?UE5 LH Z-Up 琛屽悜閲忔暟鎹紱
//   4. 鏋勫缓骞朵笂浼?StructuredBuffer<GFL2DrawData> 涓?StructuredBuffer<GFL2MaterialData>锛圸one A SRV锛夛紱
//   5. 鏋愭瀯/Shutdown 鏃剁粡 DeferredReleaseQueue 瀹夊叏褰掕繕鍏ㄩ儴 Zone A 鎻忚堪绗︼紝闆舵硠婕忛浂鎮┖銆?// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "02_RHI/Resource/GpuBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Assets/Importers/Texture/TextureManager.h"

namespace Graphics
{
	class DeferredReleaseQueue;
}

namespace Renderer
{
	class RenderServices;
	class UnifiedDescriptorHeap;
}

namespace GFL2
{
	struct LoadedTextureSlot
	{
		uint32_t     ResourceId   = 0u;
		uint32_t     BindlessIdx  = kInvalidBindlessIdx;
		bool         IsSRGB       = false;
		std::string  Role;
		std::wstring FileName;
		TextureRef   Texture;
	};

	struct DrawCallMeta
	{
		uint32_t    EventId       = 0u;
		std::string Slug;
		std::string PartNameCN;
		std::string PassName;
		uint32_t    SubmeshIndex  = 0u;
		uint32_t    IndexCount    = 0u;
		uint32_t    TriangleCount = 0u;
		uint32_t    VertexShaderId   = 0u;
		uint32_t    FragmentShaderId = 0u;
		ShadingModel ShadingModelId  = ShadingModel::Body;
		CullModeId   CullMode        = CullModeId::Back;
		BlendModeId  BlendMode       = BlendModeId::Opaque;
		StencilPresetId StencilPreset = StencilPresetId::Disabled;
		uint8_t      StencilRef      = 0u;
	};

	struct AK12AssetStats
	{
		bool         Loaded            = false;
		bool         MeshLoaded        = false;
		uint32_t     DrawCallCount     = 0u;
		uint32_t     UniqueSubmeshes   = 0u;
		uint32_t     TotalTriangles    = 0u;
		uint32_t     VertexCount       = 0u;
		uint32_t     IndexCount        = 0u;
		uint32_t     LoadedTextureCount= 0u;
		uint32_t     SrgbTextureCount  = 0u;
		uint32_t     LinearTextureCount= 0u;
		uint32_t     DrawDataSrvIdx    = kInvalidBindlessIdx;
		uint32_t     MaterialDataSrvIdx= kInvalidBindlessIdx;
		DirectX::XMFLOAT3 WorldBoundsMin = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 WorldBoundsMax = { 0.0f, 0.0f, 0.0f };
		std::wstring AssetDir;
	};

	// UE5 LH Z-Up 鍏ㄧ簿搴﹁鑹查《鐐圭粨鏋勶紙72 瀛楄妭/椤剁偣锛涗笌 CharacterOpaqueVS 杈撳叆甯冨眬閫愬瓧鑺備竴鑷达級
	struct GFL2CharacterVertex
	{
		DirectX::XMFLOAT3 Position; // (z_u, x_u, y_u)
		DirectX::XMFLOAT3 Normal;   // (nz_u, nx_u, ny_u)
		DirectX::XMFLOAT4 Tangent;  // (tz_u, tx_u, ty_u, +tw_u)
		DirectX::XMFLOAT2 UV0;
		DirectX::XMFLOAT2 UV1;
		DirectX::XMFLOAT4 Color0;   // RGB=鍒囩嚎绌洪棿骞虫粦娉曠嚎 [0,1], A=鎻忚竟瀹藉害鏉冮噸
	};
	static_assert(sizeof(GFL2CharacterVertex) == 72, "GFL2CharacterVertex must be 72 bytes");

	struct SubmeshRange
	{
		uint32_t SubmeshIndex  = 0u;
		uint32_t BaseVertex    = 0u;
		uint32_t StartIndex    = 0u;
		uint32_t IndexCount    = 0u;
		uint32_t VertexCount   = 0u;
		uint32_t TriangleCount = 0u;
	};

	class CharacterManifestLoader
	{
	public:
		CharacterManifestLoader() = default;
		~CharacterManifestLoader() = default;

		CharacterManifestLoader(const CharacterManifestLoader&) = delete;
		CharacterManifestLoader& operator=(const CharacterManifestLoader&) = delete;

		// 解析 ak12_manifest.json + 55 份 materials/EID*.json + AK12_full_assembled_LH_Unity.gltf，
		// 载入 52 张贴图与 Draw/Material SSBO 到 Zone A 并上传 UE5 几何 VB/IB。
		bool LoadAK12(Renderer::RenderServices& services, const std::wstring& customAssetDir = L"");

		// 严格按 Fence 延迟队列释放全部 Zone A 槽并销毁 StructuredBuffer / Geometry / TextureRef。
		void Shutdown(Renderer::RenderServices* services);

		const AK12AssetStats& GetStats() const { return m_Stats; }
		const std::vector<GFL2DrawData>& GetCpuDrawData() const { return m_CpuDraws; }
		const std::vector<GFL2MaterialData>& GetCpuMaterialData() const { return m_CpuMaterials; }
		const std::vector<DrawCallMeta>& GetDrawMeta() const { return m_DrawMeta; }
		const std::vector<SubmeshRange>& GetSubmeshes() const { return m_Submeshes; }
		const ByteAddressBuffer& GetVertexBuffer() const { return m_VertexBuffer; }
		const ByteAddressBuffer& GetIndexBuffer() const { return m_IndexBuffer; }
		D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const
		{
			return m_VertexBuffer.VertexBufferView(0, static_cast<uint32_t>(m_VertexBuffer.GetBufferSize()), sizeof(GFL2CharacterVertex));
		}
		D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const
		{
			return m_IndexBuffer.IndexBufferView(0, static_cast<uint32_t>(m_IndexBuffer.GetBufferSize()), true);
		}
		uint32_t FindTextureBindlessIdx(uint32_t resourceId) const;

		// 渚?M0 鏃ュ織 / UI 鏍￠獙鍙傝€冪殑鍏稿瀷璺緞鏌ユ壘
		static std::wstring ResolveAK12Root(const std::wstring& customAssetDir = L"");

	private:
		void ReleaseZoneADescriptors(Renderer::UnifiedDescriptorHeap* heap, Graphics::DeferredReleaseQueue* releaseQueue);
		bool LoadAK12Geometry(const std::wstring& ak12Root);

	private:
		AK12AssetStats                           m_Stats{};
		std::vector<LoadedTextureSlot>           m_Textures;
		std::unordered_map<uint32_t, size_t>     m_TexIndexByResourceId;
		std::vector<GFL2DrawData>                m_CpuDraws;
		std::vector<GFL2MaterialData>            m_CpuMaterials;
		std::vector<DrawCallMeta>                m_DrawMeta;
		std::vector<SubmeshRange>                m_Submeshes;
		ByteAddressBuffer                        m_VertexBuffer;
		ByteAddressBuffer                        m_IndexBuffer;
		StructuredBuffer                         m_DrawDataBuffer;
		StructuredBuffer                         m_MaterialDataBuffer;
		uint32_t                                 m_DrawDataSrvIdx     = kInvalidBindlessIdx;
		uint32_t                                 m_MaterialDataSrvIdx = kInvalidBindlessIdx;
		std::vector<uint32_t>                    m_AllocatedZoneASlots;
	};
}
