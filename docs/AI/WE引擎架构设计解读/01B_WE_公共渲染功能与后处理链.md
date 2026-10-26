# 01B · WE 公共渲染功能与后处理链（源码级证据报告）

> 任务：共享任务 task-2（owner **we-renderfeat**）。第一阶段仅只读调查，**未修改任何运行时代码**。
> 被调查对象：本地 `WickedEngine-master`（源码位于仓库子目录 `WickedEngine/`）。
> 证据约定：`WickedEngine/文件:行号`；**「实测」**= 本次调查直接读到的代码行/符号；**「推断」**= 由实测行推导的判断；**「未确认」**= 尚未读到、需读指定文件才能定论。
> 对照基线：约束文档 §2.1 的核心疑问 —— 本版本 WE 里 Forward / Deferred / Tiled Rendering 与 Path Tracing 究竟如何组织。

---

## 0. 结论速览（正面回答 §2.1）

**Q：本版本 WE 里 Forward / Deferred / Tiled 与 Path Tracing 如何组织？**

1. **不存在 Forward / Deferred / Tiled 三条 RenderPath3D 子类。** 实测：`WickedEngine/` 下无 `wiRenderPath3D_Deferred.*` / `_Forward.*` / `_Tiled.*`；`RenderPath3D` 的唯一子类是 `wiRenderPath3D_PathTracing`（`WickedEngine/wiRenderPath3D_PathTracing.h:8`：`class RenderPath3D_PathTracing : public RenderPath3D`）。`wiRenderPath3D.h` 的私有成员里也**没有任何 forward/deferred/rendermode 开关**（:33-93），全局 grep 无 `RENDERMODE` 符号。
2. **本版本唯一的光照方案是 tiled（clustered）forward+。** 不透明/透明/头发/冒名/海洋/粒子外观全部在像素（或计算）着色器里逐像素遍历「按 32×32 屏幕瓦片分桶的光列表」：`WickedEngine/shaders/shadingHF.hlsli:49 TiledLighting(...)`、`:408 TiledDecals(...)`，调用者见 `objectHF.hlsli`、`hairparticlePS.hlsl:78`、`impostorPS.hlsl:59`、`oceanSurfacePS.hlsl:110`、`emittedparticlePS_soft.hlsl:128`、`visibility_shadeCS.hlsl:94`。光列表由 compute 生成：`WickedEngine/wiRenderer.cpp:10173 ComputeTiledLightCulling(...)` → `shaders/lightCullingCS.hlsl`（含 Harada 2.5D 深度切片剔除，lightCullingCS.hlsl:9-11、:34）。瓦片尺寸 `TILED_CULLING_BLOCKSIZE=32`（`WickedEngine/shaders/ShaderInterop_Renderer.h:1223`）。
3. **同一套光照模型有两个「执行模型」，由运行期 bool 切换，不是编译期三选一。** `WickedEngine/wiRenderPath3D.h:208 bool visibility_shading_in_compute = false;`；为真时走 `WickedEngine/wiRenderPath3D.cpp:1473 wi::renderer::Visibility_Shade(visibilityResources, rtMain, cmd)`（实现 `wiRenderer.cpp:11827`），其着色器 `shaders/visibility_shadeCS.hlsl:25` 自述 **per-pixel lighting based on primitiveID**，并在 `:94` 调用**同一个** `TiledLighting`。即：prepass 写 `primitiveID`+深度（RT 格式 `R32_UINT`，`wiRenderPath3D.cpp:176`），compute 里按 primitiveID 重放材质并做 forward+ 着色 —— 这是 **visibility buffer 着色**，**不是**经典 deferred G-buffer（没有 albedo/normal/roughness 多目标）。
4. **Path Tracing 是唯一「另一条管线」，且它替换 RT 管线的方式是整体重写 `Render()`**（`wiRenderPath3D_PathTracing.cpp:302`），只保留基类的 `RenderVolumetrics`（:483）与 `RenderLightShafts`（:485）两个 const 虚函数，其余 pass 与整条后处理链自建（:583-707 与基类 `wiRenderPath3D.cpp:2334-2564` 高度重复）。→ 这既是「路径可替换」的可行样例，也是**公共功能复用不足**的反面样例。
5. **公共渲染功能层 = `wi::renderer` 命名空间 + 自由函数 + 按效果划分的 Resources POD 结构体。** 实测 `wiRenderer.h` / `wiRenderer.cpp` 中 `grep RenderPath` = **0 次**、`grep Application` = **0 次**、`wiRenderer.cpp` 中 `grep GetScene()` = **0 次** —— 它不依赖渲染路径、不依赖应用层、不取全局 Scene 单例，Scene/Visibility/CameraComponent/CommandList 全部作为参数传入（详见 §2）。
6. **后处理链只有一条，且 `Postprocess_Tonemap` 是唯一的 HDR→LDR 分界**（`wiRenderPath3D.cpp:2473-2490`），其后全部效果都在 LDR/显示色空间；FXAA 位于 tonemap **之后**（:2523-2528）。FSR2 在链首（HDR，:2350-2363），FSR1 在链尾（LDR，:2558-2562）。
7. **GPU 同步责任一分为二**：render pass **内**的 RT/DS 状态转换由 `GraphicsDevice` 后端按 LoadOp/StoreOp 自动生成 barrier（`wiGraphicsDevice_DX12.cpp:6114-6137`、:6375-6377）；pass **外**的 image layout、buffer 状态、UAV 依赖、**内存别名切换**必须由调用方显式 `GPUBarrier`（如 `wiRenderPath3D.cpp:874-877`、:2345-2346）。内存别名用 `AliasingAllocator`（`wiGraphicsDevice.h:449-498`）+ `GPUBarrier::Aliasing`（`wiGraphics.h:754`）实现，**没有运行时状态断言**（详见 §8）。

---

## 1. 调查方法与证据边界

**方法**：先 `glob/grep -n` 定位符号与调用点，再用 `sed -n 'A,Bp'` 精读区间；不整文件读 19687 行的 `wiRenderer.cpp`，也不整读 3327 行的 `wiRenderPath3D.cpp`。

**实际读取量（本报告结论所依据）**：

| 文件 | 总行数 | 本次精读范围 | 结论用途 |
| --- | --- | --- | --- |
| `WickedEngine/wiRenderer.h` | 1468 | :1-59（include/常量/helper）、:94-350（API 声明）、:385-1046（全部 Resources 结构体与效果函数签名） | Q1/Q2/Q6 |
| `WickedEngine/wiRenderer.cpp` | 19687 | :1-20（include）、:1947-1964、:2147、:3163-3166、:3257、:3363-3365、:3498（RENDERPASS 分支）、:7179-7283（DrawScene 全body）、:10158-10173（tiled 资源与剔除）、:11416-11438（bloom）、:12841（SSAO）、:17157（TAA） | Q1/Q2/Q3/Q6 |
| `WickedEngine/wiRenderPath3D.h` | 403 | 全文 | 全部 |
| `WickedEngine/wiRenderPath3D.cpp` | 3327 | :1-16、:17-341（Delete/Resize）、:375-830（PreRender）、:832-1786（Render 全文）、:1826-2332（Render* 虚函数）、:2334-2564（后处理链）、:2566-2790（RenderCameraComponents）、:2790-3320（setter） | 全部 |
| `WickedEngine/wiRenderPath3D_PathTracing.h/.cpp` | 53 / 733 | 全文（.cpp 精读 :1-733） | Q8 |
| `WickedEngine/wiRenderPath.h` / `wiRenderPath2D.h` / `wiRenderPath2D.cpp` | 51 / 142 / 550 | 全文要点 | Q8/§0.4 |
| `WickedEngine/wiApplication.cpp` | 938 | 生命周期钩子段（:64-90、:400-440） | §0.3 |
| `WickedEngine/wiGraphics.h` / `wiGraphicsDevice.h` / `wiGraphicsDevice_DX12.cpp` | — | GPUResource/AliasingAllocator/GPUBarrier/RenderPassBegin 段 | Q7 |
| `WickedEngine/shaders/*` | — | `ShaderInterop_Renderer.h`(tiled 常量)、`lightCullingCS.hlsl`、`shadingHF.hlsli`、`visibility_shadeCS.hlsl`、`tonemapCS.hlsl`、`bloomseparateCS.hlsl`、`ShaderInterop_Postprocess.h`、`postprocessVS.hlsl`、各 PS 的 TiledLighting 调用点 | Q3/Q4 |

**已知边界**：①未读 `wiRenderer.cpp` 中全部 67 处 `RENDERPASS` 分支站点（只读了上表列出的关键站点），因此 Q2 中「各 RENDERPASS 的 PSO/RT 格式差异」是**部分实测 + 部分推断**，凡推断均已标注；②未读 `wiGraphicsDevice_Vulkan.cpp`，本报告关于同步的结论**只对 DX12 后端实测成立**；③TooiyumeRender 现状未读源码（那是 `02_架构现状审计_audit-tooiyume.md` 的范围），本报告第 §9 的适用性判断基于任务给定的前提（约 1-3 万行、单 D3D12 后端、教学为主、现有 Deferred 管线）。

---

## 2. Q1 —— `wi::renderer`「公共渲染功能」的边界与依赖矩阵

### 2.1 API 形式：命名空间 + 自由函数（不是类）

- 命名空间起点 `WickedEngine/wiRenderer.h:27 namespace wi::renderer`（实际写作 `namespace wi { namespace renderer {`）。
- 全部能力都是**自由函数**：`DrawScene`（`wiRenderer.h:281-287`）、`DrawSky`（:307）、`DrawSun`（:309）、`DrawShadowmaps`（:311-314）、`DrawDebugWorld`（:316-321）、`DrawWireframeOverlay`（:323-327）、`DrawSoftParticles`（:329-333）、`DrawGaussianSplats`（:341-345）、`BindCameraCB`（:258-263）、`BindCommonResources`（:254）、`UpdatePerFrameData`（:214-215）、`ClearWorld`（:94）、`ProcessDeferredTextureRequests`（:290）等。
- 需要**状态**时，状态以**按效果划分的 POD 结构体 + 显式函数三元组**表达，而不是成员变量：`struct BloomResources{ Texture texture_bloom; Texture texture_temp; }`（`wiRenderer.h:413-417`）配 `CreateBloomResources`（:418）/`ComputeBloom`（:419-426）。同型：`TiledLightResources`(:385-390)、`LuminanceResources`(:400-403)、`SSAOResources`(:585-588)、`TemporalAAResources`(:890-898)、`FSR2Resources`(:950-997)……共 23 个结构体（清单见 §7）。
- 仅剩的**全局可变配置**是 `wi::renderer` 内部的进程级开关（如 `SetShadowsEnabled`、`SetTemporalAAEnabled`、`GetAdvancedLightCulling` 等布尔 setter/getter，`wiRenderer.h:96-250` 区），由 `DrawScene` 等函数读取（实测 `wiRenderer.cpp:7192` 中 `GetOcclusionCullingEnabled()` 参与 flags 解析）。→ 这是 `wi::renderer` 里**唯一**「隐式全局状态」残留，也是约束原则④「避免隐式全局状态依赖」要警惕的点。

