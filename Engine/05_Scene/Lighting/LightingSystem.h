
//  



#pragma once

#include <cstdint>

class StructuredBuffer;
class GraphicsContext;

namespace Math
{
	class Vector3;
	class Matrix4;

}

namespace Scene
{
	namespace Camera
	{
		class Camera;
	}
}

namespace Renderer
{
	// R3：渲染参数的唯一 CPU 真源（Engine/04_Renderer/RenderParams.h）。
	// 这里只用前置声明 + 引用，避免 05_Scene 反向依赖 04_Renderer 的头。
	struct RenderParams;
}

namespace Scene::LightingSystem
{
	// 必须与HLSL中LightData结构体保持一致（Engine/Shader/02_Deferred/LightingPS.hlsl 的 cbuffer Light）。
	// R3：末尾追加 PostParams/DebugParams 两段，作为 Renderer::RenderParams 的 GPU 镜像。
	//      96 字节 = Renderer::kLightShaderParamsBytes。
	struct alignas(16) LightData
	{
		// xyz = Direction
		// w   = Intensity
		DirectX::XMFLOAT4 DirectionIntensity;

		// rgb = Directional Color
		// a   = unused
		DirectX::XMFLOAT4 DirectionalColor;

		// xyz = Point Light Position
		// w   = Range
		DirectX::XMFLOAT4 PointLightPositionRange;

		// rgb = Point Light Color
		// a   = Point Light Intensity
		DirectX::XMFLOAT4 PointLightColorIntensity;

		// R3（RenderParams 镜像 #1）
		// x = Exposure
		// y = EnableSunLight  (0/1)
		// z = EnablePointLight(0/1)
		// w = EnableTonemap   (0/1)
		DirectX::XMFLOAT4 PostParams;

		// R3（RenderParams 镜像 #2）
		// x = DebugView 通道编号（见 Renderer::DebugView）
		// y = EnableIBL     (0/1)
		// z = IBLIntensity
		// w = unused
		DirectX::XMFLOAT4 DebugParams;

		// R4（RenderParams 镜像 #3）—— 显示编码链参数
		// x = EnableSRGBEncode (0/1)：SceneColor 是 *_UNORM，gamma 编码必须在 Shader 里做
		// y = PrefilterMaxMip：Specular IBL 的 roughness->mip 上界（= PrefilterCube 真实 mip 数 - 1）
		// z/w = unused
		DirectX::XMFLOAT4 DisplayParams;
	};

	// R4：追加 DisplayParams 后为 112 字节。16 字节对齐仍然成立（7 × float4）。
	static_assert(sizeof(LightData) == 112, "LightData 必须与 LightingPS.hlsl 的 cbuffer Light 严格同构");

	enum { MaxLights = 1 };

	enum
	{
		point,
		direction,

		Lights
	};

	void InitializeResources(void);
	void Shutdown(void);
	void CreateLights(void);
	void UpdataState(GraphicsContext& gfxContext);

	// R3：把 CPU 真源 RenderParams 写进灯光常量缓冲。
	// 语义：值没变就直接返回（零 GPU 开销）；变了就借当前帧已有的 context 上传，
	// 不走 CommandContext::InitializeBuffer（那会内部 Finish(true) 造成每帧 GPU 同步，
	// 拖滑块时会明显掉帧）。
	// 调用时机：每帧 Update 里、Render 之前；此时 buffer 的跟踪状态是 GENERIC_READ。
	void ApplyParams(GraphicsContext& gfxContext, const Renderer::RenderParams& params);

	// R4：Specular IBL 的 roughness->mip 上界（= PrefilterCube 真实 mip 数 - 1）。
	// 由渲染器在环境图烘焙完成后回填；不是用户参数，UI 只做只读展示。
	// 内部会因值变化而让下一次 ApplyParams 强制重传，避免用户参数没动时缓存早退。
	void SetPrefilterMaxMip(float maxMip);
	float GetPrefilterMaxMip(void);

	// 当前生效的参数（只读，供 UI 显示"实际生效值"与真源比对）。
	const Renderer::RenderParams* GetAppliedParams(void);
	// 自上次 ApplyParams 以来是否真正上传过（诊断用，便于验证"UI -> CB"确实生效）。
	uint64_t GetUploadCount(void);

	extern LightData m_LightCPUBuffer[MaxLights]; // 管理CPU侧灯光数据
	extern StructuredBuffer m_LightGPUBuffer; // 管理GPU Light Buffer

};
