// ============================================================================
// 模块：GFL2SharedTypes.h（少前2复刻 · C++ / HLSL 严格同构共享契约头）
// 架构层：Engine/03_AssetSystem/Assets/Constants（Phase 2 / M0）
// 对齐基准：
//   - UE5 左手系 Z-Up（+X Forward, +Y Right, +Z Up）
//   - UE5 行向量左乘（v * M）与 #pragma pack_matrix(row_major)
//   - UE5 Reverse-Z 深度序（Near=1.0, Far=0.0, Clear=0.0f, GREATER_EQUAL）
// 硬约束：任何字段增删或排序改动必须同步修改 Engine/Shader/00_Common/GFL2SharedTypes.hlsli，
//         并确保下方的所有 static_assert 全部通过。
// ============================================================================
#pragma once

#include <cstdint>
#include <DirectXMath.h>

#include "00_Core/Math/VectorMath.h"

namespace GFL2
{
	inline constexpr uint32_t kInvalidBindlessIdx = 0xFFFFFFFFu;
	inline constexpr uint32_t kCb2Vec4Count       = 32u;
	inline constexpr uint32_t kSHVec4Count        = 7u;

	// 角色与环境离散着色模型编号（与 DebugView::ShadingModelID (13) 伪彩表一一对应）
	enum class ShadingModel : uint32_t
	{
		Body        = 0,  // 身体主肌肤（PBR+NPR Ramp + 皮肤 SH DC 归一化）
		Cloth       = 1,  // 泳装 / 外套等织物
		Weapon      = 2,  // 突击步枪 / 手枪 / 战术配件
		Nails       = 3,  // 指甲 / 金属光泽小件
		Face        = 4,  // 二次元面部（SDF + 鼻梁高光 + Stencil 200）
		Hair        = 5,  // 头发（UV1 + V_ws.z 天使环 + 发丝 AO）
		EyeBase     = 6,  // 眼球底层（视差虹膜 + Stencil 204）
		EyeAdd      = 7,  // 眼部加法高光（Blend One, One）
		EyeMul      = 8,  // 眼窝乘法阴影（Blend DstColor, Zero）
		Outline     = 9,  // 背面平滑法线挤出描边（CullFront）
		Transparent = 10, // 5 组双面半透明件（CullFront -> CullBack）
		Env         = 11, // 整备室静物建筑 / Lightmap / 天空盒
	};

	// 光栅剔除模式
	enum class CullModeId : uint32_t
	{
		Back  = 0,
		Front = 1,
		None  = 2,
	};

	// 混合模式
	enum class BlendModeId : uint32_t
	{
		Opaque     = 0, // One, Zero
		AlphaBlend = 1, // SrcAlpha, InvSrcAlpha
		Additive   = 2, // One, One
		Multiply   = 3, // DstColor, Zero
	};

	// 4 态模板测试预设（M4 验证核心）
	enum class StencilPresetId : uint32_t
	{
		Disabled              = 0,
		Write200Always        = 1, // Body / Face / Weapon：Always -> Replace 200
		Write203Always        = 2, // HairBack：Always -> Replace 203
		Write204Always        = 3, // Eyebrow / EyeBase：Always -> Replace 204
		HairBangsNotEqual204  = 4, // EID 3178：Front/Back NotEqual 204 -> Replace 204
		BangsShadowInc200To201= 5, // EID 3198：Equal 200 -> IncSat (201)
		FaceRedrawDec201To200 = 6, // EID 3295：Equal 201 -> DecSat (200)
		BangsOverEyeEqual204  = 7, // EID 3335：Equal 204 -> Keep (49% Alpha 透眉)
		OutlineNotEqual204    = 8, // EID 3577：Stencil Back NotEqual 204
	};

	// Bindless 材质标志位（GFL2MaterialData::TexIndices1.w / MaterialFlags）
	enum MaterialFlagBits : uint32_t
	{
		kMatFlag_None                = 0u,
		kMatFlag_UseSecondUV         = 1u << 0, // cb2[30].z > 0（Face SDF / Hair 天使环用 UV1）
		kMatFlag_RampEnable          = 1u << 1, // cb2[30].y > 0（启用 NPR Ramp 查表）
		kMatFlag_SkinSHDCNormalize   = 1u << 2, // cb2[30].w > 0（皮肤 L2 SH 去脏归一化）
		kMatFlag_ShadowLightBias     = 1u << 3, // cb2[30].x > 0
		kMatFlag_HasBaseColorAlpha   = 1u << 4,
		kMatFlag_HasHairSpecAO       = 1u << 5,
		kMatFlag_HasSDFMask          = 1u << 6,
		kMatFlag_HasEyeBlend         = 1u << 7,
		kMatFlag_HasValidSH          = 1u << 8,
		kMatFlag_IsTransparentBack   = 1u << 9,
	};