### 2.2 依赖矩阵（实测）

| 依赖对象 | 是否依赖 | 证据 |
| --- | --- | --- |
| `wi::scene::Scene` | **是**（作为参数，不是单例） | `wiRenderer.h:94 ClearWorld(Scene&)`、:214-215 `UpdatePerFrameData(Scene&, const Visibility&, ...)`、:251 `UpdateRaytracingAccelerationStructures(const Scene&, CommandList)`、:307 `DrawSky(const Scene&, CommandList, bool)`；`grep -c GetScene() wiRenderer.cpp = 0` |
| `wi::scene::CameraComponent` | **是** | `wiRenderer.h:259-261 BindCameraCB(const CameraComponent& camera, const CameraComponent& camera_previous, const CameraComponent& camera_reflection, CommandList cmd)`（注释：previous 供 reprojection、reflection 供平面反射） |
| `Visibility` | **是**（绘制入口的对象集合） | `wiRenderer.h:283 DrawScene(const Visibility& vis, ...)`；`wiRenderer.cpp:7185-7193` 读取 `vis.flags`、`vis.scene`、`vis.camera` |
| `wi::graphics::CommandList` | **是**（每调用一次都是一个 cmd） | 几乎所有 API 末参 `CommandList cmd`（`wiRenderer.h:251/254/258/281/307/311/316/323/329/341...`） |
| `wi::graphics::Texture` / `GPUBuffer` | **是**（输出/中间资源由调用方持有并传入） | `BloomResources{Texture,Texture}`(:413-417)、`Postprocess_SSAO(SSAOResources&, Texture& output, ...)`(:590-597) |
| `wi::graphics::GraphicsDevice` | **是**（创建资源、提交 barrier/dispatch） | `CreateBloomResources` 内 `device->CreateSubresource(...)`（`wiRenderer.cpp:11432-11438`）、`GraphicsDevice::CreateAliasingAllocator()`（`wiGraphicsDevice.h:500-506`） |
| `wi::image` / `wi::font` / `wi::sprite` | **是**（调试绘制、软粒子、文字叠层） | `DrawDebugWorld(const Scene&, const CameraComponent&, const Canvas&, CommandList)`（`wiRenderer.h:317-321`）；`wiRenderer.cpp:1-20` include `wiSprite.h` |
| `wi::profiler` / `wi::backlog` / `wi::jobsystem` | **是**（埋点、异步剔除/后处理） | `wiRenderer.cpp:1-20` include `wiProfiler.h`、`wiBacklog.h`、`wiJobSystem.h` |
| **`wi::RenderPath*`（渲染路径）** | **否** | `grep -c RenderPath wiRenderer.h = 0`、`grep -c RenderPath wiRenderer.cpp = 0`；`wiRenderer.h:1-19` 的 include 列表里没有任何 `wiRenderPath*.h` |
| **`wi::Application`（应用层）** | **否** | `grep -c Application wiRenderer.h = 0`、`wiRenderer.cpp = 0`；include 列表无 `wiApplication.h` |
| **GUI / 2D 叠层** | **否**（renderer 不 include GUI；2D 叠层由 `RenderPath2D` 负责） | `wiRenderer.h:1-19` 无 `wiGUI.h`；GUI 成员在 `wiRenderPath2D.h:20 wi::gui::GUI GUI` |

**方向性结论（实测）**：依赖是**单向**的 —— `RenderPath3D → wi::renderer`，反向零引用。`wi::renderer` 的入参类型把它与「谁在编排一帧」彻底解耦：它只认识 **场景 + 相机 + 可见性 + 命令列表 + GPU 资源**。这正是约束原则②「渲染路径与公共渲染功能分离」想要的分层，也是 TooiyumeRender 可以直接照搬的**接口边界**（但不必照搬 23 个结构体，见 §9）。

**设计依据（代码内证）**：`wiRenderer.h:281-287` 的注释要求调用方「You must call BindCameraCB() at least once in this command list prior to this」——把 CB 绑定这一**隐式前置条件**写成文档契约而非类型约束，是这套自由函数 API 的固有代价（下同，`BindCommonResources` 亦为前置调用，见 `wiRenderPath3D.cpp:1750-1751`、`wiRenderer.cpp:7198`）。

**替代方案与代价**：若把 `wi::renderer` 做成一个大 `Renderer` 类持有全部资源与配置，则（优点）前置条件可固化成成员不变式、可携带状态；（代价）任何一条新路径都要改同一个类、资源生命周期与路径生命周期纠缠（WE 恰恰把这件事交给了 `RenderPath3D`）。WE 选择自由函数 + 资源外置，代价是调用方必须自己保证绑定顺序（约束原则⑤的「不得隐藏 GPU 同步关系」在此方向上是一致的）。

---

## 3. Q2 —— `DrawScene` 的完整参数语义

### 3.1 签名与两个参数轴的职责划分

```cpp
// WickedEngine/wiRenderer.h:281-287
// Draw the world from a camera. You must call BindCameraCB() at least once in this command list prior to this
void DrawScene(
  const Visibility& vis,
  wi::enums::RENDERPASS renderPass,
  wi::graphics::CommandList cmd,
  uint32_t flags = DRAWSCENE_OPAQUE
);
```

实测实现体 `WickedEngine/wiRenderer.cpp:7179-7283`。**职责划分结论**：

- **`flags` 是 CPU 端「画哪些对象、怎么排序、走哪个特例分支」的筛选与编排开关**：它影响对象遍历、剔除、排序、以及是否走「海洋 / 冒名 / 头发」这些**不由 `RenderMeshes` 承担的旁路绘制**。
- **`renderPass` 是「同一批对象的这一次绘制，产出什么」的语义标签**：它被传进 `RenderMeshes`（`wiRenderer.cpp:7261`）并向下决定输出 RT 格式、深度/模板状态、pipeline state 与像素着色器变体。
- 两者**正交且不可互替**：同一个 `RENDERPASS_MAIN` 下，用不同 flags 组合分别产出 z-prepass 前景、主色 pass、透明 pass、海洋 pass（调用点见 `wiRenderPath3D.cpp:997-1015`、:1562-1580、:2169-2185、:2204-2225）。

### 3.2 `DRAWSCENE_FLAGS` 各标志位在实现里影响什么

位定义：`WickedEngine/wiRenderer.h:266-279`。实现影响（实测行号）：

| 标志（值） | 位定义 | 实现影响（`wiRenderer.cpp`） |
| --- | --- | --- |
| `DRAWSCENE_OPAQUE = 1<<0` | :267 | `:7185` 解析；`:7209-7212` 令 `filterMask \|= FILTER_OPAQUE`，逐对象 `:7239-7240` 用 `(object.GetFilterMask() & filterMask)==0` 剔除 |
| `DRAWSCENE_TRANSPARENT = 1<<1` | :268 | `:7213-7216` 令 `filterMask \|= FILTER_TRANSPARENT \| FILTER_WATER`；`:7251-7255` 走 `renderQueue.sort_transparent()`（否则 `sort_opaque()`） |
| `DRAWSCENE_OCCLUSIONCULLING = 1<<2` | :269 | `:7192` 与 `vis.flags & Visibility::ALLOW_OCCLUSION_CULLING` 及全局 `GetOcclusionCullingEnabled()` 三者相与；为真时 `:7222-7224` 跳过 `object.IsOccluded()` |
| `DRAWSCENE_TESSELLATION = 1<<3` | :270 | 解析入 `tessellation`（:7187）；实测仅见其参与传入 `RenderMeshes`/后续 PSO 选择（**未逐行核对** tessellation 在 `RenderMeshes` 内的具体分支，需读 `wiRenderer.cpp:3161-3498`） |
| `DRAWSCENE_HAIRPARTICLE = 1<<4` | :271 | `:7272-7282`：为真且（`IsWireRender()` 或非 transparent）时对 `vis.visibleHairs` 逐个 `hair.Draw(material, renderPass, cmd)` |
| `DRAWSCENE_IMPOSTOR = 1<<5` | :272 | `:7267-7270`：为真时调用 `RenderImpostors(vis, renderPass, cmd)` —— **注意：该函数不接收 flags** |
| `DRAWSCENE_OCEAN = 1<<6` | :273 | `:7200-7207`：`ocean && !skip_planar && weather.IsOceanEnabled() && ocean.IsValid()` 且未被遮挡时调用 `vis.scene->ocean.Render(*vis.camera, cmd)` |
| `DRAWSCENE_SKIP_PLANAR_REFLECTION_OBJECTS = 1<<7` | :274 | `:7230-7232`：跳过 `object.IsNotVisibleInReflections()`（注释：don't draw subsets which have planar reflection material） |
| `DRAWSCENE_FOREGROUND_ONLY = 1<<8` | :275 | `:7226-7228`：`foreground != object.IsForeground()` 即跳过（前景与常规对象**分两次 DrawScene**，见 `wiRenderPath3D.cpp:997-1015`、:1562-1580） |
| `DRAWSCENE_MAINCAMERA = 1<<9` | :276 | `:7229-7231`：跳过 `object.IsNotVisibleInMainCamera()`（对应注释 `SetNotVisibleInMainCamera(true)`） |
| `DRAWSCENE_WIREFRAME_OVERLAY = 1<<10` | :277 | 主入口 `DrawScene` 内未使用；由 `DrawWireframeOverlay(const Visibility&, RENDERPASS, CommandList)`（`wiRenderer.h:323-327`，实现 `wiRenderer.cpp:8705-8714`）承担 |

其它实测行为：`EventBegin(DrawScene)` + `BindShadingRate(RATE_1X1)` + `BindCommonResources`（:7195-7198）；对象循环还做 `IsRenderable()`、`fadeDistance + radius` 距离剔除（:7220-7245）；收尾 `BindShadingRate(RATE_1X1)` + `EventEnd`。

**设计依据（实测）**：`flags` 默认值是 `DRAWSCENE_OPAQUE`（`wiRenderer.h:286`），说明最常见调用（不透明主色）被设计成零参数负担；特殊绘制（海洋/冒名/头发/前景）被拆成可选位，而不是新增参数或新增函数。**代价**：`RenderImpostors` / `hair.Draw` 不接受 flags（:7268、:7275），意味着这两个旁路**无法**被 `FOREGROUND_ONLY` / `MAINCAMERA` / `SKIP_PLANAR_REFLECTION_OBJECTS` 等筛选约束——调用方必须在别处（`Visibility` 的标志位与 `wiRenderPath3D.cpp:1181-1247` 的 visibility 构造）补偿。这是一个**已知的语义空洞**，TooiyumeRender 若照搬应在签名上补齐。

### 3.3 `RENDERPASS` 各枚举值在实现里的分支行为

枚举定义 `WickedEngine/wiEnums.h:48-58`：`RENDERPASS_MAIN`、`RENDERPASS_PREPASS`、`RENDERPASS_PREPASS_DEPTHONLY`、`RENDERPASS_ENVMAPCAPTURE`、`RENDERPASS_SHADOW`、`RENDERPASS_VOXELIZE`、`RENDERPASS_RAINBLOCKER`、`RENDERPASS_COUNT`。（附注：`wiEnums.h:40-45` 说明 `RENDERTYPE_*` 是 deprecated naming，即这套枚举由旧 `RENDERTYPE` 演进而来。）

