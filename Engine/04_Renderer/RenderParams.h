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

	// ============================================================
	// R6 / D3：DXR 光线效果的调试通道
	// ------------------------------------------------------------
	// 为什么**不**复用上面的 DebugView：
	//   DebugView 的每一项都对应 DeferredRenderer 的某一张 GBuffer
	//   （BaseColor / Metallic / Roughness / AO / Depth …），
	//   而 RayTracingRenderer 没有 GBuffer，也不接材质（见路线图 D3 未完成项）。
	//   把两者塞进同一个枚举会让「Deferred 选 Metallic」与「DXR 选 Metallic」
	//   表现出完全不同的含义，属于语义陷阱。
	//   因此单开一个枚举，只描述 DXR 路径**能真实产出**的通道。
	// ============================================================
	enum class RayTracingDebugView : uint32_t
	{
		Shaded = 0,      // Lambert/GGX + 硬阴影 + 反射的最终着色（主画面）
		Normal = 1,      // 主射线命中的世界空间法线（*0.5+0.5）
		Shadow = 2,      // 主光可见度：1=受光，0=被遮挡
		Reflection = 3,  // 只显示反射射线颜色（未开反射时即天空色）
		HitDistance = 4, // 主射线命中距离 / 32 米（超出为白）

		// R7 / D4：基色通道。
		//
		// 这是**两条渲染路径之间唯一可逐像素比对的通道**：
		//   Deferred 的 DebugView::BaseColor 直接输出 GBuffer0 的 albedo，
		//   本通道直接输出命中点的 baseColorFactor * BaseColor 贴图采样。
		// 两者若一致，说明"贴图寻址 + UV + 采样器 + 颜色因子"整条链路都对；
		// 若不一致，差异一定出在这四者之一，而不是被光照掩盖。
		BaseColor = 5,

		Count
	};

	inline const wchar_t* RayTracingDebugViewName(RayTracingDebugView view)
	{
		switch (view)
		{
		case RayTracingDebugView::Shaded:      return L"Shaded";
		case RayTracingDebugView::Normal:      return L"Normal";
		case RayTracingDebugView::Shadow:      return L"Shadow";
		case RayTracingDebugView::Reflection:  return L"Reflection";
		case RayTracingDebugView::HitDistance: return L"HitDistance";
		case RayTracingDebugView::BaseColor:   return L"BaseColor";
		default:                               return L"Unknown";
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

		// ============================================================
		// R6 / D3：DXR 光线效果参数
		// ------------------------------------------------------------
		// 与上面四项的根本差别：**这几项在 RayTracingRenderer 里有真实 GPU 命令**。
		// 拨动它们会改变 DispatchRays 发出的射线数量与着色结果，
		// 不是"允许 Pass 参与"的空壳。
		// 在 Deferred / Forward 里它们不生效（那两个渲染器走光栅路径）。
		// ============================================================

		// 硬阴影射线：从命中点朝主光方向再发一条可见性射线。
		// 关闭时 DXR 画面退化为纯 Lambert（可与开启时做 A/B 对照截图）。
		bool  EnableRTShadow     = true;

		// 反射射线：命中点沿反射方向再发一条射线，按 Fresnel 混合。
		// 路线图 D3 的顺序是「先硬阴影，再反射」，因此默认关闭 ——
		// 保证默认画面能被解释为"只有阴影的 DXR"，而不是两者的叠加黑箱。
		bool  EnableRTReflection = false;

		// 阴影射线的起点沿法线抬起的距离（米）。太小 → 自遮挡噪点（acne）；
		// 太大 → 接触阴影丢失（peter-panning）。1e-3 对 1.6 米高的角色模型合适。
		// 这是**唯一**的偏差来源（故意不加第二条"沿光线方向偏移"，
		// 因为两条偏差叠加会让效果难以归因，D3 阶段只保留一个可调旋钮）。
		float RTShadowNormalBias = 0.002f;

		// 阴影射线的最大距离（米）。超过这个距离认为不受遮挡（= 受光）。
		// 主光在无限远，理论上应取无穷；但有限值能让"远处不被遮挡"成立，
		// 与方向光的物理语义一致，同时避免 TMax=FLT_MAX 带来的精度问题。
		float RTShadowMaxDistance = 100.0f;

		// 反射射线的最大距离（米）。反射是"看到什么算什么"，
		// 命中与否由 TLAS 决定，TMax 只影响极远处是否算未命中（→ 天空色）。
		float RTReflectionMaxDistance = 200.0f;

		// 反射强度倍率（线性乘到反射颜色上），便于在"过亮"和"看不见"之间调。
		float RTReflectionIntensity = 0.6f;

		// DXR 专用调试通道（见上面的 RayTracingDebugView）。
		RayTracingDebugView RTDebugView = RayTracingDebugView::Shaded;

		// ============================================================
		// R7 / D4：材质接驳的两个**正交**开关
		// ------------------------------------------------------------
		// 为什么要拆成两个而不是一个：
		//   R7 P0 同时引入了「BaseColor 贴图」与「GGX/Cook-Torrance」两项改动。
		//   若只有一个总开关，画面变了也无法判断是贴图生效还是光照模型变了。
		//   拆成两个正交开关后可以做 2×2 对照（见 R7 设计方案 §5 A5a/A5b/A5c）：
		//     全关      → 必须逐字节等于 R6 基线 md5 a43b6059…（回归锁）
		//     只开 GGX  → 仍是白模，但出现高光与能量守恒
		//     只开贴图  → 有颜色，但仍是 Lambert
		//     全开      → P0 目标画面（角色应当出现真实的布料/皮肤/金属颜色）
		// ============================================================

		// 采样 BaseColor 贴图（HLSL 里是 t3 的无界 SRV 数组）+ 乘 baseColorFactor。
		// 关闭时 albedo 恒为 (0.8, 0.8, 0.8)，与 R6 完全一致。
		bool  EnableMaterialTextures = true;

		// 用 GGX/Cook-Torrance 替换 Lambert 直射/环境项。
		// 关闭时回到 R6 的 `diffuse + albedo * ambient` 表达式。
		// 注意：R6 的单次反射（TraceReflectionRay + Schlick Fresnel）不受本开关影响 ——
		// 那是 D3 已验证过、且与漫反射/镜面 BRDF 选择无关的部分。
		bool  EnableGGX = true;

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