	// ========================================================================
	// 1. RootConstants（b0，16 DWORD = 64 字节）
	// ------------------------------------------------------------------------
	// 与 RenderServices::GetBindlessGraphicsRootSignature() RootParam[0] 对齐。
	// ========================================================================
	struct alignas(16) GFL2RootConstants
	{
		uint32_t DrawIndex               = 0u;                   // DWORD  0：当前 DrawCall 在 GFL2DrawData[] 中的下标
		uint32_t MaterialIndex           = 0u;                   // DWORD  1：当前材质在 GFL2MaterialData[] 中的下标
		uint32_t DrawDataSrvIdx          = kInvalidBindlessIdx;  // DWORD  2：StructuredBuffer<GFL2DrawData> Zone A SRV
		uint32_t MaterialDataSrvIdx      = kInvalidBindlessIdx;  // DWORD  3：StructuredBuffer<GFL2MaterialData> Zone A SRV
		uint32_t MainShadowMapSrvIdx     = kInvalidBindlessIdx;  // DWORD  4：主光 Reverse-Z ShadowMap Zone B/A SRV
		uint32_t ReflectionProbeSrvIdx   = kInvalidBindlessIdx;  // DWORD  5：5682 ReflectionProbe-6 Cube SRV
		uint32_t AuxTex0SrvIdx           = kInvalidBindlessIdx;  // DWORD  6：通用扩展槽（如 LUT / Bloom / SSAO 输入）
		uint32_t AuxTex1SrvIdx           = kInvalidBindlessIdx;  // DWORD  7：通用扩展槽
		uint32_t AuxUavIdx               = kInvalidBindlessIdx;  // DWORD  8：通用扩展 UAV
		uint32_t PassSubMode             = 0u;                   // DWORD  9：Pass 子模式标识（如透明分层/描边组号）
		float    PassParam0              = 0.0f;                 // DWORD 10：Pass 浮点参数 0
		float    PassParam1              = 0.0f;                 // DWORD 11：Pass 浮点参数 1
		float    PassParam2              = 0.0f;                 // DWORD 12：Pass 浮点参数 2
		float    PassParam3              = 0.0f;                 // DWORD 13：Pass 浮点参数 3
		uint32_t Reserved0               = 0u;                   // DWORD 14
		uint32_t Reserved1               = 0u;                   // DWORD 15
	};
	static_assert(sizeof(GFL2RootConstants) == 64 && sizeof(GFL2RootConstants) % 16 == 0,
		"GFL2RootConstants must be exactly 64 bytes (16 DWORDs)");

	// ========================================================================
	// 2. GFL2FrameConstants（b1 Dynamic CBV，512 字节 = 32 × float4）
	// ========================================================================
	struct alignas(16) GFL2FrameConstants
	{
		Math::Matrix4 ViewProj;                                  // offset   0：World_UE5 -> Clip (LH Reverse-Z)
		Math::Matrix4 InvViewProj;                               // offset  64：Clip -> World_UE5
		Math::Matrix4 View;                                      // offset 128：World_UE5 -> View_UE5 (+X Right,+Y Up,+Z Fwd)
		Math::Matrix4 Proj;                                      // offset 192：View_UE5 -> Clip (Reverse-Z)
		Math::Matrix4 MainLightViewProj;                         // offset 256：主光阴影矩阵（UE5 Reverse-Z）

		DirectX::XMFLOAT4 CameraPosAndNear          = {};        // offset 320：xyz=UE5 相机位置, w=NearClip
		DirectX::XMFLOAT4 CameraFwdAndFar           = {};        // offset 336：xyz=UE5 相机前向, w=FarClip
		DirectX::XMFLOAT4 MainLightDirToLightAndInt = {};        // offset 352：xyz=指向主光源 UE5 单位方向, w=Intensity
		DirectX::XMFLOAT4 MainLightColorAndShadow   = {};        // offset 368：rgb=MainLightColor, w=ShadowStrength
		DirectX::XMFLOAT4 ScreenSizeAndInv          = {};        // offset 384：xy=(W,H), zw=(1/W, 1/H)