| 枚举值 | 实现分支证据 | 输出/状态语义（实测 + 标注推断） |
| --- | --- | --- |
| `RENDERPASS_MAIN` | `wiRenderer.cpp:1947`（`materialtype_differentiation = renderPass==RENDERPASS_MAIN ? MaterialComponent::SHADERTYPE_COUNT : 1`，注释：only RENDERPASS_MAIN needs to handle different material shader types）；`:2147`（`renderpass_info.rt_formats[0] = renderPass==RENDERPASS_MAIN ? format_rendertarget_main : format_idbuffer`）；`:3163`（`stencil_usage = renderPass==RENDERPASS_MAIN \|\| renderPass==RENDERPASS_PREPASS`）；`:3257`（`if (renderPass==RENDERPASS_MAIN)` 分支）；`:3498`（`if (renderPass!=RENDERPASS_PREPASS && renderPass!=RENDERPASS_PREPASS_DEPTHONLY)`） | 唯一做**完整材质/光照着色**的 pass；RT0 为 `format_rendertarget_main = R11G11B10_FLOAT`（`wiRenderer.h:30`）；模板启用；材质 shader 类型（Shadertype）在此生效 |
| `RENDERPASS_PREPASS` | `:1947`（差异化为 1 → 单一着色路径）；`:1964`（`if ((renderPass==RENDERPASS_PREPASS \|\| renderPass==RENDERPASS_PREPASS_DEPTHONLY) && transparency)`）；`:2147`（RT0 = `format_idbuffer = R32_UINT`，`wiRenderer.h:31`）；`:3163`、`:3166`（opaque 渲染条件含 PREPASS/PREPASS_DEPTHONLY/MAIN/SHADOW/RAINBLOCKER）；`:3363-3365`、`:3498` | 写 `primitiveID`(R32_UINT)+深度；不透明物体在此被过滤掉透明分支（:1964 推断：prepass 中 transparent 不参与） |
| `RENDERPASS_PREPASS_DEPTHONLY` | 与 PREPASS 共享 `:1964`、`:3166`；调用点 `wiRenderPath3D.cpp:1218-1226`（平面反射 depth prepass） | **推断**：只写深度不写 primitiveID（否则平面反射无需区分该枚举）。**未确认**：需读 `wiRenderer.cpp:2033-2203` 的 `renderpass_info` 构造与 `:3161-3498` 的 PSO 选择，确认其是否绑定颜色 RT |
| `RENDERPASS_ENVMAPCAPTURE` | `:1401/:1406/:1410`（RT 格式与 mip/slice 相关分支）；`:2134-2203`（renderpass_info 之一分支） | **推断**：用于环境探针 6 面捕获，格式 `format_rendertarget_envprobe = R11G11B10_FLOAT` / 深度 `format_depthbuffer_envprobe`（`wiRenderer.h:35-36`）。**未确认**具体 face/mip 选择逻辑 |
| `RENDERPASS_SHADOW` | `:3166`（opaque 条件含 SHADOW）；格式 `format_rendertarget_shadowmap = R16G16B16A16_FLOAT`、`format_depthbuffer_shadowmap = D16_UNORM`（`wiRenderer.h:32-33`）；由 `DrawShadowmaps(const Visibility&, CommandList)`（`wiRenderer.h:311-314`）驱动 | 阴影图渲染：走 opaque 几何、不透明着色前的最简路径 |
| `RENDERPASS_VOXELIZE` | `:3363-3365` 排除列表含 VOXELIZE | 体素化（VXGI 用），不参与常规几何着色 |
| `RENDERPASS_RAINBLOCKER` | `:3166`（opaque 条件含 RAINBLOCKER） | 雨遮挡体（`ReplyRainBlocker` 类对象）专用 pass |

**关键结论**：`RENDERPASS` 区分的是**渲染目的/输出产物**（主色、ID、深度、阴影、体素、探针、雨遮挡），**不是** forward/deferred 两种着色路线。把它误读成「渲染模式」是旧版 WE 印象的直接后果（见 §4）。

---

## 4. Q3（核心问题）—— forward / deferred / tiled 在本版本究竟如何表达

### 4.1 决定性否定证据（实测）

| 检查 | 命令/位置 | 结果 |
| --- | --- | --- |
| 是否存在 `RenderPath3D_Forward/Deferred/Tiled` | `WickedEngine/` 目录列举 | **不存在**（无对应 .h/.cpp） |
| `RenderPath3D` 的子类 | `wiRenderPath3D_PathTracing.h:8` | **只有** `class RenderPath3D_PathTracing : public RenderPath3D` |
| 是否有 rendermode 成员/宏 | `grep -rn RENDERMODE`（排除 third_party） | **无命中** |
| `wiRenderPath3D.h` 的路径开关 | :33-93（全部 private 参数与开关） | 只有 AO/SSR/SSGI/RT反射/阴影/泛光/调色/体积光/光柱/镜头光晕/运动模糊/景深/眼适应/锐化/描边/色差/抖动/遮挡剔除/场景更新/FSR/FSR2/mesh_blend/CRT 等**效果开关**，**无任何** forward/deferred/tiled 开关 |
| 主不透明着色的 `DrawScene` 调用次数 | `wiRenderPath3D.cpp:1544-1582` | 只有**一套**（除 `visibility_shading_in_compute` 的执行模型分支外），见下 |

### 4.2 主场景不透明着色的唯一分支：执行模型开关（实测行号）

```cpp
// WickedEngine/wiRenderPath3D.cpp:1544-1582（摘）
if (visibility_shading_in_compute)
{
    // In visibility compute shading, the impostors must still be drawn using rasterization
    wi::renderer::DrawScene(visibility_main, wi::enums::RENDERPASS_MAIN, cmd, wi::enums::DRAWSCENE_IMPOSTOR);
}
else
{
    wi::renderer::DrawScene(visibility_main, wi::enums::RENDERPASS_MAIN, cmd,
        wi::enums::DRAWSCENE_OPAQUE | wi::enums::DRAWSCENE_FOREGROUND_ONLY | wi::enums::DRAWSCENE_MAINCAMERA);
    wi::renderer::DrawScene(visibility_main, wi::enums::RENDERPASS_MAIN, cmd, drawscene_flags);
}
```

- `visibility_shading_in_compute` 是**运行期 `bool` 成员**（`wiRenderPath3D.h:208`），有 `getVisibilityComputeShadingEnabled()` / `setVisibilityComputeShadingEnabled()`（`wiRenderPath3D.h:304`、:336）。**全部出现位置**：`wiRenderPath3D.h:208/304/336`、`wiRenderPath3D.cpp:652/1473/1528/1544`。既不是编译期宏，也不是 shader 变体选择器。
- 为真时：不透明几何在 **prepass** 阶段写 `rtPrimitiveID`（RT 格式 `R32_UINT`，`wiRenderPath3D.cpp:176`；RenderPass 见 :964-980、DrawScene 见 :997-1015），随后 `wi::renderer::Visibility_Shade(visibilityResources, rtMain, cmd)`（`wiRenderPath3D.cpp:1473-1480`，实现 `wiRenderer.cpp:11827`）在 compute 里**按 primitiveID 重放材质并逐像素着色**；主 RenderPass 的 RT0 因此改为 `LOAD` 而非 `CLEAR`（`wiRenderPath3D.cpp:1526-1529`，且带 `RenderPassFlags::ALLOW_UAV_WRITES`，:1541）。
- 为假时：经典「深度预pass（写 primitiveID 与深度）→ 光栅化不透明主色 pass」流程。
- 两条路的**光照算法完全相同**：`shaders/visibility_shadeCS.hlsl:25` 注释 `This shader computes per-pixel lighting based on primitiveID`，`:94` 调用 `TiledLighting(surface, lighting, entity_flat_tile_index, camera)`；光栅路径则由 `shaders/objectHF.hlsli` 调用同一个 `TiledLighting`。

### 4.3 tiled（clustered）forward+ 光照的确切机制（实测）

1. **每相机一份 tile 缓冲**：`CreateTiledLightResources(TiledLightResources& res, XMUINT2 resolution, uint32_t camera_count=1)`（`wiRenderer.h:391`，实现 `wiRenderer.cpp:10158`）：`res.tileCount = GetEntityCullingTileCount(resolution)`（`wiRenderer.h:47-52`，按 `TILED_CULLING_BLOCKSIZE` 向上取整）；缓冲 `size = tileCount.x * tileCount.y * sizeof(uint) * SHADER_ENTITY_TILE_BUCKET_COUNT * 2 * camera_count`（`*2` 的注释：**opaque and transparent arrays**）；用法为 `UNORDERED_ACCESS | SHADER_RESOURCE`、`BUFFER_STRUCTURED`。
2. **常量**：`shaders/ShaderInterop_Renderer.h:1223 TILED_CULLING_BLOCKSIZE=32`、`:1224 TILED_CULLING_THREADSIZE=8`、`:1148 SHADER_ENTITY_TILE_BUCKET_COUNT = SHADER_ENTITY_COUNT / 32`、`:1399 entity_culling_tile_bucket_count_flat`（注释：tilecount.x*tilecount.y*SHADER_ENTITY_TILE_BUCKET_COUNT）。
3. **剔除**：`ComputeTiledLightCulling(const TiledLightResources&, const Visibility&, const Texture& debugUAV, CommandList cmd)`（`wiRenderer.h:393-398`，实现 `wiRenderer.cpp:10173`）：先 `Barrier(Buffer entityTiles SRV→UAV)`；若 `visibleLights`/`visibleDecals`/`visibleEnvProbes` 全空则只 `ClearUAV` + barrier 回转（快路径）；否则 `Dispatch(tileCount.x, tileCount.y, camera_count)`，CS 变体由 `GetDebugLightCulling()` / `GetAdvancedLightCulling()` 选择；结束后 barrier 回 SRV。
4. **CS 实现**：`shaders/lightCullingCS.hlsl:4 RWStructuredBuffer<uint> entityTiles : register(u0)`；`:9-11` `groupshared uMinDepth/uMaxDepth/uDepthMask`（注释 **Harada Siggraph 2012 2.5D culling**）+ `tile_opaque/tile_transparent[SHADER_ENTITY_TILE_BUCKET_COUNT]`；`:16-31` 用 `InterlockedOr` 把实体索引置 bit；`:34 ConstructEntityMask` 做深度切片；`:400-417` 按 `camera.entity_culling_tile_offset` 计算 `flatTileIndex`，opaque 写 `entityTiles[addr+i]`、transparent 写 `entityTiles[camera.entity_culling_tile_bucket_count_flat + addr+i]`。
5. **消费**：`shaders/shadingHF.hlsli:43-47 GetFlatTileIndex(pixel, camera)`（`floor(pixel / TILED_CULLING_BLOCKSIZE)` → 2D→flat → `* SHADER_ENTITY_TILE_BUCKET_COUNT`）；`:49 TiledLighting(inout Surface, inout Lighting, uint flatTileIndex, ShaderCamera camera = GetCamera())`：`camera.buffer_entitytiles_index < 0` 或 `OPTION_BIT_FORCE_UNLIT` 时直接返回；逐 bucket 读 `load_entitytile`（`shaders/globals.hlsli:594-596`，分别索引 opaque / transparent 偏移）累加光照与 envmap/probe，含 `WaveActiveBitOr` 做 bucket 早退（注释引用 Siggraph 2017 Improved Culling [Michal Drobot]）；`:408 TiledDecals(...)` 同法遍历 decal。
6. **调用点（实测 grep）**：`ComputeTiledLightCulling` 在 `wiRenderPath3D.cpp:1047`（主相机，`tiledLightResources`、`visibility_main`、`debugUAV`）与 `:1327`（平面反射，`tiledLightResources_planarReflection`、`visibility_reflection`、`Texture()`）；`CreateTiledLightResources` 在 `wiRenderPath3D.cpp:322`（`ResizeBuffers`）、`:2661`（`RenderCameraComponents` 内为 `CameraComponent::render_to_texture` 建**局部** `TiledLightResources tiledres`）、`:3144`（`setReflectionsEnabled` 内重建平面反射版）。

