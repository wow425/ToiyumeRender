// RenderParams.h
// R3：渲染参数的唯一 CPU 真源。
//
// 设计约束（见 docs/AI/TooiyumeRender_升级路线图_2026-10-24.md §6.2）：
//   UI（ImGui 面板） -> RenderParams（唯一 CPU 真源） -> 常量缓冲/结构化缓冲 -> Shader
// 任何渲染参数只允许有两处存在形式：这里的一份 CPU 值，以及派生出来的 GPU 缓冲。
// 面板不得直接持有参数副本，否则 UI 显示会和实际生效值分叉。
//
// 这里刻意不依赖 ImGui / D3D12 / 任何 RHI 头：RenderParams 是纯数据，
// 布局一致性由各消费者（Scene::LightingSystem、着色器 cbuffer）自己保证。

#pragma once

#include <cstdint>

namespace Renderer
{
	// Debug View 通道。编号与 docs/AI/R1_核心渲染正确性.md 中记录的调试开关一致，
	// 便于对照历史截图与验证记录。
	enum class DebugView : uint32_t
	{
		Final = 0,      // 正常 PBR + IBL 输出
		BaseColor = 1,  // GBuffer BaseColor (sRGB 前)
		Normal = 2,     // 世界空间法线 * 0.5 + 0.5
		Metallic = 3,
		Roughness = 4,
		AO = 5,
		Emission = 6,
		Depth = 7,      // 线性化前的深度缓冲

		Count
	};

	inline const wchar_t* DebugViewName(DebugView view)
	{
		switch (view)
		{
		case DebugView::Final:     return L"Final";
		case DebugView::BaseColor: return L"BaseColor";
		case DebugView::Normal:    return L"Normal";
		case DebugView::Metallic:  return L"Metallic";
		case DebugView::Roughness: return L"Roughness";
		case DebugView::AO:        return L"AO";
		case DebugView::Emission:  return L"Emission";
		case DebugView::Depth:     return L"Depth";
		default:                   return L"Unknown";
		}
	}

	// 渲染参数（CPU 真源）。
	//
	// 默认值与 Scene::LightingSystem::CreateLights() 里的历史硬编码值一致，
	// 保证「接入 RenderParams」这一动作本身不改变任何既有画面表现。
	struct RenderParams
	{
		// ---- 方向光 ----
		// 注意语义：这里存的是 DirectionIntensity.xyz 的原始值，
		// 着色器内部会做 dirL = normalize(-DirectionIntensity.xyz)。
		// 也就是说它写的是「光的传播方向」的反向约定沿用既有数据，不要在这里取反，
		// 否则整幅画面的主光高低会翻转。
		float SunDirection[3] = { 0.3f, -1.0f, -0.5f };
		float SunIntensity = 3.0f;                        // R1 曾把 10 降到 3 抑制过曝
		float SunColor[3] = { 1.0f, 0.95f, 0.9f };
		bool  EnableSunLight = true;

		// ---- 点光 ----
		float PointLightPosition[3] = { 0.0f, 2.0f, -2.0f };
		float PointLightRange = 30.0f;
		float PointLightIntensity = 4.0f;                 // R1 曾把 80 降到 4
		float PointLightColor[3] = { 1.0f, 0.8f, 0.6f };
		bool  EnablePointLight = true;

		// ---- 环境光（IBL）----
		bool  EnableIBL = true;
		float IBLIntensity = 1.0f;

		// ---- 曝光 / 色调映射 / 输出色彩空间 ----
		// 曝光的语义：**真实物理曝光倍率**，在 ACES 之前乘到 HDR 线性颜色上。
		// R4 起这一条链（Exposure -> Tonemap -> sRGB）由 Lighting 与 Skybox **共用**
		// 同一份 HLSL 实现（Shader/02_Deferred/ColorPipeline.hlsli），
		// 因此天空不再是被曝光链漏掉的那一块。
		float Exposure = 1.0f;
		bool  EnableTonemap = true;

		// R4：输出色彩空间。
		// 为什么需要这个开关：SceneColor 与 swapchain 都是 R8G8B8A8_**UNORM**（非 _SRGB），
		// 硬件不会替我们做 gamma 编码。若不显式编码，线性值会被直接当成 gamma 值显示。
		// 保留开关是为了能做「编码前 / 编码后」的 A/B 取证 —— 不是装饰性控件。
		bool  EnableSRGBEncode = true;

		// 注意：Specular IBL 的 roughness->mip 上界**刻意不放在这里**。
		// 它不是用户可调的参数，而是烘焙产物的真实属性（PrefilterCube 的 mip 数 - 1）。
		// 放进来会形成"用户真源"与"资产事实"两份可能分叉的值，
		// 因此改由 Scene::LightingSystem::SetPrefilterMaxMip（渲染器在烘焙后回填）承载，
		// 面板只做只读展示。

		// ---- 调试视图 ----
		DebugView Debug = DebugView::Final;

		// ============================================================
		// R4.6：Renderer Feature 开关
		// ------------------------------------------------------------
		// 与上面那些参数一样，这里是**唯一 CPU 真源**：UI 只写这份，
		// 渲染器每帧从 RenderFrameDesc::Params 读取。
		//
		// 重要前提（避免把开关做成"假控件"）：
		//   Shadow / SSAO / Transparent / PostProcess 在 DeferredRenderer 里
		//   目前只有 PIX 事件、没有实际 GPU 命令（见 Render() 里各 Pass 的注释）。
		//   因此这些开关控制的是"**是否允许该 Pass 参与**"，
		//   并不凭空变出效果。面板上如实标注「已实现 / 空壳」，避免
		//   "拨了没反应"被误认为 bug。
		// ============================================================
		bool EnableShadow      = false;
		bool EnableSSAO        = false;
		bool EnableTransparent = true;
		bool EnablePostProcess = false;

		// 把全部参数恢复为默认值。
		// 实现方式是"整体赋值一个默认构造的 RenderParams"，而不是逐字段手写 ——
		// 后者每加一个字段就要记得同步，迟早漏掉一个。
		void ResetToDefaults(void) { *this = RenderParams{}; }
	};

	// GPU 侧参数块大小（字节）。LightingPS.hlsl 的 cbuffer Light 必须逐字段一致：
	//   float4 DirectionIntensity;         // 0
	//   float4 DirectionalColor;           // 16
	//   float4 PointLightPositionRange;    // 32
	//   float4 PointLightColorIntensity;   // 48
	//   float4 PostParams;                 // 64  x=Exposure y=EnableSun z=EnablePoint w=EnableTonemap
	//   float4 DebugParams;                // 80  x=DebugView y=EnableIBL z=IBLIntensity w=unused
	//   float4 DisplayParams;              // 96  x=EnableSRGBEncode y=PrefilterMaxMip z/w=unused  （R4 新增）
	// 总计 112 字节（16 字节对齐）。
	// 新增字段只能**追加在末尾**，并且必须同步 Scene::LightingSystem::LightData
	// 与 LightingPS.hlsl 的 cbuffer —— 三处任一处漏改都会让后续字段整体错位。
	inline constexpr uint32_t kLightShaderParamsBytes = 112;
}