		DirectX::XMFLOAT4 HeadOriginUE5             = {};        // offset 400：xyz=AK-12 头部原点(UE5), w=unused
		DirectX::XMFLOAT4 HeadForwardUE5            = {};        // offset 416：xyz=面部正前向(UE5), w=unused
		DirectX::XMFLOAT4 HeadRightUE5              = {};        // offset 432：xyz=面部右侧向(UE5), w=unused

		DirectX::XMFLOAT4 ToggleFlags0              = {};        // offset 448：x=DebugView, y=EnableRamp, z=SkinSHDCNorm, w=IBLIntensity
		DirectX::XMFLOAT4 ToggleFlags1              = {};        // offset 464：x=OutlineWidthScale, y=OutlineZBias, z=MinPx, w=MaxPx
		DirectX::XMFLOAT4 ToggleFlags2              = {};        // offset 480：x=FaceSdfSmooth, y=HairSpecInt, z=HairViewZShift, w=BangsAlpha
		DirectX::XMFLOAT4 ShadowBiasParams          = {};        // offset 496：x=NormalBias, y=DepthBias, z=PCFRadius, w=FeatureMaskBits
	};
	static_assert(sizeof(GFL2FrameConstants) == 512 && sizeof(GFL2FrameConstants) % 16 == 0,
		"GFL2FrameConstants must match GFL2SharedTypes.hlsli FrameCB (512 bytes)");

	// ========================================================================
	// 3. GFL2DrawData（StructuredBuffer<GFL2DrawData>，256 字节 = 16 × float4）
	// ========================================================================
	struct alignas(16) GFL2DrawData
	{
		Math::Matrix4 WorldMatrix;                               // offset   0：Object -> UE5 World (row-major)
		Math::Matrix4 WorldIT;                                   // offset  64：Normal Matrix
		DirectX::XMFLOAT4 SH_L2[kSHVec4Count] = {};              // offset 128：已循环置换至 UE5 基的 Unity L2 SH
		uint32_t MaterialIndex                = 0u;              // offset 240
		uint32_t SubmeshIndex                 = 0u;              // offset 244
		uint32_t EventId                      = 0u;              // offset 248
		uint32_t Flags                        = 0u;              // offset 252（bit0: HasValidSH 等）
	};
	static_assert(sizeof(GFL2DrawData) == 256 && sizeof(GFL2DrawData) % 16 == 0,
		"GFL2DrawData must match HLSL StructuredBuffer<GFL2DrawData> (256 bytes)");

	// ========================================================================
	// 4. GFL2MaterialData（StructuredBuffer<GFL2MaterialData>，560 字节 = 35 × float4）
	// ------------------------------------------------------------------------
	// 完整保留原截帧 cb2[0..31] 全部参数与 Zone A 8 个纹理/状态槽，零有损丢弃。
	// ========================================================================
	struct alignas(16) GFL2MaterialData
	{
		// 纹理 Bindless 下标槽 #0（offset 0）
		uint32_t BaseColorIdx       = kInvalidBindlessIdx;       // *_d 或 *_da
		uint32_t NormalIdx          = kInvalidBindlessIdx;       // *_n（GF2 G 翻转）
		uint32_t RmoOrSdfIdx        = kInvalidBindlessIdx;       // *_rmo 或 6843 C_face_b
		uint32_t RampMapIdx         = kInvalidBindlessIdx;       // 4 行 R16G16B16A16F Ramp

		// 纹理 Bindless 下标槽 #1（offset 16）
		uint32_t HairSpecAoIdx      = kInvalidBindlessIdx;       // 8939 hair_spc
		uint32_t EyeBlendIdx        = kInvalidBindlessIdx;       // 8557 eyeblend
		uint32_t ReflectionProbeIdx = kInvalidBindlessIdx;       // 5682 env_ReflectionProbe6
		uint32_t MaterialFlags      = 0u;                        // MaterialFlagBits

		// 元数据与管线状态索引（offset 32）
		uint32_t EventId            = 0u;
		uint32_t ShadingModelId     = 0u;                        // GFL2::ShadingModel
		uint32_t CullMode           = 0u;                        // GFL2::CullModeId
		uint32_t StencilRefAndPreset= 0u;                        // 低8位=StencilRef, 次8位=StencilPresetId, 高8位=BlendModeId

		// 原生 cb2[0..31]（offset 48..559，512 字节）
		DirectX::XMFLOAT4 RawCb2[kCb2Vec4Count] = {};
	};
	static_assert(sizeof(GFL2MaterialData) == 560 && sizeof(GFL2MaterialData) % 16 == 0,
		"GFL2MaterialData must match HLSL StructuredBuffer<GFL2MaterialData> (560 bytes)");
}