### 4.4 一句话结论与「为什么旧印象会误导」

**本版本 WE 没有 forward/deferred/tiled 三条路径，也没有运行期的 forward↔deferred 切换。它只有一套 tiled forward+ 光照，外加两个执行模型（光栅化 PS / visibility-buffer CS），再外加一条完全独立、整体重写 `Render()` 的 PathTracing 路径。**

- 旧版 WE（约 0.60 之前）确实有 `RenderPath3D_Forward` / `_Deferred` / `_Tiled` 三个子类；本版本的演进方向是**收敛到 tiled forward+ 一套**（opaque 用 forward+ 一次着色，透明用前向混合），并把曾经的 deferred 能力替换为 visibility buffer 计算着色（`visibility_shadeCS.hlsl`）——后者复用了 prepass 的 primitiveID，但**不写 G-buffer**，因此不能算 deferred。
- 该结论的**决定性证据行**：`wiRenderPath3D.h:208`（唯一执行模型开关）、`wiRenderPath3D.cpp:1473`（compute 着色入口）、`wiRenderPath3D.cpp:1544-1582`（主不透明着色唯一分支）、`shaders/visibility_shadeCS.hlsl:25/94`（自述按 primitiveID 逐像素着色且复用 TiledLighting）、`wiRenderPath3D.cpp:1575-1580`（光栅不透明着色只有一套 `drawscene_flags`）。
- **对约束文档 §2.1 的直接回答**：WE 的高层架构不是「用子类表达管线差异」，而是「**一个 RenderPath3D + 运行期特性开关 + 子类整体重写 Render()**」。因此 TooiyumeRender 若想为 Deferred/Forward 分别建 RenderPath 子类，**不能从 WE 抄现成结构**；可抄的是 §0.5 的公共功能层与 §5 的后处理契约。

---

## 5. Q4 —— `RenderPostprocessChain` 的逐效果调用顺序、HDR/LDR 与分界

函数：`void RenderPath3D::RenderPostprocessChain(CommandList cmd) const`（声明 `wiRenderPath3D.h:191`，实现 `wiRenderPath3D.cpp:2334-2564`）。入口先 `BindCommonResources(cmd)` + `BindCameraCB(*camera, camera_previous, camera_reflection, cmd)`（:2338-2339）。

### 5.1 执行顺序表（实测行号）

| # | 效果 | 行号 | 空间/阶段 | ping-pong 参与 |
| --- | --- | --- | --- | --- |
| 0 | `GPUBarrier::Aliasing(&rtPrimitiveID, &rtPostprocess)` | :2345-2346 | — | 别名切换（进入后处理前把 rtPostprocess 从 rtPrimitiveID 的物理内存接管） |
| 1 | **FSR2** `Postprocess_FSR2(fsr2Resources, *camera, rtFSR[1], *rt_read, depthBuffer_Copy, rtVelocity, rtFSR[0], cmd, scene->dt, getFSR2Sharpness())` | :2350-2363 | **HDR** | 是（兼任 TAA；之后 `rt_read=&rtFSR[0]; rt_write=&rtFSR[1]`，:2374-2375；并**重新 BindCameraCB + BindCommonResources**，注释：rebind these, because FSR2 binds other things to those constant buffers，:2365-2372） |
| 2 | **TemporalAA** `Postprocess_TemporalAA(temporalAAResources, *rt_read, cmd)`（条件：`GetTemporalAAEnabled() && !GetTemporalAADebugEnabled() && temporalAAResources.IsValid()` 且非 wireframe） | :2377-2383 | **HDR** | **否** —— 结果放进 `rt_first`：`rt_first = temporalAAResources.GetCurrent();`（:2384），即 TAA 输出作为「下一步读取源」而不占用 ping-pong 对 |
| 3 | **Underwater** `Postprocess_Underwater(rt_first==nullptr ? *rt_read : *rt_first, *rt_write, cmd)`（条件：`scene->weather.IsOceanEnabled()`） | :2386-2396 | **HDR** | 是（用后 `rt_first=nullptr` 并 swap） |
| 4 | **Custom BeforeTonemap**（循环 `custom_post_processes`，`stage==BeforeTonemap`） | :2398-2415 | **HDR**（注释：Before tonemap and bloom in HDR color space，`wiRenderPath3D.h:374`） | 是（每步 swap） |
| 5 | **Depth of Field** `Postprocess_DepthOfField(depthoffieldResources, read, *rt_write, cmd, getDepthOfFieldStrength())` | :2417-2429 | **HDR** | 是 |
| 6 | **Motion Blur** `Postprocess_MotionBlur(scene->dt, motionblurResources, read, *rt_write, cmd, getMotionBlurStrength())` | :2431-2444 | **HDR** | 是 |
| 7 | **Eye adaption** `ComputeLuminance(luminanceResources, read, cmd, getEyeAdaptionRate(), getEyeAdaptionKey())` | :2451-2460 | HDR（**产出 luminance buffer，不改变颜色 RT**） | 否（写入 `luminanceResources.luminance`） |
| 8 | **Bloom** `ComputeBloom(bloomResources, read, cmd, getBloomThreshold(), getExposure(), eyeAdaption ? &luminanceResources.luminance : nullptr)` | :2461-2471 | HDR（**产出 bloom 纹理，不写入主链**） | 否 |
| 9 | **TONEMAP（唯一 HDR→LDR 分界）** `Postprocess_Tonemap(read, *rt_write, cmd, exposure, brightness, contrast, saturation, dither, colorGradingMap, &rtParticleDistortion, luminance(可选), bloom(可选), colorspace, getTonemap(), &distortion_overlay, getHDRCalibration())` | :2473-2490 | **HDR → LDR（显示色空间）** | 是（之后 `rt_first=nullptr; swap`，:2492-2493） |
| 10 | **Custom AfterTonemap**（循环，`stage==AfterTonemap`） | :2498-2514 | LDR | 是 |
| 11 | **Sharpen** `Postprocess_Sharpen(*rt_read, *rt_write, cmd, getSharpenFilterAmount())` | :2516-2521 | LDR | 是 |
| 12 | **FXAA** `Postprocess_FXAA(*rt_read, *rt_write, cmd)` | :2523-2528 | **LDR（在 tonemap 之后）** | 是 |
| 13 | **Chromatic Aberration** `Postprocess_Chromatic_Aberration(*rt_read, *rt_write, cmd, getChromaticAberrationAmount())` | :2530-2535 | LDR | 是 |
| 14 | **CRT** `Postprocess_CRT(*rt_read, *rt_write, cmd, 0, 0, true)` | :2537-2542 | LDR | 是 |
| 15 | **收尾** `lastPostprocessRT = rt_read;` | :2544 | LDR | — |
| 16 | **GUI 背景模糊** `Postprocess_Downsample4x(*rt_read, rtGUIBlurredBackground[0], cmd, hdrToSRGB)` → `Downsample4x([0] → [2])` → `Postprocess_Blur_Gaussian([2],[1],[2],cmd,-1,-1,true)` | :2546-2556 | LDR | 否（旁路输出到 `rtGUIBlurredBackground`） |
| 17 | **FSR1（LDR 末端）** `Postprocess_FSR(*rt_read, rtFSR[1], rtFSR[0], cmd, getFSRSharpness()); lastPostprocessRT = &rtFSR[0];` | :2558-2562 | LDR | 否（输出到 `rtFSR[0]` 并**改写** `lastPostprocessRT`） |

### 5.2 关键契约结论

- **唯一的 HDR→LDR 分界是 `Postprocess_Tonemap`（:2473）**。它前面的 1-8 步全在 HDR；后面 10-17 步全在显示色空间。分界的实现级证据在着色器：`shaders/tonemapCS.hlsl:95-194` 里 `if (flags & TONEMAP_FLAG_SRGB)` 才做 ACES/Uchimura tonemap + `ApplySRGBCurve_Fast`，否则 `result *= hdr_calibration`（HDR10 直通路径）。标志位定义 `shaders/ShaderInterop_Postprocess.h:129-135`。
- **FXAA 在 tonemap 之后**（:2523 vs :2473），且 FXAA 之后还叠了色差与 CRT，说明 WE 把「显示后效果」统一放在链尾。
- **Bloom 与 Eye adaption 不在链内做合成，而是作为 tonemap 的输入**（:2449-2450 注释：Bloom and eye adaption is not part of post process chain, because they will be applied to the screen in tonemap）。这是明确的性能/契约取舍：减少 1-2 次全屏 ping-pong，代价是 tonemap 这一个 compute 承担曝光、泛光合成、tonemap、色空间转换、3D LUT 调色、抖动、亮度/对比度/饱和度。
- **dither 与 color grading 都不是独立 pass**：前者作为 `TONEMAP_FLAG_DITHER` 位（:2473-2490 第 7 个实参 `getDitherEnabled()`），后者作为 3D LUT 纹理（`colorGradingMap`）。
- **outline / light shafts / volumetrics 不在本函数内**：outline 在主 RenderPass 里执行 `RenderOutline(cmd)`（`wiRenderPath3D.cpp:1586`，对 `rtOutlineSource` 就地读写，实现 :1909-1921）；light shafts 与 volumetrics 在透明前执行 `RenderLightShafts(cmd)`（:1747）与 `RenderVolumetrics(cmd)`（:1749），结果在 `RenderTransparents` 内以加性/双边上采样并回 `rtMain`（:2246-2270）。→ **「后处理链」严格指 tonemap 及其后处理，不包含这些主 pass 内的效果**（它们的输入是深度/stencil/体积光 RT 而非彩色图）；TooiyumeRender 若要「统一调度全部效果」，需要先决定是否收编这些效果，WE 的答案是不收编。

### 5.3 tonemap 的输入输出契约（实测，约束原则④的直接范本）

