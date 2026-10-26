# 01B 补充证据：WE 后处理链的 HDR/LDR 分界与 ping-pong 规则（Lead 实测）

> **这份文件为什么存在**：`01B_WE_公共渲染功能与后处理链.md` 由队友 we-renderfeat 负责，截至本文写作时尚未交付。
> 其中「后处理链逐效果顺序 / HDR→LDR 分界 / ping-pong 实现」三项是 `04_目标架构与模块边界.md` §4.4-B4 与 `03_架构差距报告.md` 的**直接依据**，不能悬空。
> 因此由 Lead 直接读源码补上这三项，其余 01B 议题（`wi::renderer` 完整依赖矩阵、DrawScene 的 renderPass 分支细节、forward/deferred/tiled 的完整判定）仍以正式 01B 为准。
> 所有结论均 `[实测]`，行号为本地 WE 源码 `wiRenderPath3D.cpp`。

---

## 补-1 `RenderPostprocessChain` 的真实结构：HDR 段 / Tonemap / LDR 段

函数入口：`wiRenderPath3D.cpp:2334` `void RenderPath3D::RenderPostprocessChain(CommandList cmd) const`

开头三行确定了 ping-pong 的**初始状态**（`:2340-2343`）：

`@cpp
const Texture* rt_first = nullptr; // not ping-ponged with read / write
const Texture* rt_read  = &rtMain;
const Texture* rt_write = &rtPostprocess;
`@

并且注意 `:2346` 有一句 **aliasing barrier**：`device->Barrier(GPUBarrier::Aliasing(&rtPrimitiveID, &rtPostprocess), cmd);`
—— 即 `rtPostprocess` 与 `rtPrimitiveID` **共享同一块显存**，靠 aliasing barrier 切换所有权。**这正是「资源生命周期与 GPU 执行同步」必须显式可见的证据**（原则⑤）。

### 1) HDR 段（`:2348-2445`，注释原文 `// 1.) HDR post process chain` `:2348`）

按执行顺序：

| 顺序 | 效果 | 行号 | 说明 |
|---|---|---|---|
| 0 | FSR2（与 TAA **互斥**分支） | `:2352-2379` | 走 FSR2 时 `rt_read=rtFSR[0] / rt_write=rtFSR[1]`，并**重新绑定** `BindCameraCB` 与 `BindCommonResources`（因为 FSR2 覆写了这些常量缓冲） |
| 0' | TemporalAA（else-if 分支） | `:2380-2394` | `Postprocess_TemporalAA(temporalAAResources, *rt_read, cmd)`；结果写入 `rt_first`（**不参与 ping-pong**） |
| 1 | Underwater | `:2395-2404` | 输入 `rt_first ? *rt_first : *rt_read`，输出 `*rt_write`，然后 `std::swap(rt_read, rt_write)` |
| 2 | 自定义后处理（BeforeTonemap 阶段） | `:2406-2425` | 同上模式；`custom_post_processes` 带 `stage` 字段区分 Before/After Tonemap |
| 3 | DepthOfField | `:2427-2441` | 有 `aperture_size/strength` 阈值与 `depthoffieldResources.IsValid()` 三重守卫 |
| 4 | MotionBlur | `:2442-2450` | 同上模式 |

### 2) Tonemap：**唯一的 HDR→LDR 跨界点**（`:2446-2465`）

注释原文（`:2447-2449`）：

> `// 2.) Tone mapping HDR -> LDR`
> `// Bloom and eye adaption is not part of post process "chain",`
> `//	because they will be applied to the screen in tonemap`

调用：`wi::renderer::Postprocess_Tonemap(rt_first == nullptr ? *rt_read : *rt_first, *rt_write, cmd, getExposure(), getBrightness(), getContrast(), getSaturation(), ... getEyeAdaptionEnabled() ? &luminanceResources.luminance : nullptr)`（`:2453-2465` 起）。

**结论（对原则④的意义）**：
1. **Tonemap 是链条上唯一一个有「域语义变化」的步骤**，它之前的输入是线性 HDR，之后的输出是 LDR。
2. **Bloom 与 eye adaptation 不属于 chain**，因为它们被**合并进 tonemap 的 shader**里执行（注释明说）。=> 它们不是独立 pass，没有独立 RT。
3. exposure/brightness/contrast/saturation **不是独立 pass**，而是 tonemap 的参数。

### 3) LDR 段（`:2473-2563`）

按执行顺序（`Postprocess_Tonemap` 之后）：

| 顺序 | 效果 | 行号 | 备注 |
|---|---|---|---|
| 1 | 自定义后处理（AfterTonemap 阶段） | `:2502-2521` | 与 HDR 段同一机制的第二处插入点 |
| 2 | Sharpen | `:2523` | `Postprocess_Sharpen(*rt_read, *rt_write, cmd, getSharpenFilterAmount())` |
| 3 | **FXAA** | `:2530` | `Postprocess_FXAA(*rt_read, *rt_write, cmd)` —— **在 tonemap 之后**（这点与「AA 应在 HDR 做」的直觉相反，因为 FXAA 是 LDR 空间算法） |
| 4 | Chromatic Aberration | `:2537` | |
| 5 | CRT | `:2544` | `Postprocess_CRT(*rt_read, *rt_write, cmd, 0, 0, true)` |
| 6 | GUI 模糊背景 | `:2558` | `Postprocess_Blur_Gaussian(rtGUIBlurredBackground[2], [1], [2], ...)` —— **旁路产物**，不参与主链 ping-pong |
| 7 | FSR（空间放大） | `:2565` | `Postprocess_FSR(*rt_read, rtFSR[1], rtFSR[0], cmd, getFSRSharpness())` |

