// ============================================================================
// 模块：EnvSceneLoader（M6 整备室环境场景 + 光照 + 7281 3D LUT 加载器）
// 架构层：Engine/03_AssetSystem/Loaders/GFL2
// 核心职责：
//   1. 解析 docs/AI/少前2复刻/资产/环境场景/env_manifest.json 与 220 份材质 JSON；
//   2. 载入 76 张环境 .dds 贴图与 7281_Baked3DLUT_1024x32_Linear.dds 至 Zone A；
//   3. 载入 Scene_full_assembled_LH_Unity.gltf（59 唯一网格）并正交置换为 UE5 LH Z-Up；
//   4. 解析光照/additional_lights.json（Light[0]/Light[1] 与 Light[1] 512x512 阴影矩阵）；
//   5. 析构/Shutdown 时经 DeferredReleaseQueue 安全归还全部 Zone A 描述符。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "02_RHI/Resource/GpuBuffer.h"
#include "03_AssetSystem/Assets/Constants/GFL2SharedTypes.h"
#include "03_AssetSystem/Loaders/GFL2/CharacterManifestLoader.h"

namespace Renderer
{
	class RenderServices;
	class UnifiedDescriptorHeap;
}

namespace GFL2
{
	enum EnvMaterialFlags : uint32_t
	{
		kEnvFlag_None          = 0u,
		kEnvFlag_HasLightmap   = 1u << 0,
		kEnvFlag_AlphaCutout   = 1u << 1, // fs=8175 贴花透明裁剪
		kEnvFlag_IsEmissive    = 1u << 2, // fs=7295 灯条/发光体
		kEnvFlag_IsPlatform    = 1u << 3, // fs=8173 训练平台底盘
	};

	enum class EnvDrawCategory : uint8_t
	{
		Opaque = 0,
		Skybox = 1,
		FloorMirror = 2,
		OtherFX = 3,
	};

	struct EnvDrawMeta
	{
		uint32_t        EventId      = 0u;
		uint32_t        SubmeshIndex = 0u;
		uint32_t        FragShaderId = 0u;
		CullModeId      CullMode     = CullModeId::Back;
		EnvDrawCategory Category     = EnvDrawCategory::Opaque;
		bool            IsDecal      = false;
	};

	struct GFL2AdditionalLightGpu
	{
		DirectX::XMFLOAT4 PositionAndType = {}; // xyz=UE5 pos, w=1
		DirectX::XMFLOAT4 ColorAndRange   = {}; // rgb=color, w=range
		DirectX::XMFLOAT4 AttenParams     = {};
		DirectX::XMFLOAT4 SpotDirUE5      = {};
		DirectX::XMFLOAT4 ShadowParams    = {}; // x=hasShadow
	};

	struct alignas(16) GFL2AdditionalLightsCB
	{
		GFL2AdditionalLightGpu Lights[2] = {};
		Math::Matrix4          Light1WorldToShadow;
		DirectX::XMFLOAT4      ControlParams = {}; // x=enabled, y=scale, z=addShadowSrvIdx(as float bits or pass via b0), w=activeCount
	};

	struct EnvAssetStats
	{
		bool     Loaded             = false;
		bool     MeshLoaded         = false;
		uint32_t OpaqueDrawCount    = 0u;
		uint32_t UniqueSubmeshes    = 0u;
		uint32_t VertexCount        = 0u;
		uint32_t IndexCount         = 0u;
		uint32_t LoadedTextureCount = 0u;
		uint32_t DrawDataSrvIdx     = kInvalidBindlessIdx;
		uint32_t MaterialDataSrvIdx = kInvalidBindlessIdx;
		uint32_t Lut3DSrvIdx        = kInvalidBindlessIdx;
		uint32_t SkyboxTexSrvIdx    = kInvalidBindlessIdx;
		uint32_t LightmapSrvIdx     = kInvalidBindlessIdx;
		uint32_t ShadowMaskSrvIdx   = kInvalidBindlessIdx;
		float    FloorPlaneZ        = 0.00045f;
		std::wstring AssetDir;
	};

	class EnvSceneLoader
	{
	public:
		EnvSceneLoader() = default;
		~EnvSceneLoader() = default;

		EnvSceneLoader(const EnvSceneLoader&) = delete;
		EnvSceneLoader& operator=(const EnvSceneLoader&) = delete;

		bool LoadEnv(Renderer::RenderServices& services, uint32_t reflectionProbe5682SrvIdx = kInvalidBindlessIdx);
		void Shutdown(Renderer::RenderServices* services);

		const EnvAssetStats& GetStats() const { return m_Stats; }
		const std::vector<GFL2DrawData>& GetCpuDrawData() const { return m_CpuDraws; }
		const std::vector<GFL2MaterialData>& GetCpuMaterialData() const { return m_CpuMaterials; }
		const std::vector<EnvDrawMeta>& GetDrawMeta() const { return m_DrawMeta; }
		const std::vector<SubmeshRange>& GetSubmeshes() const { return m_Submeshes; }
		const Math::Matrix4& GetLight1ShadowViewProj() const { return m_Light1ShadowViewProj; }
		const GFL2AdditionalLightsCB& GetAdditionalLightsCB() const { return m_AddLightsCB; }

		D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const
		{
			return m_VertexBuffer.VertexBufferView(0, static_cast<uint32_t>(m_VertexBuffer.GetBufferSize()), sizeof(GFL2CharacterVertex));
		}
		D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const
		{
			return m_IndexBuffer.IndexBufferView(0, static_cast<uint32_t>(m_IndexBuffer.GetBufferSize()), true);
		}

		uint32_t FindTextureBindlessIdx(uint32_t resourceId) const;
		static std::wstring ResolveAssetRoot();

	private:
		bool LoadEnvGeometry(const std::wstring& envRoot);
		bool LoadAdditionalLights(const std::wstring& assetRoot);

	private:
		EnvAssetStats                            m_Stats{};
		std::vector<LoadedTextureSlot>           m_Textures;
		std::unordered_map<uint32_t, size_t>     m_TexIndexByResourceId;
		std::vector<GFL2DrawData>                m_CpuDraws;
		std::vector<GFL2MaterialData>            m_CpuMaterials;
		std::vector<EnvDrawMeta>                 m_DrawMeta;
		std::vector<SubmeshRange>                m_Submeshes;
		ByteAddressBuffer                        m_VertexBuffer;
		ByteAddressBuffer                        m_IndexBuffer;
		StructuredBuffer                         m_DrawDataBuffer;
		StructuredBuffer                         m_MaterialDataBuffer;
		std::vector<uint32_t>                    m_AllocatedZoneASlots;

		Math::Matrix4                            m_Light1ShadowViewProj;
		GFL2AdditionalLightsCB                   m_AddLightsCB{};
	};
}