```cpp
// WickedEngine/wiRenderer.h:925-942
void Tonemap(
  const Texture& input,
  const Texture& output,
  CommandList cmd,
  float exposure, float brightness, float contrast, float saturation,
  bool dither,
  const Texture& texture_colorgradinglut,
  const Texture& texture_distortion,
  const GPUBuffer* buffer_luminance,
  const Texture& texture_bloom,
  wi::graphics::ColorSpace display_colorspace = wi::graphics::ColorSpace::SRGB,
  Tonemap tonemap = Tonemap::Reinhard,
  const Texture& texture_distortion_overlay = Texture(),
  float hdr_calibration = 1);
```

- 可选输入用**空纹理/负索引**表达：`shaders/ShaderInterop_Postprocess.h:136-150 struct PushConstantsTonemap` 用 `int texture_input / buffer_input_luminance / texture_input_distortion / texture_colorgrade_lookuptable / texture_bloom / texture_output / texture_input_distortion_overlay`，着色器内以 `>= 0` 判定是否启用（bindless 描述符表 + 负值哨兵）。
- 分辨率契约：`float2 resolution_rcp`（push constant），且 `shaders/tonemapCS.hlsl:95-100` 用 `GetCamera().is_uv_inside_scissor(uv)` 之外直接 `return` —— 注释明确这是为了支持 **upscaled resolution（FSR 2）**。→「输入分辨率 ≠ 显示分辨率」被显式建模。
- 枚举 `Tonemap{Reinhard, ACES, Uchimura}`（`wiRenderer.h:919-924`）通过 flags 位传给 CS（`TONEMAP_FLAG_ACES` / `TONEMAP_FLAG_UCHIMURA`，`ShaderInterop_Postprocess.h:131/134`）。
- **代价评估**：一个 compute 14 个参数、内嵌多个可选输入分支，是「效果好但接口粗」的典型。TooiyumeRender 若采用，建议改为传 `TonemapSettings` POD 结构体；并保留「dither/colorGrading 内嵌于 tonemap」以省 pass，但**在文档里显式登记**（约束原则④要求每效果明确输入/输出/分辨率/格式/资源状态/顺序——合并实现仍然满足，但必须写在契约表里）。

---

## 6. Q5 —— ping-pong 的具体实现与别名规避

### 6.1 三个指针 + 一次别名切换（实测）

```cpp
// WickedEngine/wiRenderPath3D.cpp:2341-2346
const Texture* rt_first = nullptr; // not ping-ponged with read / write
const Texture* rt_read  = &rtMain;
const Texture* rt_write = &rtPostprocess;
// rtPostprocess aliasing transition:
device->Barrier(GPUBarrier::Aliasing(&rtPrimitiveID, &rtPostprocess), cmd);
```

- 初始态：`rt_read = &rtMain`（主场景彩色结果）、`rt_write = &rtPostprocess`（后处理临时 RT）。
- 每一步「读 A 写 B」结束后 `std::swap(rt_read, rt_write)`（:2396、:2415、:2429、:2444、:2493、:2521、:2528、:2535、:2542 各步之后），并显式 `rt_first = nullptr;`（:2396、:2415、:2493）以取消 TAA/自定义效果的旁路输出。
- `rt_first` 是**唯一的旁路指针**：当 TAA（:2384）或自定义效果把结果写到 ping-pong 对之外时，下一步读它（`rt_first==nullptr ? *rt_read : *rt_first`，:2386），随后立刻置空并继续 ping-pong。
- 结束时把「最后一次写入的那个」暴露出去：`lastPostprocessRT = rt_read;`（:2544）；FSR1 启用时改写成 `&rtFSR[0]`（:2562）。

### 6.2 mutable 成员 + const 虚函数：为什么这样写

```cpp
// WickedEngine/wiRenderPath3D.h:175-180
// Post-processes are ping-ponged, this function helps to obtain the last postprocess render target that was written
mutable const wi::graphics::Texture* lastPostprocessRT = &rtPostprocess;
const wi::graphics::Texture* GetLastPostprocessRT() const { return lastPostprocessRT; }
```

- **形态**：9 个渲染阶段虚函数与 `Render()` 全部是 `const`（`wiRenderPath3D.h:184-192`、:388），但它们要改缓存指针（`lastPostprocessRT`）、帧计数（`temporalAAResources.frame`）、渐变量（`mutable float lightShaftsFadeFactor`，`wiRenderPath3D.h:182`），因此这些成员被声明为 `mutable`。
- **设计依据（实测注释）**：`wiRenderPath3D.h:175` 直接说明用途 —— 供 `Compose()`/外部查询「最后一个被写入的后处理 RT」。唯一消费者是 `wiRenderPath3D.h:235 GetRenderResult3D() const { return *GetLastPostprocessRT(); }`（注释：The 3D scene rendering without 2D overlay that is used in Compose() to combine the final image）。
- **代价**：`const` 在这里表达的是「不改变本对象的**外部可观察配置**（不 resize、不创建资源）」，不是「逻辑常量」。API 读起来干净（`Render()` 可在 const 上下文调用），但把「一帧内会变的缓存状态」藏进了 const 成员函数。对教学项目：**可借鉴但必须写注释**，否则读者会误以为 `Render()` 无副作用。

### 6.3 ping-pong 出错会怎样：别名冲突如何被规避

- **风险根源**：`rtPostprocess` **不是**独立分配的内存 —— 它与 `rtPrimitiveID`、`rtSceneCopy` 共享同一块别名内存（见 §8.2），所以「后处理开始」必须先把这块内存从原主人手里接管，这一步就是 :2345-2346 的 `GPUBarrier::Aliasing(&rtPrimitiveID, &rtPostprocess)`。
- **若漏掉该 barrier**：`rtPrimitiveID`（prepass 产物，`R32_UINT`）与 `rtPostprocess`（`R11G11B10_FLOAT`）在 GPU 上指向同一物理地址，后处理第一次写 `rtPostprocess` 时会污染 prepass 结果；而 `rtPrimitiveID` 仍是 `visibility_shading_in_compute` 路径的输入 → 表现为**画面随机闪烁/材质错乱，且单帧内自洽**（命令顺序不定时不报错）。WE 的对策是**纪律 + 注释**：在 `Render()` 的 job1 barrier 列表（:874-877）与后处理入口（:2345-2346）各写一次，形成「谁用谁接管」的成对约定。
- **若 ping-pong 自身写错**（同一步读写同一张纹理）：后处理 compute 会读写同一 UAV/SRV，DX12 下属于**未定义行为**（调试层可报 hazard）。WE 通过「初始 `rt_read=&rtMain`、`rt_write=&rtPostprocess`、每步无条件 swap」的**机械规则**规避：只要每个效果严格「读 read 写 write」，同帧内读写的物理资源必然不同。
- **反例（重要）**：`Postprocess_Outline` 就对 `rtOutlineSource` **就地读写**（`wiRenderPath3D.cpp:1909-1921`，无输出参数），`RenderSceneMIPChain` 也是原地生成 mip 链（:2055-2083）—— 这两个效果靠**自身 shader 的读写顺序**保证正确，不享受 ping-pong 保护。这就是「按需 Ping-Pong」的真正边界（约束原则④的实现注脚）。
- **别名与 ping-pong 的关系（实测）**：别名组把 `rtPostprocess / rtPrimitiveID / rtSceneCopy` 绑在一起（`wiRenderPath3D.cpp:164-166`），而 ping-pong 对是 `rtMain`（独立分配，:227）与 `rtPostprocess`（别名，:171）。因此 ping-pong **从不与 rtMain 互为别名**，`rtMain` 可以安全地在链中反复作为只读源（这也是 `rt_read` 初值取 `rtMain` 的前提）。

---

## 7. Q6 —— 「按效果划分 Resources 结构体」模式

### 7.1 三个结构体的封装内容（逐字段实测）

**BloomResources**（`wiRenderer.h:413-417`）：
```cpp
struct BloomResources
{
  wi::graphics::Texture texture_bloom; // 四分之一分辨率的 mip 链（链内被逐级模糊）
  wi::graphics::Texture texture_temp;  // 模糊用临时图
};
```
- 创建：`CreateBloomResources(BloomResources& res, XMUINT2 resolution)`（`wiRenderer.h:418`，实现 `wiRenderer.cpp:11416-11438`）：`R11G11B10_FLOAT`、`width/4 × height/4`、`mip_levels = std::min(5u, (uint32_t)std::log2(std::max(width, height)))`；随后对每个 mip 显式 `device->CreateSubresource(tex, SubresourceType::SRV/UAV, 0, 1, i, 1)` 并 **`assert(subresource_index == i)`**（`wiRenderer.cpp:11432-11438`）—— 即**假定 subresource 序号必须与 mip 索引一致**，一旦不一致直接断言失败。
- 使用：`ComputeBloom(res, input, cmd, threshold=1.0f, exposure=1.0f, const GPUBuffer* buffer_luminance=nullptr)`（`wiRenderer.h:419-426`）。
- 创建/重建调用点：`wiRenderPath3D.cpp:3170`（`setBloomEnabled` 内，为 false 时 `bloomResources = {}`）；PathTracing 侧 `wiRenderPath3D_PathTracing.cpp:135`。

**TemporalAAResources**（`wiRenderer.h:890-898`）：
```cpp
mutable int frame = 0;
wi::graphics::Texture texture_temporal[2];
bool IsValid() const { return texture_temporal[0].IsValid(); }
const Texture* GetCurrent() const { return &texture_temporal[frame % arraysize(texture_temporal)]; }
const Texture* GetHistory() const { return &texture_temporal[(frame + 1) % arraysize(texture_temporal)]; }
```
- **封装了「双缓冲历史 + 帧序号」这一跨帧状态**；`Postprocess_TemporalAA(res, input, cmd)` 的**输出是隐含的**（`res.GetCurrent()`），输入是外部纹理（`wiRenderer.h:900-904`）。
- 创建：`CreateTemporalAAResources(res, XMUINT2 resolution)`（:899，实现 `wiRenderer.cpp:17157-…`）：`R11G11B10_FLOAT`、全分辨率、SRV|UAV，两张。
- 创建/销毁调用点：`wiRenderPath3D.cpp:433`（`PreRender` 内惰性创建，条件 `TemporalAA && !wireframe`）；`:438`（条件不满足时 `temporalAAResources = {}` 释放）。

**SSAOResources**（`wiRenderer.h:585-588`）：
```cpp
struct SSAOResources
{
  wi::graphics::Texture temp; // 半分辨率 AO 临时图
};
```
- 创建：`CreateSSAOResources(res, XMUINT2 resolution)`（:589，实现 `wiRenderer.cpp:12841-…`）：`R8_UNORM`、**`width/2 × height/2`**、`UNORDERED_ACCESS|SHADER_RESOURCE`、`layout = SHADER_RESOURCE_COMPUTE`。
- 使用：`Postprocess_SSAO(res, output, cmd, range=1, samplecount=16, power=1)`（:590-597）与 `Postprocess_HBAO(res, camera, output, cmd, power=1)`（:598-604）—— **两个算法共用同一个 `SSAOResources`**（都只需一张半分辨率临时图）；MSAO 因需要 12 张 4 级降采样图而独享 `MSAOResources`（:605-627），RTAO 独享 `RTAOResources`（:636-…）。
- 创建/销毁调用点：`wiRenderPath3D.cpp:2791-2810`（`setAO` 内先清空 `rtAO / ssaoResources / msaoResources / rtaoResources`，再按 `getAO()` 分支重建）、`:2848 CreateSSAOResources`、`:2851 CreateMSAOResources`、`:2854 CreateRTAOResources`。