（顺序以本地源码为准；行号取该效果调用在 `wiRenderPath3D.cpp` 中的实际位置。）

### 4) 出口

`:2544` 段之后 `lastPostprocessRT = rt_read;`（成员 `mutable`），对外由 `GetLastPostprocessRT()` 暴露；
`wiRenderPath3D.h:235` `const Texture& GetRenderResult3D() const { return *GetLastPostprocessRT(); }`。

---

## 补-2 ping-pong 的三条硬规则（可直接用于本项目设计）

从上述代码可提炼出三条规则，**本项目可原样采纳**：

| 规则 | WE 的写法 | 为什么这样做 |
|---|---|---|
| **R1 读写分离指针** | `rt_read / rt_write` 两个指针，每完成一个效果就 `std::swap` | 避免「同一个 RT 同时被读写」，这是最容易被忽视的 GPU 隐患 |
| **R2 旁路产物不参与 ping-pong** | `rt_first`（TAA 结果）与 `rtGUIBlurredBackground`（GUI 背景）都不参与 swap | 它们的生命周期与消费方独立，混进链会打乱双缓冲节奏 |
| **R3 分支必须重置 ping-pong 基准** | FSR2 分支里显式 `rt_read=rtFSR[0]; rt_write=rtFSR[1];` 并重新 `BindCameraCB` | 分辨率与绑定都可能变化，链的后续步骤不能假设原有基准 |

**对本项目的直接结论**：`04` 的 §4.4-B4 里「显式 src/dst + 允许 in-place 需注明 + 链由路径拥有」与之**一致**；但需补一条：**TAA/GUI 模糊这类旁路产物必须用独立成员持有，不进 ping-pong**。

---

## 补-3 与本项目现状的对照（原则④差距的硬证据）

| 维度 | WE（实测） | TooiyumeRender（HEAD，见 `02`） | 差距性质 |
|---|---|---|---|
| 是否有后处理链 | 有：HDR 段 4–5 效果 + tonemap + LDR 段 5–6 效果 | **无**：六段空壳（`DeferredRenderer.cpp:349-371`）后直接 `CopyBuffer` 到交换链 | **能力缺失**，不是质量问题 |
| HDR 中间 RT | `rtMain`（HDR）→ `rtPostprocess`（ping-pong） | 无 HDR RT；`RendererConfig.h:17` 交换链 `R8G8B8A8_UNORM` | **格式即架构**：没有 HDR RT 就无法有 HDR 链 |
| Tonemap 位置 | 链中唯一跨界步骤（`:2453`） | 藏在 `LightingPS.hlsl` 的 ACESFilm（光照 pass 内） | **语义错位**：tonemap 被塞进光照，skybox 等后续内容不再经过 tonemap |
| Ping-pong | `rt_read/rt_write` + `std::swap` | 无（无链） | 缺失 |
| 旁路产物 | `rt_first`/`rtGUIBlurredBackground` 独立持有 | 无 | 缺失 |
| 同步可见性 | aliasing barrier 显式出现在链首（`:2346`） | 全工程无 aliasing；barrier 由 `CommandContext` 攒批 | 设计选择不同，本项目**不必引入 aliasing**（见 `04` §4.6） |

---

## 补-4 仍待正式 01B 回答的议题（本文件**未**覆盖）

1. `namespace wi::renderer` 的完整依赖矩阵（它依赖什么、刻意不依赖什么）。
2. `DrawScene` 内部如何按 `wi::enums::RENDERPASS` 分支；`DRAWSCENE_FLAGS` 每一位的实际作用。
3. **forward / deferred / tiled 在本版本到底如何表达**（已知不存在三条 RenderPath 子类；已定位 `ComputeTiledLightCulling` 在 `:1047/:1327/:2742`、`CreateTiledLightResources` 在 `:322/:2661/:3144`，但**尚未确认是否存在「deferred vs forward」的运行时分支**）。
4. 按效果划分的 `*Resources` 结构体的创建/销毁/Resize 调用点清单。
5. `visibility_shading_in_compute`（`:652/:1473/:1544`）的含义与影响。

**这些议题不阻塞 03/04/05 的结论**，因为本项目已明确不引入 WE 的 tiled culling、多队列、aliasing（见 `04_目标架构与模块边界.md` §4.6 与 `01A` §10.2 的规模判断）。但若后续要做 Forward 路径的可见性剔除，需回头补齐第 3 项。

---

## 附：本文件使用的命令（可复现）

`@bash
sed -n '2334,2420p' wiRenderPath3D.cpp      # 链头 + HDR 段前半
sed -n '2334,2473p' wiRenderPath3D.cpp | grep -nE 'Postprocess_[A-Za-z_]+\('
sed -n '2468,2480p' wiRenderPath3D.cpp      # tonemap 调用与其参数
sed -n '2473,2575p' wiRenderPath3D.cpp | grep -nE 'Postprocess_[A-Za-z_]+\('
`@