**共同模式（实测归纳）**：结构体只封装 ①中间 RT ②跨帧小状态（`frame`、`cleared`）③有效性/缓冲查询（`IsValid` / `GetCurrent` / `GetHistory`）；**分辨率与格式不进结构体**（只在 `Create*` 入参里）；**常量缓冲不进结构体**（走 bindless 描述符表：`wiRenderPath3D.cpp:728-794` 把各资源描述符索引写进 `CameraComponent`）。创建/销毁/Resize 都发生在**使用它的 RenderPath** 里（§7.2、§8.3）。

### 7.2 声明 / 创建 / 销毁调用点一览（实测）

| 资源结构体 | 声明（wiRenderer.h） | Create 实现（wiRenderer.cpp） | RenderPath3D 内创建点 | 销毁点 |
| --- | --- | --- | --- | --- |
| `TiledLightResources` | :385-390 | :10158 | `wiRenderPath3D.cpp:322`（Resize）、:2661（render_to_texture 局部）、:3144（平面反射） | `DeleteGPUResources` :60 |
| `LuminanceResources` | :400-403 | :11338 | `wiRenderPath3D.cpp:3082`（setEyeAdaptionEnabled）；PathTracing :134 | :82 区 |
| `BloomResources` | :413-417 | :11416 | `wiRenderPath3D.cpp:3170`（setBloomEnabled）；PathTracing :135 | :76 |
| `SSAOResources` | :585-588 | :12841 | `wiRenderPath3D.cpp:2848`（setAO 分支） | :60-82 区统一置空（setAO 开头 :2791-2810 再清一次） |
| `MSAOResources` | :605-627 | — | `wiRenderPath3D.cpp:2851` | 同上 |
| `TemporalAAResources` | :890-898 | :17157 | `wiRenderPath3D.cpp:433`（PreRender 惰性） | :438（条件不满足时置空） |
| `FSR2Resources` | :950-997 | :17593 | `wiRenderPath3D.cpp:3016`（setFSR2Enabled） | :80 区 |
| 其余 | `VisibilityResources` :434、`SurfelGIResources` :500、`VXGIResources` :539、`RTAOResources` :636、`RTDiffuseResources` :657、`SSGIResources` :675、`RTReflectionResources` :694、`SSRResources` :716、`RTShadowResources` :744、`ScreenSpaceShadowResources` :764、`DepthOfFieldResources` :783、`MotionBlurResources` :819、`AerialPerspectiveResources` :842、`VolumetricCloudResources` :851、`MeshBlendResources` :1046 | — | 各自 setter / PreRender 惰性段（:450-638、:676-726） | `DeleteGPUResources` :60-82 |

（共 23 个按效果结构体；`RenderPath3D` 成员实例列表见 `wiRenderPath3D.h:145-169`。）

### 7.3 与「一个大 Renderer 类持有全部资源」的对比 + 对 TooiyumeRender 的适用性

| 维度 | 按效果结构体（WE 现状，实测） | 一个大 Renderer 类 |
| --- | --- | --- |
| 依赖面 | 使用方只 include `wiRenderer.h`，**按需**在成员里放要用的结构体：`RenderPath3D` 放了 24 个（`wiRenderPath3D.h:145-169`），而 `PathTracing` 只放它需要的 2 个（`wiRenderPath3D_PathTracing.cpp:134-135` 只复用 luminance/bloom） | 全部资源与全部路径耦合，任何路径都要拿到整个渲染器 |
| 生命周期 | 结构体随路径成员一起 `= {}` 释放（`wiRenderPath3D.cpp:60-82`）；**创建可惰性延迟**（PreRender 内 :450-638 全是「需要才建」） | 创建/销毁集中在构造/析构，难做「按开关惰性」 |
| 组合复用 | `SSAOResources` 被 SSAO 与 HBAO 两算法共用（:590-604）；`BloomResources`/`LuminanceResources` 被 RenderPath3D 与 PathTracing 共用 | 复用靠继承+条件，容易长成巨型类 |
| 缺点 | **资源不集中**：24 个成员 + 24 个创建点，靠命名与 setter 纪律维护；新人难判断谁在用哪个 | 巨型类可读性同样差，但资源集中便于调试 |

**对 TooiyumeRender 的判断（依据任务给定前提：约 1-3 万行、单 D3D12 后端、教学为主、现有 Deferred 管线）**：
- **必需**：按效果划分的资源结构体 + 「`Create*` 只申请、`<Effect>(res, in, out, cmd)` 只使用」的命名纪律。低成本高收益：GTarget/Bloom/SSAO/TAA 各一个结构体，就能把「谁申请、谁释放、什么分辨率」写清楚（约束原则④）。
- **必须简化**：不要一次照搬 23 个。建议**按需增长**：只为现有 Deferred 管线真正用到的效果建结构体（GBuffer 组、Bloom、Tonemap 输入、可选 SSAO），每加一个效果再加一个。WE 的 23 个是被 12+ 种效果的真实复用基数逼出来的，教学项目没有这个基数。
- **规模不匹配**：`FSR2Resources`（`wiRenderer.h:950-997`：内嵌 `Fsr2Constants` 20+ 字段 + 15 张纹理 + `GetJitter()`）属于「引入第三方 upscaler 才需要」的复杂度，**不建议借鉴其内部结构**；但可借鉴其**接口形态**（`IsValid()` + `GetJitter()` + 一个 `Postprocess_FSR2`）。同理 `VXGIResources`/`SurfelGIResources` 对教学项目完全不适用。

---

## 8. Q7 —— 资源生命周期与 GPU 同步

### 8.1 纹理创建 / 销毁调用点与延迟释放

- **创建**集中在两处：`RenderPath3D::ResizeBuffers`（`wiRenderPath3D.cpp:96-341`，全部主 RT 与 depth buffer 在此按新分辨率重建）与 `PreRender`（:375-830）内的**惰性创建段**（`rtWaterRipple` :523-539、`reprojectedDepth` :560-585、`rtVelocity` :587-617、`rtShadow` :619-638、DoF :676-695、MotionBlur :697-713、TAA :431-434、meshblend :815-825）。
- **销毁**：全部写成 `texture = {};`（`DeleteGPUResources`，:17-94），**没有** `DestroyTexture` 之类的显式 API。实测机制：`GPUResource{ wi::allocator::shared_ptr<void> internal_state; }`（`wiGraphics.h:861-863`），`GPUBuffer`(:893)/`Texture`(:901) 均持 `desc`；释放靠 **shared_ptr 引用计数**，最后一个引用析构时归还。
- **延迟释放提示**：`RenderPath2D::DeleteGPUResources` 末尾 `GetDevice()->FlushDeallocations()`（`wiRenderPath2D.cpp:20`，紧接 `RenderPath3D::DeleteGPUResources()` 之后被调，`wiRenderPath3D.cpp:93`），`GraphicsDevice::FlushDeallocations`（`wiGraphicsDevice.h:440-448`）实现为连续 `BUFFERCOUNT` 次 `SubmitCommandLists()` —— 即**通过推进帧来确保 GPU 不再使用旧资源**，而不是等待 fence。这是「资源生命周期与 GPU 执行同步」的一个**薄弱点**（约束原则⑤）：`= {}` 与 `FlushDeallocations()` 之间没有显式 fence 语义，靠提交次数兜底。

### 8.2 内存别名：`aliasingAllocation` + `offset_*` 的机制与约束（实测）

**机制**：
1. `GraphicsDevice::CreateAliasingAllocator()`（`wiGraphicsDevice.h:500-506`）只设置内部 `device` 指针，返回一个轻量 `AliasingAllocator`。
2. `AliasingAllocator::Add(const TextureDesc* desc)`（:459-469）：用 `device->GetDeviceTextureMemoryRequirements(desc)` 取该纹理的 size/alignment，**对齐后返回当前 offset 并累加 size**；同时 `rt_ds |= (desc->bind_flags & (RENDER_TARGET|DEPTH_STENCIL))`。
3. `AliasingAllocator::Add(std::initializer_list<const TextureDesc*> descs)`（:471-483）：取**同组最大** size/alignment，**返回同一个 offset** —— 同组多张纹理共享同一段内存。
4. `Commit()`（:485-497）：按 `size + alignment` 创建一块 `GPUBuffer`，`misc_flags = rt_ds ? ResourceMiscFlag::ALIASING_TEXTURE_RT_DS : ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS`。
5. 调用方用**带 offset 的 `CreateTexture`** 把这些纹理建到该 buffer 的对应偏移上：实测 `wiRenderPath3D.cpp:171`（rtPostprocess）、:176（rtPrimitiveID）、:196（rtSceneCopy_tmp）、:204（rtParticleDistortion）、:532（rtWaterRipple）。

**RenderPath3D 的实际分组**（`wiRenderPath3D.cpp:108-166`）：
```cpp
GraphicsDevice::AliasingAllocator aliasing_allocator = device->CreateAliasingAllocator();
// 组1：后处理临时区
offset_rtPostprocess = offset_rtPrimitiveID = offset_rtSceneCopy
  = aliasing_allocator.Add({&desc_rtPostprocess, &desc_rtPrimitiveID, &desc_rtSceneCopy});
// 组2：半/低分辨率临时区
offset_rtParticleDistortion = offset_rtWaterRipple = offset_rtAO
  = aliasing_allocator.Add({&desc_rtParticleDistortion, &desc_rtWaterRipple, &desc_rtAO});
aliasingAllocation = aliasing_allocator.Commit();
```
（描述符：`desc_rtPostprocess` 全分辨率 `R11G11B10_FLOAT`、`desc_rtPrimitiveID` `R32_UINT` 全分辨率、`desc_rtSceneCopy` 主格式 1/4 分辨率且 `mip_levels = min(8, log2(max(w,h)))`；`desc_rtParticleDistortion` `R16G16_FLOAT` 全分辨率、`desc_rtWaterRipple` `R16G16_FLOAT` 1/8 分辨率、`desc_rtAO` `R8_UNORM` 全分辨率，详见 `wiRenderPath3D.cpp:111-161`。）

**约束（实测推出）**：
- 同组内**同一时刻只能有一个资源在用**；尺寸由组内最大者决定（`Add(initializer_list)` 语义），因此「小图很省内存」只体现在**组间**复用，组内是「取最大」。
- 组内有任何 RT/DS 绑定的资源时，整块内存进 `ALIASING_TEXTURE_RT_DS` 堆（`wiGraphicsDevice.h:486-497`）——即**别名堆的类型由组内最严者决定**。
- 切换使用者必须显式 barrier：`GPUBarrier::Aliasing(before, after)`（`wiGraphics.h:706-770` 区的 `Type::ALIASING`，:754 静态构造）。实测使用点：`wiRenderPath3D.cpp:874-877`（rtPostprocess ← rtPrimitiveID）、:1596-1599（rtAO ← rtParticleDistortion）、`RenderAO` :1828-1831（反向）、`RenderTransparents` :2088-2104（rtWaterRipple ← rtParticleDistortion）、`RenderSceneMIPChain` :2055-2083（rtSceneCopy_tmp ← rtPrimitiveID，且与 image layout barrier 成对）、`RenderPostprocessChain` :2345-2346。

### 8.3 barrier / 资源状态转换的责任方划分（实测）

| 范围 | 责任方 | 证据 |
| --- | --- | --- |
| **render pass 内**的 RT/DS 状态与 load/store | **后端自动** | `wiGraphicsDevice.h:216-219` 纯虚 `RenderPassBegin(SwapChain*, cmd)` / `RenderPassBegin(RenderPassImage*, count, cmd, flags)` / `RenderPassEnd(cmd)`；DX12 实现把 `LoadOp` 映射到 `D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_{PRESERVE,CLEAR,DISCARD}`、`StoreOp` 映射到 `ENDING_ACCESS_TYPE_{PRESERVE,DISCARD}`（`wiGraphicsDevice_DX12.cpp:6114-6137`），并生成 begin/end barrier 后随命令列表提交（:6323/:6331 push_back barrierdesc；:6375-6377 `ResourceBarrier(...)`） |
| **pass 之外**的 image layout 转换 | **调用方显式** | `GPUBarrier::Image(texture, before, after, mip=-1, slice=-1, aspect=nullptr)`（`wiGraphics.h:706-770` 区）；实测往返用法 `wiRenderPath3D.cpp:2055-2083`（rtSceneCopy_tmp → UNORDERED_ACCESS → 复原） |
| buffer 状态转换（如 tiled 列表 SRV↔UAV） | **调用方显式** | `wiRenderer.cpp:10173` 内 `Barrier(Buffer entityTiles SRV→UAV)` 与回程 |
| UAV 写后读依赖 | **调用方显式** | `GPUBarrier::Memory(resource)`（注释：UAV accesses）；实测 `wiRenderPath3D.cpp:1041` 附近 `Visibility_Prepare` 后接 `ComputeTiledLightCulling`，barrier 由各 `Postprocess_*`/`Visibility_*` 内部或调用点发出 |
| 内存别名切换 | **调用方显式** | `GPUBarrier::Aliasing(...)`（见 §8.2 调用点清单） |
| 资源释放与 GPU 完成 | **shared_ptr 引用计数 + `FlushDeallocations()`** | `wiGraphics.h:861-863`；`wiGraphicsDevice.h:440-448`；`wiRenderPath2D.cpp:20` |

### 8.4 是否「用抽象隐藏了同步关系」（对照约束原则⑤）

**部分隐藏，且隐藏的那部分恰好是安全的；不安全的部分没有被隐藏。**

- **安全的部分（后端自动）**：`RenderPassBegin` 把「RT/DS 的 load/store 与状态」封装掉了，调用方只表达「LOAD 还是 CLEAR」（例如 `wiRenderPath3D.cpp:1526-1529` 的 `visibility_shading_in_compute ? LOAD : CLEAR`），不必写 D3D12 的 barrier —— 这符合原则③「避免 D3D12 细节泄漏」。
- **不安全的部分（调用方负责）**：别名切换、image layout、UAV 依赖**全都是裸 `GPUBarrier` 调用**，没有任何断言或状态跟踪来证明「接管者与被接管者不再同时使用」。一旦漏写，行为是**未定义但往往不报错**（§6.3）。这正是原则⑤警告的「为追求抽象而隐藏重要 GPU 同步关系」的**反面**：WE 没有隐藏，而是**把责任显式交给了人**，代价是正确性依赖纪律与注释（`wiRenderPath3D.cpp:2345` 那行注释就是这种纪律的产物）。
- **对 TooiyumeRender 的判断**：`AliasingAllocator` 这套机制（约束原则⑤的对照组）**判定为规模不匹配 / 可选后置**。理由：①收益是显存占用，教学项目瓶颈不在此；②成本是引入「同组互斥 + 显式 Aliasing barrier」这一隐式不变式，而 TooiyumeRender 现有 RHI 未必有 `ALIASING_TEXTURE_RT_DS` 这类 misc flag 支持；③若要做，也应先建立**资源状态跟踪 + 调试断言**（哪张图当前 owner 是谁），再谈别名收益。**必需**借鉴的是：`RenderPassBegin` 式的 load/store 声明（让 pass 只表达「保留/清除」），以及**把 barrier 写在效果函数内部、与效果绑定**的纪律（WE 的 `Postprocess_*` 大多自带 barrier，而非散落在 RenderPath 里）。

---

## 9. Q8 —— RenderPath3D 的虚函数扩展面与 PathTracing 的覆写现实

### 9.1 扩展面清单（声明与基类默认实现）

9 个虚函数全部 `const`，声明在 `WickedEngine/wiRenderPath3D.h:184-192`：

| 虚函数 | 声明 | 基类默认实现（行号） | 默认实现做了什么（实测） | 谁调用它 |
| --- | --- | --- | --- | --- |
| `RenderAO(CommandList)` | :184 | `wiRenderPath3D.cpp:1826-1880` | 先 `if (rtAO.IsValid()) Barrier(Aliasing(&rtParticleDistortion,&rtAO))`；按 `getAO()` 分派 `Postprocess_SSAO` / `_HBAO` / `_MSAO` / `_RTAO` 写入 `rtAO`；`AO_DISABLED` 不做事 | 基类 `Render()` :1091 |
| `RenderSSR(CommandList)` | :185 | :1881-1893 | `getSSREnabled() && !getRaytracedReflectionEnabled()` 时 `Postprocess_SSR(ssrResources, rtSceneCopy, rtSSR, cmd, cutoff)` | 基类 `Render()` :1102 |
| `RenderSSGI(CommandList)` | :186 | :1894-1908 | `getSSGIEnabled()` 时 `Postprocess_SSGI(ssgiResources, rtSceneCopy, depthBuffer_Copy, visibilityResources.texture_normal_roughness, rtSSGI, cmd, depthRejection)` | 基类 `Render()` :1104 |
| `RenderOutline(CommandList)` | :187 | :1909-1921 | `getOutlineEnabled()` 时 `Postprocess_Outline(rtOutlineSource, cmd, threshold, thickness, color)` —— **就地读写，无输出参数** | 基类 `Render()` :1586（在主 RenderPass 内） |
| `RenderLightShafts(CommandList)` | :188 | :1922-2029 | 计算 `sunDotCamera`，维护 `mutable lightShaftsFadeFactor`（`Lerp(factor, target, 1-exp(-fadeSpeed*dt))`），按 stencil 把太阳画进 `rtSun[0]`（MSAA 时 `rtSun_resolved`），降采样到 `rtSun[2]`，再 `Postprocess_LightShafts(rtSun[2], rtSun[1], cmd, sun, strength)` | 基类 `Render()` :1747；PathTracing :485 |
| `RenderVolumetrics(CommandList)` | :189 | :2030-2054 | `RenderPass(rtVolumetricLights, CLEAR)` + `DrawVolumeLights(visibility_main, cmd)`；其上采样在 `RenderTransparents` 内（:2246-2258） | 基类 `Render()` :1749；PathTracing :483 |
| `RenderSceneMIPChain(CommandList)` | :190 | :2055-2083 | `Aliasing(&rtPrimitiveID,&rtSceneCopy_tmp)` + `ClearUAV` + `Postprocess_Downsample4x(rtMain, rtSceneCopy, cmd)` + `MIPGEN_OPTIONS{mipopt.gaussian_temp=&rtSceneCopy_tmp}` + `GenerateMipChain(rtSceneCopy, MIPGENFILTER_GAUSSIAN, cmd, mipopt)`（供折射/粗糙反射采样） | 基类 `RenderTransparents` 内 :2189（条件 `visibility_main.IsTransparentsVisible()`） |
| `RenderTransparents(CommandList)` | :191 | :2084-2332 | 水波纹 → （FSR2 时 `rtMain→rtFSR[1]` 拷贝并交换 layout barrier，:2106-2125）→ `UpdateGaussianSplatsForCamera` → 主 RenderPass(LOAD) 内：海洋 `DrawScene(...,DRAWSCENE_OCEAN)`、`RenderSceneMIPChain`、透明 `DrawScene(...,TRANSPARENT\|TESSELLATION\|OCCLUSIONCULLING\|MAINCAMERA)`、`DrawDebugWorld`/`DrawWireframeOverlay`/`DrawLightVisualizers`/`DrawSoftParticles`/`DrawGaussianSplats`/`DrawSpritesAndFonts`，体积光双边上采样、光柱加性合成、`DrawLensFlares`、畸变粒子，最后 `Postprocess_Downsample4x(rtMain, rtSceneCopy)` | 基类 `Render()` :1751 |
| `RenderPostprocessChain(CommandList)` | :192 | :2334-2564 | 见 §5 全表 | 基类 `Render()` :1763；**PathTracing 未调用** |

### 9.2 PathTracing 覆写了什么、未覆写时发生什么（实测）

`WickedEngine/wiRenderPath3D_PathTracing.h:8` / `.cpp`：

**覆写**：`~RenderPath3D_PathTracing`（:45，`wi::jobsystem::Wait(denoiserContext)`）、`ResizeBuffers()`（:50）、`Update(float dt)`（:145，内部 :191 调 `RenderPath3D::Update(dt)`）、`Render() const`（:302）、`Compose(cmd) const`（:714）；另有两个 `#ifdef OPENIMAGEDENOISE` 版本的 `isDenoiserAvailable()`（:40/:42）。

**不覆写**：§9.1 的 9 个 `Render*` 虚函数**一个都没覆写**。但由于**它覆写了 `Render()`**，基类 `Render()` 这个「模板方法」不会执行，因此基类那 9 个默认实现**不会被自动调用**；唯一被显式调用的是 `RenderVolumetrics(cmd)`（:483）与 `RenderLightShafts(cmd)`（:485）。

**后果（实测对比）**：
- **复用成功的地方**：`RenderVolumetrics` / `RenderLightShafts` 被直接复用；`CreateLuminanceResources` / `CreateBloomResources` 被直接复用（`.cpp:134-135`）；`RayTraceScene(..., &depthBuffer_Main, &rtPrimitiveID)`（:390-405）复用基类 RT；`ResizeBuffers` 里显式跳过基类 RT（:142 注释：we don't need to use any buffers from RenderPath3D, so skip those）。
- **复用失败的地方**：整条后处理链在 PathTracing 里**重写了一遍**（`.cpp:583-707`），与基类 `RenderPostprocessChain`（`wiRenderPath3D.cpp:2334-2564`）在效果顺序、条件判断、`lastPostprocessRT` 语义上高度重复但**并非同一实现**（基类有 FSR2/FSR1/TAA 分支与 `rt_first` 语义，PathTracing 没有 FSR；且 PathTracing 的 GUI 模糊读的是 `rtPostprocess`（:699）而非 `rt_read`，与基类 `:2551` 读 `*rt_read` 不同）。
- **架构含义**：WE 的「虚函数扩展面」在**真正换 RT 管线**时不够用 —— PathTracing 只能整体重写 `Render()` 并复制后处理链。这正好印证约束文档原则②的后半句（允许确实不同的算法保留专用实现），也暴露了原则①/②的风险：**一旦路径重写 `Render()`，公共功能的复用就退化为复制代码**。

**对 TooiyumeRender 的判断**：
- **必需**：保留 `RenderAO/RenderSSR/.../RenderPostprocessChain` 这类**阶段钩子**，并且**把「后处理链」也做成可单独复用的组件**（例如 `PostProcessChain::Execute(cmd, in, out)` 或一个接收效果列表的调度器），而不是让每条路径复制链。这是 WE 的教训中最值得学的一条。
- **可选**：`RenderPath2D` 与 `RenderPath` 的两级基类（2D 叠层与 GUI 独立于 3D）对本项目有价值（GUI/叠层不必随 3D 路径重写），但可以合并为一级。
- **规模不匹配**：`RenderSceneMIPChain`（专为折射/粗糙反射的降采样链）与 `RenderLightShafts`（太阳 stencil + 径向模糊）属于本项目现阶段不需要的效果，不必模仿其内部实现。

---

## 10. Q9 —— 对 TooiyumeRender 的规模适用性总表

前提（任务给定，非本次实测）：TooiyumeRender ≈ 1-3 万行、单 D3D12 后端、教学为主、已有 Deferred 管线；现状审计见 `02_架构现状审计_audit-tooiyume.md`（他人在写）。

| WE 设计 | 解决了什么问题 | 设计依据（实测） | 替代方案与代价 | TooiyumeRender 适用性 |
| --- | --- | --- | --- | --- |
| `wi::renderer` 自由函数 + 参数化依赖 | 公共绘制能力与「谁编排一帧」解耦 | `grep RenderPath = 0`、`grep Application = 0`、`grep GetScene() = 0`（§2.2） | 大 Renderer 类：前置条件可固化，但资源与路径纠缠 | **必需**。这是原则②最小成本的落地形式：把现有 DeferredRenderer 里的「画场景/画天空/后处理单效果」抽成无状态自由函数或纯工具类，入参用 Scene/相机/CommandList |
| `DRAWSCENE_FLAGS` 与 `RENDERPASS` 两轴分离 | CPU 端筛选 vs 输出语义各司其职 | `wiRenderer.cpp:7179-7283`、`:1947`、`:2147`（§3） | 只用 flags 或只用 renderPass：会互相污染（筛选影响 PSO、或 pass 影响对象集合） | **必需**（概念上）。教学项目可用更少的枚举（主色/prepass/阴影）与更少的 flag 位起步，但**两轴分离的思想要保留** |
| tiled forward+ 光列表（32×32 tile + bucket buffer + Harada 2.5D） | 大量光源下的每像素光照成本 | `wiRenderer.cpp:10158/10173`、`lightCullingCS.hlsl`、`shadingHF.hlsli:49`（§4.3） | 逐光源前向：光源多时爆炸；GPU-driven 剔除：更复杂 | **可选 / 后置**。现有 Deferred 管线本身已解决多光源问题；若教学目标是「公共功能层分离」，tiled 可以延后。若要做，可先做 **简化版**（不减 bucket、不做 2.5D 切片、单相机） |
| `visibility_shading_in_compute`（visibility buffer + CS 着色） | 降低带宽/支持 meshlet、SVT 等 | `wiRenderPath3D.h:208`、`.cpp:1473`、`visibility_shadeCS.hlsl:25/94`（§4.2） | 传统光栅 PS：更简单，但 G-buffer 带宽高 | **规模不匹配**。它是 WE 为 meshlet/大场景做的执行模型，教学项目引入后调试成本远超收益；但**概念可取**（「同一光照模型可换执行模型」值得写进文档） |
| 按效果 Resources 结构体 | 资源归属与生命周期局部化、可惰性创建 | §7（23 个结构体清单+创建点） | 一个大 Renderer 类：集中但耦合 | **必需（简化版）**。只建当前用到的少数几个；命名与「Create 只申请 / 使用函数只读引用」纪律照搬 |
| 单一 tonemap 作为 HDR→LDR 分界 + push constant 契约 + 负索引可选绑定 | 明确色空间分界，减少全屏 pass | `wiRenderPath3D.cpp:2447-2494`、`tonemapCS.hlsl:95-194`、`ShaderInterop_Postprocess.h:129-150`（§5.3） | 每个效果一个 pass：更清晰但更慢；或不做色空间约定：后续加 HDR/广色域会返工 | **必需**。「唯一分界 + 契约表」是原则④最容易落地的部分 |
| ping-pong（`rt_read`/`rt_write` 机械 swap + `rt_first` 旁路 + `lastPostprocessRT` 契约） | 避免读写冲突、把「链的最后输出」变成稳定契约 | `wiRenderPath3D.cpp:2341-2344`、:2544、`wiRenderPath3D.h:175-180`（§6） | 每效果各自的临时 RT：显存多但不需纪律；或不做 ping-pong：读写冲突 | **必需**。但建议把指针换成显式的 `PostProcessContext{ in, out }` 结构，避免依赖调用方记得 swap |
| 内存别名（`AliasingAllocator` + `GPUBarrier::Aliasing`） | 省显存 | `wiGraphicsDevice.h:449-506`、`wiGraphics.h:754`、`wiRenderPath3D.cpp:108-166`（§8.2） | 各自独立分配：多占显存、零隐式不变式 | **规模不匹配 / 后置**。需要 RHI 支持 + 状态跟踪 + 断言，教学项目收益小、风险大 |
| `RenderPassBegin` 式 load/store 声明 | 让调用方只表达「保留/清除」，后端自动 barrier | `wiGraphicsDevice.h:216-219`、`wiGraphicsDevice_DX12.cpp:6114-6137`（§8.3） | 每次手动 `ResourceBarrier`：更啰嗦但更显式 | **必需**（若现有 RHI 已有，保持；若没有，优先补） |
| 阶段虚函数（9 个 `Render*` + `RenderPath2D` 两级） | 路径可替换、2D 叠层独立 | `wiRenderPath3D.h:184-192`、§9 | 单函数 `Render()`：最简但不可扩 | **可选（改造为「钩子 + 可复用后处理链组件」）**。WE 的教训是：只有钩子、没有可复用的链组件时，新路径会复制代码 |
| PathTracing 整体重写 `Render()` | 证明 DXR 路径可以完全换掉 RT 管线 | `wiRenderPath3D_PathTracing.cpp:302-712`（§9.2） | 强行共用基类 `Render()`：会产生大量条件分支 | **局部借鉴**：DXR 作为独立 Demo/独立 RenderPath（原则⑥正是这么要求），但**不要**让它复制后处理链——这需要先把链做成组件 |

### 10.1 给 TooiyumeRender 的三条落地建议（基于上述实测）

1. **分层顺序**：先做「公共渲染功能层」（无状态自由函数/工具类，入参 Scene+相机+CommandList，对应 §2）→ 再做「每效果资源结构体」（§7，只做现有用到的）→ 最后才做「RenderPath 编排与阶段钩子」（§9）。理由：逆序会把资源与路径重新绑死，正是约束原则②要避免的。
2. **后处理链**：把「HDR 段 → tonemap（唯一分界）→ LDR 段」写成一张**契约表**（效果名 / 输入纹理 / 输出纹理 / 分辨率 / 格式 / 资源状态 / 顺序 / 是否 HDR），并用统一 `PostProcessContext` 传递 ping-pong 读写（§5、§6）。这张表就是原则④的交付物。
3. **暂不做**：tiled 光列表、visibility compute shading、内存别名、FSR2、异步多队列。它们的共同点是「为规模与带宽优化」——教学项目的收益点不在那里（§10 表内已逐条给出理由）。

---

## 11. 未确认清单（诚实边界）

| # | 未确认内容 | 需要读什么才能确认 |
| --- | --- | --- |
| 1 | `RENDERPASS` 各枚举在 **PSO/着色器变体**层面的完整差异（本报告只覆盖 6 处关键分支） | `WickedEngine/wiRenderer.cpp:447-669`（`GetASTYPE/GetMSTYPE/GetVSTYPE/GetGSTYPE/GetHSTYPE/GetDSTYPE/GetPSTYPE` 内的 shader 变体 switch）、`:2033-2203`（`renderpass_info` 构造）、`:3161-3498`（`RenderMeshes`/`RenderImpostors` 内的 PSO 与变体选择） |
| 2 | `RENDERPASS_PREPASS_DEPTHONLY` 是否仍绑定颜色 RT（:2147 对非 MAIN 一律给 `format_idbuffer`，但调用点可能只绑 depth） | `WickedEngine/wiRenderPath3D.cpp:1181-1247` 的 RenderPass 构造 + `wiRenderer.cpp:2033-2203` |
| 3 | `TemporalAAResources.frame` 的自增位置（结构体自身不含自增逻辑） | `grep -rn temporalAAResources.frame WickedEngine/*.cpp`（本报告未执行） |
| 4 | `Postprocess_FSR2` 内部的资源状态/barrier 细节 | `WickedEngine/wiRenderer.cpp:17761-…`（本次未读） |
| 5 | `UpdateRenderData` / `UpdateRenderDataAsync` 具体写入哪些 CB/描述符（含 tiled 列表地址与 Camera CB） | `WickedEngine/wiRenderer.cpp` 内 `UpdateRenderData` 定义处（本次未读） |
| 6 | 非 DX12 后端（Vulkan）的同步差异 | `WickedEngine/wiGraphicsDevice_Vulkan.cpp`（本次未读；本报告同步结论**仅对 DX12 成立**） |
| 7 | TooiyumeRender 现有 RHI 是否已支持显式 barrier / load-store 声明 / aliasing | TooiyumeRender 的 RHI 源码（属 `02_架构现状审计` 范围，本报告未读） |

## 12. 一句话总结

本版本 WE 用「**一个 `RenderPath3D`（运行期特性开关 + 9 个 const 虚函数钩子 + 一条 HDR→LDR 后处理链）+ 一个无状态 `wi::renderer` 公共功能层（自由函数 + 按效果资源结构体）**」取代了旧版的 forward/deferred/tiled 三条子类；Path Tracing 是唯一另起炉灶的路径，用整体重写 `Render()` 的方式替换 RT 管线，代价是复制了整条后处理链。对 TooiyumeRender 而言：**公共功能层分离、按效果资源结构体、单一 tonemap 分界与契约表、ping-pong 读写上下文**是可立即借鉴的核心；**tiled 光列表、visibility compute shading、内存别名、FSR2** 属规模不匹配，应显式延后。

---

*报告完成时间：2026-10-24；调查者：we-renderfeat（task-2）。本报告只读调查，未修改任何运行时代码。*