# TooiyumeRender 当前真实架构审计（交付物 A 的现状输入）

| 项 | 值 |
| --- | --- |
| 审计者 | 队友 audit-tooiyume（共享任务 task-4） |
| 采集时间 | 2026-10-24 17:37 – 18:15（各结论均带实测时间戳） |
| 事实基线 | **HEAD = 51c09d0**（提交信息 `1`，2026-10-16），前序 2bd1c6d / 194db9d / 0f24418 / 7b09330 |
| 工作树 | 审计期间被并发写入（17:22 起持续变化），见 §1；**审计结束时无源码处于截断态** |
| 只读声明 | 本次审计**未修改任何运行时代码**（Engine/、Demos/、Assets/、Shader/ 只读），未执行 git checkout/restore/stash，未构建 |
| 方法 | `git status/diff/numstat/show` + `git grep` + `grep/glob` + 逐文件 `read`；所有 `file:line` 均为实测 |

> 阅读约定：**「HEAD」= 提交内容（事实基线）**；**「WT」= 2026-10-24 18:0x 的工作树（未提交）**。凡被截断或被并发改写的文件，结论一律以 `git show HEAD:<file>` 为准并同时写出 WT 状态。

---

## 0. 结论速览

1. **项目的真实分层与设计意图不一致。**声明式分层（`Application → Renderer → Scene/Asset → RHI → Core`）只体现在目录名与 include 路径上；实际依赖是**双向**的：
   - 场景层反向依赖渲染器层与 RHI：`Engine/05_Scene/Model/Model.h:5-12` 直接 include `02_RHI/Command/CommandContext.h`、`02_RHI/Resource/GpuBuffer.h`、`02_RHI/Resource/Heap/UploadBuffer.h`、`04_Renderer/Pipeline/PipelineDesc.h`、`04_Renderer/Material/Material.h`；
   - 场景层反向依赖应用层：`Engine/05_Scene/Camera/CameraController.h:3` include `01_Application/GameCore.h`；
   - 渲染器层直接持有全局描述符堆：`Engine/04_Renderer/Renderer/Base/BaseRenderer.h:46-47` + `BaseRenderer.cpp:11-12`。
2. **渲染路径只有 2 条，且共用的是一个「接口」而非「基类」**：`BaseRenderer`（`Engine/04_Renderer/Renderer/Base/BaseRenderer.h:172-215`）除 `ModelSort()` 外没有任何共享实现，`ForwardRenderer` 与 `DeferredRenderer` 把 Initialize/Shutdown/OnResize/BuildDescriptorHeaps/BuildRootSignature/GetPSOIndex/BindRenderState/BindMaterial 各写了一份（见 §3.3 逐段比对）。
3. **路径切换机制是死代码**：`RendererRegistry::CreateRenderer()`（`Renderer/Base/RendererRegistry.cpp:16-22`）零调用者；三个 Demo 各自 `new`/全局定义渲染器对象（`Demos/DeferredApp/DeferredApp.cpp:57`、`Demos/RayTracingApp/RayTracingApp.cpp:57`）。
4. **Deferred 管线真实实现了 3 段**：GBufferPass、LightingPass、SkyPass；**6 段是纯空壳**（只有成对的 PIX 事件、零 GPU 命令）：SSAO、Shadow、Transparent、PostProcess、TAA、Tonemap（`DeferredRenderer.cpp:274-279 / 281-285 / 349-353 / 355-359 / 361-365 / 367-371`）。
5. **不存在后处理链**：`DeferredRenderer.cpp:355-371` 两段空壳之后，`Display::Present()`（`Engine/01_Application/Display.cpp:229-247`）只做 **一次 `CopyBuffer` 到交换链**（`Display.cpp:251-263 PreparePresentSDR`）。没有 HDR 中间 RT，tonemap 只以 `ACESFilm` 形式藏在 `LightingPS.hlsl:155`，且结果直接写进**非 sRGB**的 `R8G8B8A8_UNORM`（`RendererConfig.h:17`）。
6. **Resize 是断链的**：`Display::Resize`（`Display.cpp:140-168`）只重建交换链，**不通知 Camera、不通知任何 Renderer、不重建 SceneColorBuffer/GBuffer/Depth**；唯一挂点 `ResizeDisplayDependentBuffers(...)` 在 `Display.cpp:167` 被注释，且该函数在 `Display.h:24` 只有声明、**全仓库无实现**；`ForwardRenderer::OnResize`/`DeferredRenderer::OnResize` 有定义（`:95`/`:139`）**零调用者**。
7. **DXR 完全不生效**：`Demos/RayTracingApp/RayTracingApp.cpp:36/66` 的 `RequiresRaytracingSupport()` 缺 `const`，与基类 `GameCore.h:26` 签名不一致 → 不构成 override → 恒返回 `false`；且该 Demo 实际加载的是 `DeferredRenderer`（`RayTracingApp.cpp:57`）。`DispatchRays` 全仓库 0 命中。
8. **确定性 bug 仍有 8 条在 HEAD 成立**（详见 §2.3），其中 5 条会直接导致画面错误：纹理槽 enum 错配（`ModelConvert.cpp:343-351`）、默认纹理槽错位（`ModelLoader.cpp:97-104`）、深度 VB 偏移不递增（`ModelConvert.cpp:133`）、硬编码 1920×1080（`LightingPS.hlsl:85`）、未还原的切线（`StaticMeshVS.hlsl:56`）。
9. **P0（本次审计新增）：工作树在审计期间被并发大幅改写，基线不可冻结。** 详见 §1。这不影响 HEAD 事实，但**任何「当前状态」结论都会在数分钟内过期**，后续施工必须固定提交点。
10. **可保留的高价值资产**：PBR/IBL 的数学实现（`DeferredCommon.hlsli:58-129`，GGX/Smith/Schlick/能量分配全对）、环境光烘焙四段 Compute（`EnvironmentLightingManager.cpp:147-168`，真实存在且已工作）、`MeshSorter` 排序框架（`BaseRenderer.cpp:41-106`）、`CommandContext` 的屏障攒批 + fence 编码设计（`CommandContext.cpp:385-409`、`CommandListManager.cpp:14-15`）。

---

## 1. 基线选择与工作树状态（P0 风险章节）

### 1.1 为什么把基线定在 HEAD = 51c09d0

1. **工作树在审计期间持续被写。** 17:37 首次采集时 13 个源文件处于改动/截断混合态；17:48 复测时 `DeferredRenderer.cpp` 已从「1007 字节 / 30 行」恢复到 655 行；17:51 三个 Demo 的行数又整体变了（见 1.2）。mtime 覆盖 17:22–18:11，说明有另一个并行会话（提交信息中的 R0/R0.2 系列改动）正在落盘。**无法把工作树当作稳定的事实基线。**
2. **HEAD 是最后一次一致提交**，且旧审计（`docs/AI/01_Audit_2026-10-24/`）自述基于同一提交只读审计，两套结论可直接对表。
3. **旧审计的 P0 结论（纹理槽、切线、分辨率、resize 断链、DXR）在 HEAD 上全部可复现**，说明 HEAD 仍能代表「项目真实掌握的能力水平」。
4. **工作树的改动方向明确且是增量（R0 系列）**：资产路径解析（`AssetRoot`）、命令行开关（`CommandLineArg::HasFlag`）、日志落盘（`Utility.h`）、shader 依赖（`ShaderDeps.cmake`）、`RELEASE` 宏补齐（`Engine/CMakeLists.txt`）。这些是**「修基础设施」而非「改架构」**，不影响本报告对架构现状的判断，但必须作为「已在飞行中的改动」记录，避免后续工作包重复劳动或冲突。

### 1.2 工作树截断改动逐文件清单（实测）

**方法**：`git status --porcelain=v1`、`git diff --stat`、`git diff --numstat`、`git diff --cached --stat`、`git diff -- <file>`、`wc -l`、`stat -c`（mtime）、`md5sum`（前 8 位）。**未做任何修复动作。**

**(a) lead 前提的核对结果：部分过期。**lead 给的前提是「17 个文件改动，34 insertions / 1373 deletions，多个源码被截断」。实测：

| lead 前提 | 实测（18:0x） | 判定 |
| --- | --- | --- |
| 17 个文件改动 | **未暂存 28 个文件**（+ 索引中 3 个删除） | 已过期（期间新增了脚本、cmake、GameInput 等改动） |
| 34 insertions / 1373 deletions | **未暂存 455 insertions / 548 deletions**；另**已暂存删除 1533 行**（`DefaultPS.h` 649 + `DefaultVS.h` 876 + `CMakeSettings.json` 8） | 已过期 |
| 多文件被截断成残缺态 | **17:37 实测确实存在截断态**（见下表 `DeferredRenderer.cpp` 17:25:53 / 1007 B / 30 行 vs HEAD 638 行）；**18:0x 已全部恢复**，当前无任何源码处于截断态 | 曾成立，现已消失 |
| mtime 17:22–17:25 疑中断写入 | **mtime 在 17:22–18:11 之间持续推进**（我审计期间仍在增长）；lead 18:17:49 核实：存在**另一个并行的开发会话**在同一工作树施工（提交信息里的 R0/R0.2 系列），被截断的 `DeferredRenderer.cpp` 已由对方恢复（mtime 17:48:05） | 成立，且已定性：**并行会话持续写入**，不是本会话的中断残留 |

**(b) 逐文件清单（WT vs HEAD；时间为我实测的该文件 mtime）**

| 文件 | mtime（实测） | HEAD 行数 | WT 行数 | numstat(+/-) | 状态 |
| --- | --- | --- | --- | --- | --- |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp` | 17:25:53 → 17:48:05 | 638 | **30（截断态，1007 B）→ 655（恢复后）** | 19/2 | **曾截断，已恢复**；恢复后改动仅为 AssetRoot 化 HDR 路径 + 注释 |
| `Demos/DeferredApp/DeferredApp.cpp` | 17:29:21 → 17:51:13 | 143 | 144 → 148 | 15/10 | 已重写（Asset::Resolve + HasFlag） |
| `Demos/ForwardApp/Tooiyume.cpp` | 17:35:48 → 17:51:13 | 145 | 153 → 157 | 21/9 | 同上 |
| `Demos/RayTracingApp/RayTracingApp.cpp` | 17:29:21 → 17:51:13 | 143 | 144 → 148 | 13/8 | 同上 |
| `Engine/01_Application/GameCore.cpp` | 17:24:22 → 17:37:50 | 160 | 174（4800 B）→ 179 | 19/2 | 未截断；新增 Asset/命令行初始化 + [Boot] 日志 |
| `Engine/00_Core/Utility/AssetRoot.h` | 17:27:37 | 无（未跟踪） | 45 | 新增 | **新建未跟踪** |
| `Engine/00_Core/Utility/AssetRoot.cpp` | 17:48:33 | 无（未跟踪） | 228 | 新增 | **新建未跟踪** |
| `Engine/00_Core/Utility/CommandLineArg.h` | 17:24:09 → 17:51:04 | 11 | 13 → 16 | 6/1 | 新增 `HasFlag`/`InitializeFromSystemCommandLine` |
| `Engine/00_Core/Utility/CommandLineArg.cpp` | 17:28:14 → 17:51:04 | 75 | 90 → 106 | 48/10 | 新增无值开关解析 |
| `Engine/00_Core/Utility/Utility.h` | 17:31:33 | 150 | 189 → 198 | 57/9 | 新增日志落盘；缓冲 512→2048；Release 断言宏补分号 |
| `Engine/01_Application/Display.cpp` | 17:33:18 | 265 | 267 | 2/1 | 仅 2 行 [Boot] 日志 |
| `Engine/01_Application/GameInput.cpp` | — | — | — | 83/26 | 未逐行复核（不在我的必读范围） |
| `Engine/02_RHI/GraphicsCore.cpp` | 17:35:26 | 314 | 326 | 13/1 | [Boot] 日志 + InfoQueue 的 WARNING 断点关掉 |
| `Engine/05_Scene/Model/ModelLoader.cpp` | — | — | — | 10/2 | 仅错误信息增强（`git diff` 全文已读） |
| `Engine/04_Renderer/EnvironmentLightingManager.cpp` | — | — | — | 3/1 | 推断为注释/日志（未逐行复核） |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp` | — | — | — | 16/14 | include 改生成头 + [Boot] 日志 |
| `Engine/20_cmake/ShaderCompiler.cmake` | — | — | — | 15/1 | 接入 `ShaderDeps.cmake`，`DEPENDS` 改全依赖 |
| `Engine/20_cmake/ShaderDeps.cmake` | — | — | 60 | 新增 | **新建未跟踪**（递归收集 `#include`） |
| `Engine/CMakeLists.txt` | 17:26:35 → 17:50:31 | 155 | 165 → 221 | 74/8 | 新增 `compile_shader(DefaultVS/DefaultPS)`、ShaderCache include 优先级、按 build type 定义 `RELEASE` |
| `CMakeLists.txt`（根） | — | — | — | 32/2 | 未逐行复核 |
| `CMakePresets.json` / `vcpkg.json` / `CMakeSettings.json` | — | — | — | 17/26、2/1、索引删除 | 未逐行复核 |
| `Engine/Shader/01_Default/DefaultPS.h`、`DefaultVS.h` | — | 649 / 876 | — | **已暂存删除（索引中）** | HEAD 里存在的旧字节码头，WT 已删 |
| `docs/Roadmap.md` | — | 18 | 删除 | 0/18 | 文档删除 |
| `.idea/*` 9 文件 | — | 407 | 删除 | 0/407 | 编辑器配置清理 |
| 未跟踪目录 | — | — | — | — | `docs/AI/`、`docs/AIWorkLogs/`、`scripts/`（`build.bat` 4493 B、`run.bat` 1945 B） |

**(c) 对审计的影响说明**

1. **凡涉及以下文件的结论，我一律以 `git show HEAD:<file>` 为准**，并在正文标注 WT 差异：`DeferredRenderer.cpp`、`ForwardRenderer.cpp`、三 Demo、`GameCore.cpp`、`Display.cpp`、`Utility.h`、`CommandLineArg.*`、`Engine/CMakeLists.txt`、`ModelLoader.cpp`。
2. **「截断态」不构成对 HEAD 架构结论的影响**：截断态只存在于工作树，且已被并行会话自行修复；本报告中所有行号均已用 `git show HEAD:` 或（对未改动文件）直接 `read` 复核。
3. **但截断态本身是一个必须记录的工程风险**：它说明 ① 存在多个写入者共享同一工作树；② 写入未走原子替换（出现「文件被清空到 30 行」的中间态）；③ 若当时有人构建/运行，会得到与源码不符的二进制。**建议：任何构建只允许在干净提交点进行；施工会话独占工作树。**
4. **审计结论的时效**：本报告写入时间 2026-10-24 18:1x；工作树当时仍在演进。§1.2 的 WT 列是快照，不是结论。

### 1.3 我**未能亲眼确认**的 lead 前提

- lead 的「17 个文件、34 insertions / 1373 deletions」：我在 17:37 采集到的等效统计是 **13 项源码/配置改动 + 10 项删除（`.idea/*` 9 个 + `docs/Roadmap.md`）**；18:0x 复测为 **未暂存 28 项（455+/548-）+ 索引删除 3 项（1533-）**；lead 18:17:49 复测是 **合计 38 项**。三组数字互不相同 → **该前提已过期，我未能复现原始数字**；不影响任何架构结论。
- 「截断的由来」：已由 lead 于 18:17 核实为 **另一个并行开发会话在同一工作树施工**（R0/R0.2 系列），**不是本会话的中断残留**。我能独立证明的只有「截断态存在过、随后被第三方写回」（17:37 实测 1007 B / 30 行 → 17:48:05 恢复为 655 行）；写入者身份来自 lead 核实。
- 仍无法确认：并行会话的**改动清单与边界**（它在我审计期间继续增长），因此 WT 的任何描述都只是快照。

---

## 2. 旧审计结论复核表（基线 HEAD；WT 单列）

> 口径：**仍成立** = HEAD 与旧审计一致；**已改变** = HEAD 与旧审计一致、但 WT 已被并行会话改动；**不成立** = 旧审计有误（我给出修正）。旧审计原文：`docs/AI/01_Audit_2026-10-24/00_审计总报告.md` 及 `raw/` 五份子报告。所有行号已用 `git show HEAD:<file>` 或直接 read 复核。

### 2.1 六段 STUB —— 全部「仍成立」

六段的实体完全相同：**只有一对相邻的 `context.PIXBeginEvent(L"…")` / `context.PIXEndEvent()`，中间零 GPU 命令**。

| 段名 | HEAD 行号（DeferredRenderer.cpp） | WT 行号 | 判定 |
| --- | --- | --- | --- |
| `3. SSAOPass` | 274-279 | 291-296 | 仍成立 |
| `4.ShadowPass` | 281-285 | 298-302 | 仍成立 |
| `7. Transparent Forward Pass` | 349-353 | 366-370 | 仍成立 |
| `8. PostProcess Pass` | 355-359 | 372-376 | 仍成立 |
| `9. TAA Pass` | 361-365 | 378-382 | 仍成立 |
| `10. Tonemap Pass` | 367-371 | 384-388 | 仍成立 |

对照真实执行的段：`GBufferPass`（HEAD :231-271）、`DeferredLightingPass`（HEAD :287-335）、`SkyPass`（HEAD :337-347）→ **Render() 的 10 个 PIX 段里只有 3 段有实际 GPU 命令**；外层 `PIXBeginEvent(L"DeferredRenderer")` 在 HEAD :202、`PIXEndEvent` 在 :372。

### 2.2 ORPHAN 清单逐条复核

| # | ORPHAN 项 | 我的判定 | 证据（实测 file:line） |
| --- | --- | --- | --- |
| 1 | `RendererRegistry::CreateRenderer` | **仍成立（零调用）** | `Renderer/Base/RendererRegistry.cpp:16-22` 查表返回 `RendererPtr`，未命中返回 nullptr；自动注册模板在 `RendererRegistry.h:44-59`；三个 Demo 仍直接定义全局渲染器对象：`Demos/DeferredApp/DeferredApp.cpp:57`、`Demos/RayTracingApp/RayTracingApp.cpp:57`、`Demos/ForwardApp/Tooiyume.cpp` |
| 2 | `TAACS.hlsl` | **仍成立（零调用者）** | 文件存在：`Engine/Shader/04_PostProcess/TAACS.hlsl`；`grep -rIl TAACS Engine Demos` 只命中 `Engine/04_Renderer/TemporalEffects.cpp`，且那里的 include 被注释 |
| 3 | `TemporalEffects` 六个函数无定义 | **仍成立** | `Engine/04_Renderer/TemporalEffects.h:11-39` 声明 `EnableTAA(:16)` `Initialize(:18)` `Shutdown(:20)` `Update(:25)` `GetFrameIndexMod2(:29)` `GetJitterOffset(:34)` `ClearHistory(:36)` `ResolveImage(:38)`；`TemporalEffects.cpp` 无实现；`GraphicsCore.cpp` 里 `TemporalEffects::Initialize()` 整段在 `/* */` 内，其 include 也被注释 |
| 4 | `VelocityBuffer` 建了不写不读 | **仍成立** | `DeferredRenderer.cpp:476` 创建 `L"Deferred VelocityBuffer"`；Render 的 10 段中零处读写它 |
| 5 | Forward 绑了灯光 SRV 但 PS 无光照 | **仍成立** | `ForwardRenderer.cpp:65-71` 声明了 `DestCount`/`SourceCounts`/`SourceTextures` 但从未使用；`Engine/Shader/01_Default/DefaultPS.hlsl:51-57` 的 `MainPS` 只返回 `baseColorFactor * baseColorTexture.Sample(...)`，**完全没有光照代码** |
| 6 | `alphaCutoff` 写进 glTF 但未进常量 | **仍成立** | `Engine/03_AssetSystem/Importers/glTF.h:135` union 内有 `alphaCutoff:16`；`ModelConvert.cpp:294` 只做 `material.flags = srcMat.flags`；HLSL 侧无 `alphaRef`，`GBufferPS.hlsl:88-124` 无 clip/discard |
| 7 | `RenderUI` 钩子零调用 | **仍成立** | `Engine/01_Application/GameCore.h:24` `virtual void RenderUI(GraphicsContext&) {}`；`GameCore.cpp:58-77` 的 `UpdateApplication` 只有 `game.Update` / `game.RenderScene()` / `Display::Present()` |
| 8 | BackBuffer 未对外暴露 | **仍成立** | `Display.h:16-25` 只暴露 `SceneColorBuffer`；`ColorBuffer g_DisplayPlane[3]` 在 `Display.cpp:131` 的静态区；全仓库 0 处 `GetCurrentBackBufferIndex` |
| 9 | `ShadowCamera`（旧审计未列入总报告 ORPHAN，我建议补入） | **仍成立** | `Engine/05_Scene/Camera/ShadowCamera.cpp:6-11` 的 `UpdateMatrix(...)` **函数体为空**，`m_ShadowMatrix`（`ShadowCamera.h:27`）从未被写入；`.h:3` 注释自述「没完全啃明白」 |
| 10 | 空文件 / 死目录 | **仍成立** | `Engine/04_Renderer/Shader/ShaderLibrary.h`=0 行、`ShaderLibrary.cpp`=0 行、`Pipeline/PSOBuilder.h`=0 行、`PSOBuilder.cpp`=0 行、`Material/MaterialFlags.h`=0 行；`Engine/04_Renderer/RenderGraph/` 空目录；`Engine/Shader/03_Shadow/` 空、`Engine/Shader/Generated/` 空 |

### 2.3 P0 BUG 清单逐条复核（12 条）

| # | BUG | 我的判定 | 证据与说明 |
| --- | --- | --- | --- |
| 1 | 材质纹理槽 enum 错配 | **仍成立** | `glTF.h:138` 顺序 `{kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal}`（0..4） vs `ConstantBuffers.h:18-26` `{kBaseColor=0, kNormal=1, kEmissive=2, kOcclusion=3, kMetallicRoughness=4}`。`ModelConvert.cpp:343/345/347/349/351` 用**后者的枚举名**索引**前者的数组**：`:343 [0]`正确、`:345 kMetallicRoughness(4)→textures[4]=Normal`错、`:347 kOcclusion(3)→Emissive`错、`:349 kEmissive(2)→Occlusion`错、`:351 kNormal(1)→MetallicRoughness`错（5 处只对 1 处）。**新增证据：第三套顺序存在于 `Engine/Shader/01_Default/DefaultPS.hlsl:44-48`（BASECOLOR=0, METALLICROUGHNESS=1, OCCLUSION=2, EMISSIVE=3, NORMAL=4）→ 全项目共三套互不相同的纹理槽顺序** |
| 2 | 默认纹理槽错位 | **仍成立** | `ModelLoader.cpp:97-104` 的 `DefaultTextures[kNumTextures]` 下标语义是 `ConstantBuffers.h` 顺序，但注释与取值按 glTF 顺序写 → 只有 `[0]` 正确：`[1]` 取 `kWhiteOpaque2D`（法线经 *2-1 后为 (1,1,1)）、`[2]` 取白（emissive 应为黑）、`[3]` 取 `kBlackTransparent2D=(0,0,0,0)`（ao 恒 0 → 环境光归零）、`[4]` 取 `kDefaultNormalMap=(0.502,0.502,1.0)`（metallic 恒 1、roughness 减半）。常量定义：`Engine/02_RHI/Pipeline/GraphicsCommon.cpp:91-104` |
| 3 | 深度 VB 偏移不递增 | **仍成立（触发条件窄）** | `ModelConvert.cpp:133` `std::memcpy(uploadMem + curDepthVBOffset, draw->DepthVB->data(), …)` 无 `curDepthVBOffset` 增量；`:134` 索引有增量；`:138-141` 只在每个 Mesh 结束后累加 `curVBOffset/curDepthVBOffset/curIBOffset/curIndexOffset` → **仅当同一 Mesh 内 ≥2 个 draw 才互相覆盖** |
| 4 | `LightingPS.hlsl:85` 硬编码分辨率 | **仍成立** | `Engine/Shader/02_Deferred/LightingPS.hlsl:85` `float2 renderResolution = float2(1920, 1080);`，`:86` 直接用它换算 uv → 任何非 1920×1080 分辨率下世界坐标重建错误 |
| 5 | `StaticMeshVS.hlsl:56` 未还原 Tangent | **仍成立** | `StaticMeshVS.hlsl:49` `float3 Normal = vsInput.Normal*2.0-1.0;`、`:50` `float4 Tangent = vsInput.Tangent*2.0-1.0;`，但 `:56` `vsOutput.TangentWS = float4(normalize(mul(vsInput.Tangent.xyz,(float3x3)WorldIT)), vsInput.Tangent.w);` —— 用的是**未还原的原始值**，`:50` 的 `Tangent` 是死变量 |
| 6 | resize 断链 | **仍成立（表述修正，见 §6）** | 真实入口是 `Display::Resize`（`Display.cpp:140-168`）；唯一挂点 `Display.cpp:167 // ResizeDisplayDependentBuffers(...)` 被注释；`Display.h:24` 有声明但**全仓库无实现**；`Renderer::OnResize` 只有定义（`ForwardRenderer.cpp:95`、`DeferredRenderer.cpp:139`），**零调用者**。旧审计把「Display 的挂钩」与「Renderer::OnResize」混述，且 Display 里并不存在名为 `OnResize` 的成员 |
| 7 | WM_SIZE 不滤 0 尺寸 | **仍成立** | `GameCore.cpp:158-175` WndProc `case WM_SIZE: Display::Resize((UINT)((UINT64)lParam & 0xFFFF), (UINT)((UINT64)lParam >> 16));` → 最小化时宽高为 0 会直落 `ResizeBuffers(3, 0, 0, …)` |
| 8 | `RELEASE` 宏无定义 | **仍成立（且是 Release 断言的根因）** | HEAD `Utility.h:128-144` 有 `#ifdef RELEASE` 分支，但**从未有任何构建配置定义 RELEASE**（WT 已在 `Engine/CMakeLists.txt` 加注释按 build type 补齐，原话：「源码里有 13 处 #ifdef RELEASE / #ifndef RELEASE 分支，但此前从未有任何构建配置真正定义过 RELEASE」）；且 HEAD `#define ASSERT(isTrue,...) (void)(isTrue)` **无分号**，Release 生效后会把 `ASSERT(x)` 之后紧跟的语句解析错（WT 已改为带分号版本并注释原因） |
| 9 | Forward 用入库旧 `.h` 字节码 | **HEAD 仍成立 / WT 已改变** | HEAD：`ForwardRenderer.cpp:16-17` include `Shader/01_Default/DefaultVS.h` 与 `DefaultPS.h`，二者是**提交进仓库的旧字节码头**（索引删除量实测：`DefaultVS.h` 876 行、`DefaultPS.h` 649 行，共 1533 行）；HEAD 的 `Engine/CMakeLists.txt` **从未 `compile_shader` 过 01_Default**。WT：改为 `#include "DefaultVS.h"` / `"DefaultPS.h"`（生成到 `build/ShaderCache`）并纳入 `compile_shader`，且把 ShaderCache 放到 include 顺序最前 |
| 10 | `forceRebuild=true` 写死 | **HEAD 仍成立 / WT 已改变** | HEAD：三个 Demo 各自 `bool forceRebuild = true;`；WT：`bool forceRebuild = CommandLineArgs::HasFlag(L"RebuildModels");`（`CommandLineArg.h:15` 声明、`CommandLineArg.cpp:94-97` 实现、`:31-41` 支持无值开关）；`ModelLoader.cpp:217` 仍以 `needBuild = forceRebuild` 起手 |
| 11 | 硬编码绝对路径 18 处 | **HEAD 仍成立（实测恰好 18 处）/ WT 已消除** | HEAD `git grep D:/CS-Self-Study HEAD`：`Demos/DeferredApp/DeferredApp.cpp:55,56,57,58,60,62,64`、`Demos/ForwardApp/Tooiyume.cpp:53,54,55,56,58`、`Demos/RayTracingApp/RayTracingApp.cpp:57,58,59,60,62`、`Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:40`（**引擎层也有一处**）。WT：Demo 全部改 `Asset::Resolve(L"Model/…")`，引擎侧改 `Asset::Resolve(L"Skybox/grasslands_sunset_2k.dds")`，全仓库只剩 `AssetRoot.h:10` 的注释示例 |
| 12 | DXR `RequiresRaytracingSupport` 少 `const` | **仍成立（HEAD 与 WT 都成立）** | 基类 `GameCore.h:26` `virtual bool RequiresRaytracingSupport() const { return false; }`；`RayTracingApp.cpp:36` 声明与 `:66` 定义均**无 const 也无 override** → 不构成重写，虚表指向基类 → 恒 `false`，`Graphics::Initialize(false)`。补充实测：`RayTracingApp.cpp:57` 实例化的其实是 `Renderer::Deferred::DeferredRenderer`（**不是 RT 渲染器**）；`DispatchRays` 全仓库 0 命中；`RayTracingRenderer` 目录不存在 |

### 2.4 旧审计其他关键结论复核

| 结论 | 判定 | 证据 |
| --- | --- | --- |
| `Camera.h:3-5` 约定：右手系 / Forward=-Z / 行向量 v*M / 行主序 / 深度 [0,1] / 非 ReverseZ | **仍成立** | `Camera.h:3-5` 注释；`Camera.h:105` `m_ReverseZ`（构造置 false，`Camera.h:129-133`）；`Camera.h:92` `ReverseZ()` 零调用；`Camera.cpp:87-88` `Q1=Far/(Near-Far); Q2=Q1*Near`；`Camera.cpp:92-97` 行主序矩阵；`GraphicsCommon.cpp:145-153` `DepthFunc=LESS` / `LESS_EQUAL` |
| `Camera.h:82 SetAspectRatio` 零调用 → resize 后仍按 9:16 拉伸 | **仍成立** | `Camera.h:82` 定义；`Camera.h:129-133` 默认 `aspect=9.0f/16.0f`；全仓库无调用点；与 §2.3#6 同源 |
| `ConstantBuffers.h:18-26` enum 顺序 | **仍成立** | 逐值实测 |
| `RendererConfig.h:8-19` GBuffer 4RT + Depth + Velocity | **仍成立** | `:8-15` R8G8B8A8_UNORM / R16G16_FLOAT / R16G16B16A16_FLOAT / R8G8B8A8_UNORM；`:17` SceneColor=R8G8B8A8_UNORM；`:18` Depth=D32_FLOAT；`:19` Velocity=R16G16_FLOAT |
| 运行期主链 Asset→BuildModel→ModelLoader→Model::Update→GatherRenderables→MeshSorter→DeferredRenderer::Render→Display::Present | **仍成立** | `ModelLoader.cpp:189-264`；`BaseRenderer.cpp:19-24`；`DeferredRenderer.cpp:167-373`；`Display.cpp:229-263` |
| `MeshConvert.cpp` 顶点布局 | **仍成立，路径需更正** | 真实路径 `Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp`；`Importers/` 下为 `Mesh/`、`Model/`、`Texture/` 加 `glTF.cpp/.h`、`json.hpp` |
| `GlobalConstants` C++ 4 字段 vs HLSL 2 字段 | **仍成立** | `ConstantBuffers.h:60-66` vs `StaticMeshVS.hlsl:15-19` 与 `DefaultPS.hlsl:25-29`（均 2 字段）；偏移 0 相同故暂未爆 |
| `SkyboxPS` 把 UV 当 NDC 二次映射 | **仍成立** | `SkyboxVS.hlsl:19-21` 已把 uv∈[0,2] 映射为 NDC 写进 `PositionH`；`SkyboxPS.hlsl:22-26` 又 `ndc = input.UV*2.0-1.0` → 实得 [-1,3]。**且该路径每帧真被执行**：`EnvironmentLightingManager.cpp:325-356` 的 `SkyboxPass` 末尾 `Draw(3,0)`，由 `DeferredRenderer.cpp:344` 调用 |
| IBL LOD 硬编码 8.0 而实际 5 mip | **仍成立** | `LightingPS.hlsl:143` `const float maxPrefilterMip = 8.0;` vs `EnvironmentLightingManager.cpp:382` 用 `m_PrefilterMipCount` 建 cube（`.h:106` = 5）与 `:278` `roughness = mip/(m_PrefilterMipCount-1)` |
| `SaveDDS()` 空实现 | **仍成立** | `EnvironmentLightingManager.cpp:122-126` 空函数体 → 烘焙结果不落盘、每次启动重算 |
| `m_HDRLoaded` 从不置位 | **仍成立** | `EnvironmentLightingManager.h:100` 声明；`.cpp:150` 只读不写 |
| `BakeEnvironmentTextures` 每帧调用且内部 `Finish(true)` 阻塞 | **已改变（需修正）** | 函数确实每帧被 `DeferredRenderer.cpp:154` 调用，但 `EnvironmentLightingManager.cpp:150` 的 `if (m_Baked \|\| …) return;` 使**实际只烘焙一次**（`:166-167` 置 `m_Baked`/`m_Ready`）；`:164` 的 `Finish(true)` 只在首帧同步阻塞一次 |
| `GpuResource` 无延迟释放队列、`Destroy` 立即置空 ComPtr | **仍成立** | `Engine/02_RHI/Resource/GpuResource.h:30-35` |
| `SamplerManager` 去重失效 | **仍成立** | `Engine/02_RHI/Pipeline/SamplerManager.cpp:20-32`：命中即返回，未命中创建后**从不 insert** → 每次调用都新建采样器描述符 |
| `CommandListManager` fence 无条件回写（可回退） | **不成立（我修正）** | `CommandListManager.cpp:138-139` `m_LastCompletedFenceValue = std::max(m_LastCompletedFenceValue, m_pFence->GetCompletedValue());` → **有 std::max 保护** |
| barrier 攒批可越界 | **仍成立，但需精确限定** | `CommandContext.cpp:385-386/408-409`（TransitionResource）与 `:422-437`（BeginResourceTransition）都有 `if (FlushImmediate \|\| m_NumBarriersToFlush == 16) FlushResourceBarriers();`；**只有 `:442-450 InsertUAVBarrier` 与 `:457-466 InsertAliasBarrier` 是 `if (FlushImmediate)`** → 这两条路径可越过 16，而 Release 下 ASSERT 变 `(void)` |
| `DescriptorAllocator` 完全无锁 | **仍成立** | `DescriptorHeap.cpp:40-62` 的递增路径（`m_CurrentHandle.ptr += …; m_RemainingFreeHandles -= …`）无锁；只有 `RequestNewHeap()`（`:19-37`）与 `DestroyAll()` 持 `sm_AllocationMutex`。补充：`DestroyAll()` 只 `sm_DescriptorHeapPool.clear()`，**不回退 allocator 自身状态** |
| `DescriptorHeap` 只增不减 | **仍成立** | `DescriptorHeap.h:105` `Destroy(){ m_Heap=nullptr; }`（不归还描述符）；`DescriptorHeap.cpp:14-18` |
| fence 高 8 位存队列 Type「设计巧」 | **仍成立** | `CommandListManager.cpp:14-15`、`:88-93`（`GetQueue(D3D12_COMMAND_LIST_TYPE(FenceValue >> 56))`）、`:150-157`、`:177-181` |
| `CommandContext` 已是 GraphicsCommandList4 | **仍成立** | `CommandContext.h:189` `ComPtr<ID3D12GraphicsCommandList4>`；但 `:136` 的 `GetCommandList()` 仍返回 `ID3D12GraphicsCommandList*` |
| DXR 能力探测 DONE 但句柄未导出 | **仍成立** | `GraphicsCore.cpp` DXR 段（OPTIONS5 + RaytracingTier + `QueryInterface<Device5>`）；`GraphicsCore.h:20-38` 只导出 `g_Device`/`g_DXGIFactory`/`g_CommandManager`/`g_ContextManager`/`g_DescriptorAllocator[]`/两个 TypedUAVLoad 标志 |
| PBR BRDF 数学正确 | **仍成立（逐行读过）** | `Engine/Shader/02_Deferred/DeferredCommon.hlsli:58-129`：`FresnelSchlick`、`DistributionGGX(a=roughness²)`、`GeometrySchlickGGX(k=(r+1)²/8)`、`GeometrySmith`、`EvaluateCookTorranceLight`（kS=F、kD=(1-kS)(1-metallic)、diffuse=kD*baseColor/PI，再乘 `lightRadiance*NdotL`）→ **全项目最值得保留的资产** |
| `GBufferPS.hlsl:120` B 通道恒 0.5 → F0 恒 0.04 | **仍成立** | 实测 `GBufferPS.hlsl:121` `output.Material = float4(metallic, roughness, 0.5, 0.0); // RT2。B = Specular` |
| `GBufferPS` 无 clip/discard | **仍成立** | `GBufferPS.hlsl:88-124` 的 `MainPS` 全函数无裁剪分支 |
| 构建 83/83 全绿、3 个 exe | **未复核** | 本阶段禁止构建（任务硬约束）；WT 新增 `scripts/build.bat`(4493 B)、`scripts/run.bat`(1945 B) |
| `Assets/Model` 不存在且被 `.gitignore` 忽略 → 无法验证画面 | **已改变（分两层）** | **本机**：`Assets/Model` 存在，实测 211 个文件（含 6 个 `.ty` 烘焙缓存）；**仓库**：`git ls-files Assets` 仅 3 个文件（`Skybox/grasslands_sunset_2k.dds`、`.hdr`、`新建文本文档.txt`），`.gitignore:386 /Assets/Model` 仍忽略 → 「clean clone 缺资产」成立；「本机无法验证画面」不成立 |

### 2.5 我对旧审计的修正（5 处）

1. **fence 回写有保护**：`CommandListManager.cpp:138-139` 用 `std::max`，「无条件回写、多线程可回退」不成立。
2. **barrier 越界只在 UAV/Alias 路径**：Transition 路径有 `==16` 自动 flush，一刀切描述需收敛到 `CommandContext.cpp:442-450 / 457-466`。
3. **`BakeEnvironmentTextures` 不是每帧重烘**：`m_Baked` 守卫使其只烘焙一次；真问题是「每帧空转调用 + 首帧 `Finish(true)` 阻塞 + 结果不落盘」。
4. **resize 断链的准确表述**：不存在 `Display::OnResize`；断链 = 「`Display::Resize` 不通知任何人」加「`ResizeDisplayDependentBuffers` 只有声明无实现、唯一调用被注释」加「`Renderer::OnResize` 零调用」三件事叠加。
5. **RayTracingApp 的真实身份**：`RayTracingApp.cpp:57` 用的是 `DeferredRenderer`，并把 `DeferredRenderer.h` include 进去（`:11`）；它连 RT 渲染器都不是，`RequiresRaytracingSupport` 又因缺 const 永不生效。

---

## 3. 真实分层架构图与依赖方向

### 3.0 磁盘分层 vs 真实依赖（依赖方向是有环的）

```
             ┌───────────────────────────────────────────────────────────┐
  应用层      │ Demos/{DeferredApp,ForwardApp,RayTracingApp}/*.cpp (148/157/148 行) │
  (01_Application + Demos) │ Engine/01_Application/{GameCore,Display,GameInput,SystemTime} │
             └───────┬───────────────────────────────▲──────────────────┘
                     │ 直接 new/全局定义渲染器对象      │ 场景层反向依赖：CameraController.h:3
                     │ (DeferredApp.cpp:57 等)         │ include 01_Application/GameCore.h
                     ▼                                │
             ┌───────────────────────────────────────┴──────────────────┐
  渲染路径    │ Engine/04_Renderer/Renderer/{Base,Deferred,Forward}/*    │
             │  + BufferManager / EnvironmentLightingManager /        │
             │    TemporalEffects / Pipeline(PipelineStateCache)      │
             └───┬───────────────────────┬──────────────────▲──────────┘
                 │ Renderer→Scene        │ Renderer→Asset   │ Scene→Renderer
                 │ (BaseRenderer.h:46 等) │ (TextureManager) │ (Model.h:5-12 直接 include
                 ▼                       ▼                  │  PipelineDesc.h / Material.h)
             ┌───────────────────────┐  ┌──────────────────┴──────────┐
  场景 / 资产 │ Engine/05_Scene/{Model,│  │ Engine/03_AssetSystem/      │
             │  Camera,Lighting}/…    │  │  Importers/{glTF,Mesh,Model,│
             │                        │  │  Texture}/…                 │
             └───────┬───────────────┘  └──────────────┬──────────────┘
                     │ Scene→RHI（Model.h:5-11 直接   │ Asset→RHI（DDSTextureLoader
                     │ include CommandContext/GpuBuffer│  直接 CreateCommittedResource）
                     │ /UploadBuffer）                 │
                     ▼                                 ▼
             ┌──────────────────────────────────────────────────────────┐
  RHI        │ Engine/02_RHI/{GraphicsCore, Command/*, Descriptor/*,     │
             │  Pipeline/*, Resource/*, Memory/*, d3dx12.h}             │
             └──────────────────────────┬───────────────────────────────┘
                                        ▼
             ┌──────────────────────────────────────────────────────────┐
  Core       │ Engine/00_Core/{Utility,Math,Threading,Hash,PCH…}          │
             └──────────────────────────────────────────────────────────┘

  注：渲染器层还包括 04_Renderer 下的 Material/ Pipeline/ Shader/ RenderGraph/
      （Shader/ 与 RenderGraph/ 是空的，Material/ 只有 Material.h）
```

**真实依赖边（每条都有 file:line 证据）**

| 方向 | 边 | 证据 |
| --- | --- | --- |
| 应用层 → 渲染路径 | Demo 构造/持有渲染器 | `Demos/DeferredApp/DeferredApp.cpp:57`、`Demos/RayTracingApp/RayTracingApp.cpp:57`、`Demos/ForwardApp/Tooiyume.cpp` |
| 应用层 → 场景 | Demo 直接持有 `Camera` 与 `ModelInstance Models[10]` | `Demos/DeferredApp/DeferredApp.cpp:30-36`、`:83-84` |
| 应用层 → RHI | Demo 直接 `GraphicsContext::Begin/Finish`、`Display::Present` | `Demos/DeferredApp/DeferredApp.cpp:94-110`、`:113-131`；`Engine/01_Application/GameCore.cpp:72` |
| 渲染路径 → 场景 | `BaseRenderer.h:66-78 RenderFrameDesc{const Scene::Camera::Camera*; std::vector<Scene::Model::ModelInstance*>}`；`BaseRenderer.h:113-170 MeshSorter` 持 `Scene::Model::Mesh*` / `Scene::Material::Material*` | 实测头文件 |
| 渲染路径 → 资产 | `ForwardRenderer.cpp`/`DeferredRenderer.cpp` 调 `TextureManager::Initialize/Shutdown`、`LoadDDSFromFile` | `Engine/03_AssetSystem/Importers/Texture/TextureManager.h:26-38` |
| 渲染路径 → Core | `Utility::Printf`、`Math::Matrix4` | 全文件 |
| **场景 → 渲染路径（反向）** | `Engine/05_Scene/Model/Model.h:5-12` include `04_Renderer/Pipeline/PipelineDesc.h` 与 `04_Renderer/Material/Material.h` | 实测 include 行 |
| **场景 → RHI（反向）** | `Model.h:5-11` include `02_RHI/Command/CommandContext.h`、`02_RHI/Resource/GpuBuffer.h`、`02_RHI/Resource/Heap/UploadBuffer.h`；`Model.cpp:166/169` 直接写 `D3D12_RESOURCE_STATE_*` | 实测 |
| **场景 → 应用层（反向）** | `Engine/05_Scene/Camera/CameraController.h:3` include `01_Application/GameCore.h` | 实测 |
| 资产 → RHI | `DDSTextureLoader.cpp:838-839/879-880/938-939` 直接 `CreateCommittedResource`；`TextureManager.cpp:135/137/234` 直接 `D3D12_RESOURCE_STATE_*`/`D3D12_RESOURCE_DESC`/`D3D12_CPU_DESCRIPTOR_HANDLE` | 实测 |
| RHI → Core | `GraphicsCore.h:26-38` 使用 `Utility::`、ComPtr、Math | 实测 |

**结论**：`Application → Renderer → Scene/Asset → RHI → Core` 只在**声明层**成立；真实依赖存在三条反向边（Scene→Renderer、Scene→RHI、Scene→Application）与一条资产→RHI 的直连，**栈式分层不成立**。这正是《约束》原则 3（场景、材质与底层 RHI 解耦）要解决的问题。

### 3.1 应用层

#### 3.1.1 三个 Demo 的真实重复度

**统计方法（可复现）**
1. 逐行读三个 `main` 所在文件全文，按**函数/段**切块：`class 声明`、`wWinMain`、`资产路径函数`、`Startup`、`Update`、`RenderScene`、`Cleanup`、`成员变量`。
2. 对每一块做**逐段文本比对**（相同/等价/不同），统计「等价行数」与「仅差异行数」。
3. 行数口径：WT 实测（`wc -l`）`DeferredApp.cpp`=148、`Tooiyume.cpp`=157、`RayTracingApp.cpp`=148（HEAD 分别为 143/145/143）。
4. 重复比例 = 等价行数 ÷ 文件行数（对三个文件取平均），并把「可提取公共后仍需逐行保留的差异部分」单列。

**逐段比对结果**

| 段 | DeferredApp | Tooiyume（Forward） | RayTracingApp | 比对 |
| --- | --- | --- | --- | --- |
| `class Xxx : public GameCore::IGameApp` 声明 | `:22-36` | 同名同构 | `:22-36` | **等价**（仅类名与是否声明 `RequiresRaytracingSupport` 不同） |
| `wWinMain` → `GameCore::WinRun<T>` | `:52-55` | 同 | 同 | **等价**（仅类名字符串不同） |
| 全局渲染器对象 | `:57` `Renderer::Deferred::DeferredRenderer DeferredRenderer;` | ForwardRenderer | `:57` DeferredRenderer | **等价结构，类型不同（RayTracingApp 也用 Deferred！）** |
| 资产路径函数 | `:57-66`（6 个） | `:55-60`（5 个） | `:59-64`（5 个） | **等价**（同一批 `Asset::Resolve(L"Model/…")`） |
| `Startup` | `:70-92` | 同构 | 同构 | **等价**（差异：`forceRebuild` 来源、加载哪两个模型、描述符 desc 初始化） |
| `Update` | `:94-110` | 同构但**额外手动遍历 Models** | 同构 | **等价为主**，差异 3-6 行 |
| `RenderScene` | `:113-131` | 同 | 同 | **等价**（仅 PIX 名不同） |
| `Cleanup` | `:135-143` | 同 | 同 | **等价**（`Models[i]=nullptr` 加 `Shutdown()`） |

**结果与估算**
- **结构骨架约 85-95% 逐字等价**；其中 `wWinMain`/`RenderScene`/`Cleanup`/`Startup` 骨架近乎完全相同。
- 按行数口径：去掉公共骨架后，每个 Demo **真正独有的内容约 10-20 行**（渲染器类型 1 行、模型清单 2-6 行、`forceRebuild` 1 行、额外遍历 0-5 行、`RequiresRaytracingSupport` 0-2 行）。
- **重复比例估算 ≈ 85%**（等价行数 ≈ 125-135 / 148-157）。
- 方法局限（诚实标注）：**未做 token 级 diff 统计**，以上为逐段人工比对 + 行数统计的**推断值**；若要精确数字，需要写脚本做规范化（去掉类名/PIX 名）后的行级 diff。
- 旧审计给的「96% 相同」量级与我的结论同向，但**我的口径更保守（85%+）**，因为 Forward Demo 的 `Update` 与模型清单确有结构差异。

#### 3.1.2 `GameCore` 的职责，以及它承担了多少本该属于「渲染路径」的职责

**它做了什么（实测）**
| 职责 | 位置 | 是否越界 |
| --- | --- | --- |
| 窗口类注册/创建/销毁、消息循环 | `GameCore.cpp:97-155`（RegisterClassEx、CreateWindow、PeekMessage 循环） | 属于应用层，合理 |
| 应用生命周期编排（Initialize/Start/每帧 Update/终了 Shutdown） | `GameCore.cpp:28-56`、`:58-77`、`:79-86` | 合理 |
| 资产根解析 + 日志（WT 新增） | `GameCore.cpp:36-41` | 合理（启动期） |
| **帧循环固定为 `game.Update` → `game.RenderScene()` → `Display::Present()`** | `GameCore.cpp:58-77` | **越界**：把「最终合成 + Present」的顺序写死在应用框架里；渲染路径无法接管 present（例如无法插入后处理链、无法选择 present 目标） |
| **设备/交换链初始化与关闭** | `GameCore.cpp:43` `Graphics::Initialize(...)`、`:153` `Graphics::Shutdown()` | **半越界**：设备属于 RHI，交换链属于「输出」，目前由应用框架驱动 |
| **窗口 resize 消息直接路由到 Display** | `GameCore.cpp:158-175` `case WM_SIZE: Display::Resize(...)` | **越界**：resize 是「渲染目标重建」事件的源头，它只改了交换链；**不通知 Camera、不通知任何 Renderer**（§6） |
| `IGameApp::RenderUI` 钩子 | `GameCore.h:24` | 预留但零调用 |

**本该在渲染路径、现在落在应用层的职责（清单）**
1. **Present**：`Display::Present()` 由 `GameCore.cpp:72` 调用，渲染器无法参与最终合成（不存在的 PostProcess/Tonemap 段本应在这里）。
2. **输出目标绑定**：`Demos/DeferredApp/DeferredApp.cpp:76` `Display::SceneColorBuffer = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;` —— **应用层手动把渲染器的内部 RT 挂到 Display**，这是渲染路径的输出契约，却由 Demo 完成。
3. **渲染器的构造与生命周期**：Demo 直接定义全局渲染器对象并调用 `Initialize/Shutdown`（`DeferredApp.cpp:57`、`:75`、`:141`），渲染路径没有自己的工厂/管理器（`RendererRegistry` 是死代码）。
4. **相机与场景输入**：Demo 自己创建 `Camera`、`CameraController`、`ModelInstance` 并逐帧填 `RenderFrameDesc`（`DeferredApp.cpp:88-90`、`:94-110`），即「场景提交」目前是应用层责任，而渲染路径只被动接收。
5. **resize**：见上表。

**判定**：`GameCore` 目前是**应用框架 + 部分渲染路径职责（present/输出契约）+ 部分 RHI 生命周期（设备初始化）** 的混合体。按原则 1，需要把它收敛为「窗口/输入/主循环/场景选择」，把 Present 与输出契约移交渲染路径。

#### 3.1.3 WT 新增的两个文件的真实作用

**`AssetRoot.h/.cpp`（新建未跟踪，45 + 228 行）——「资源根目录解析」单点收敛**
- 解决什么问题：HEAD 有 **18 处硬编码绝对路径**（清单见 §3.4.3），换机器/CI 直接失败，且失败表现为「一行日志 + 黑屏」。
- 设计依据（源码注释自述）：`AssetRoot.h:5-25` 写明 5 级解析优先级与理由「资源目录既可能在源码树里（开发期），也可能被复制到 exe 旁边（发布期）」；实现分别在 `AssetRoot.cpp:92`（命令行）、`:112-116`（环境变量）、`:140`（exe 上溯 ≤8 层）、`:149-158`（cwd 上溯）、`:162-171`（编译期兜底）。
- 对外接口：`Initialize()`、`g_AssetsRoot`、`Resolve(relative)`、`Exists(relative)`、`LogResolution()`（`AssetRoot.h:30-44`）。
- 调用点（实测）：`GameCore.cpp:36-41`（启动期 Initialize+LogResolution）、`ModelLoader.cpp`（经 Demo 传入的绝对路径）、三 Demo 的资产函数、`DeferredRenderer.cpp:38-48` 的 `HDRTexturePath()`。
- 副作用与缺口：`AssetRoot.cpp:78-82` 的 `LooksLikeAssets()` **定义后零调用（小死代码）**；`Resolve()` 在根未解析时只报一次错并返回相对路径（`:192-202`），调用方若不做 `Exists()` 仍会静默失败。
- 与 6 原则的关系：属于原则 3/5 的**前置基础设施**（把「资源位置」从散落常量变成受控单点），不改变分层。

**`CommandLineArg.h/.cpp`（WT 16 + 106 行，HEAD 11 + 75 行）——启动参数从「有值键值对」扩展到「无值开关 + 系统命令行」**
- 新增 `InitializeFromSystemCommandLine()`（`CommandLineArg.h:8`）：用 `CommandLineToArgvW(GetCommandLineW())` 直接取系统命令行（`CommandLineArg.cpp:54-63`），因为 `GameCore` 的 `wWinMain` 之前拿不到 `argc/argv`。
- 新增 `HasFlag(key)`（`CommandLineArg.h:15`、`.cpp:94-97`）：注释自述「无值开关：命令行里出现 `-RebuildModels` 即视为开启」；配套的解析在 `GatherArgs` 的 `:31-41`（下一个 token 以 `-` 开头则记为空值）。
- 使用点：三 Demo 的 `forceRebuild = CommandLineArgs::HasFlag(L"RebuildModels")`（消除旧审计 P0#10 的写死 true）；`AssetRoot.cpp:92` 用 `GetString(L"AssetsRoot", …)`。
- 已知脆弱点：`GetInteger` 走 `std::stoi`（`CommandLineArg.cpp:78-84`）——空字符串会抛异常；目前无调用点，**未确认**是否会被后续工作包使用（见 §9）。

### 3.2 渲染路径

#### 3.2.1 类、继承与实例化

| 项 | 内容 | 证据 |
| --- | --- | --- |
| 基类 | `class BaseRenderer`（非虚析构未声明），成员 `m_CreateDesc`/`m_Initialized`/`m_DefaultPSO`/`m_MainViewport`/`m_MainScissor`/`m_NumRTVs`/`DefaultSorter` | `Renderer/Base/BaseRenderer.h:172-215` |
| 纯虚接口 | `GetName()` `Initialize(RendererCreateDesc)` `Shutdown()` `OnResize(w,h)` `Update(RenderFrameDesc, GraphicsContext&)` `Render(GraphicsContext&, RenderFrameDesc, DrawPass, BatchType)` `GetFeatures()` `GetPSO(PipelineDesc)` `BindRenderState(GraphicsContext&)` `BindMaterial(GraphicsContext&, const Material&)` | `BaseRenderer.h:174-194` |
| 有默认空实现的 | `BeginFrame()`、`EndFrame()` | `BaseRenderer.h:180-190` |
| 唯一非虚共享实现 | `ModelSort(const ModelInstance&)` → `GatherRenderables(DefaultSorter)` + `DefaultSorter.Sort()` | `BaseRenderer.cpp:19-24` |
| 唯一真正共享的数据结构 | `MeshSorter`（含 `SortKey`/`SortObject`/`AddMesh`/`Sort`/`Reset`） | `BaseRenderer.h:113-170`、`BaseRenderer.cpp:26-106` |
| 派生 1 | `class ForwardRenderer final : public BaseRenderer`，`GetName()` 返回 `L"ForwardRenderer "`（**尾随空格**） | `Renderer/Forward/ForwardRenderer.h:53-56` |
| 派生 2 | `class DeferredRenderer final : public BaseRenderer`，`GetName()` 返回 `L"DeferredRenderer"` | `Renderer/Deferred/DeferredRenderer.h:89-92` |
| 注册机制 | `RendererRegistry`（`Instance/RegisterRenderer/CreateRenderer` + `unordered_map<wstring,Creator>`）与模板 `RendererAutoRegister<T>` | `Renderer/Base/RendererRegistry.h:29-59`、`.cpp:5-22` |
| 自动注册对象 | `ForwardRenderer.cpp:27` `RendererAutoRegister<ForwardRenderer> s_RegisterForwardRenderer(L"ForwardRenderer ");`；`DeferredRenderer.cpp:33` `RendererAutoRegister<DeferredRenderer> s_RegisterForwardRenderer(L"DeferredRenderer");`（**变量名复制粘贴错**） | 实测 |
| **实例化方式** | **不是 Registry**：Demo 直接定义全局对象（`DeferredApp.cpp:57`、`RayTracingApp.cpp:57`），Forward Demo 同样直接持有 `ForwardRenderer` | `RendererRegistry.cpp:16-22` 零调用者 |
| `GetFeatures()` 返回值 | 两者**完全相同**：`Deferred \| Shadow \| SSAO \| Transparent \| PostProcess` —— **Forward 也自称 Deferred、且都缺 `Forward` 位** | `ForwardRenderer.h:70-77`、`DeferredRenderer.h:106-113` |

#### 3.2.2 谁调用谁（每帧真实调用链，含 PIX 事件名）

```
GameCore::WinRun<DeferredApp>                     Demos/DeferredApp/DeferredApp.cpp:52-55
└─ GameCore::RunApplication                        Engine/01_Application/GameCore.cpp:97-155
   ├─ InitializeApplication (:28-56)
   │   Asset::Initialize/LogResolution (:36-41) → Graphics::Initialize (:43) → SystemTime/GameInput → game.Startup()
   │   └─ DeferredApp::Startup                     DeferredApp.cpp:70-92
   │      ├─ DeferredRenderer.Initialize(desc)      DeferredApp.cpp:75
   │      │   └─ DeferredRenderer::Initialize      DeferredRenderer.cpp:60-104
   │      │      BuildRootSignature / BuildPSOs / BuildDescriptorHeaps
   │      │      → TextureManager::Initialize(L"") → CreateDeferredBufferTargets (:447-479，
   │      │        经 BufferManager::CreateColorBuffer×5 + CreateDepthBuffer×1)
   │      │      → BuildLightingSignature / BuildLightingPSO
   │      │      → Scene::LightingSystem::InitializeResources()
   │      │      → s_EnvironmentLightingManager.Initialize + LoadHDR(HDRTexturePath())
   │      ├─ Display::SceneColorBuffer = ...GetDeferredBuffer().SceneColorBuffer   DeferredApp.cpp:76  ← 应用层绑定输出
   │      └─ Scene::Model::LoadModel ×2 → ModelLoader::LoadModel  ModelLoader.cpp:189-264
   └─ 帧循环 (:136-150)
      ├─ UpdateApplication (:58-77)
      │   ├─ game.Update(dt) → DeferredApp::Update   DeferredApp.cpp:94-110
      │   │   GraphicsContext::Begin(L"Scene Update") → CameraController::Update
      │   │   → DeferredRenderer.Update              DeferredRenderer.cpp:140-163
      │   │      ├─ ModelInstance::Update(gfxContext, dt)（IsDirty 时）→ Model::Update
      │   │      ├─ BaseRenderer::ModelSort → Model::GatherRenderables → MeshSorter::Sort
      │   │      └─ s_EnvironmentLightingManager.BakeEnvironmentTextures (:154，,:150 守卫后仅首帧真烘)
      │   │      → gfxContext.Finish()
      │   ├─ game.RenderScene() → DeferredApp::RenderScene   DeferredApp.cpp:113-131
      │   │   GraphicsContext::Begin(L"Scene DeferredRenderer ")
      │   │   → DeferredRenderer::Render              DeferredRenderer.cpp:167-373
      │   │      PIXBeginEvent(L"DeferredRenderer") (:202)
      │   │      → BindRenderState（SetRootSignature(m_GBufferRootSig)+SetDescriptorHeap×2+SetPrimitiveTopology）
      │   │      → 4 张 GBuffer + SceneColor 转 RENDER_TARGET 并 ClearColor，SceneDepth 转 DEPTH_WRITE 并 ClearDepth，
      │   │        SetRenderTargets(4, m_CPUGBuffers, DSV)
      │   │      → [GBufferPass]  PIX :231-271  遍历 DefaultSorter：SetConstantBuffer(kMeshConstants)
      │   │        → BindMaterial → SetPipelineState(GetPSO(desc)) → SetVertexBuffer/SetIndexBuffer → DrawIndexed
      │   │      → [SSAO] 空壳  [Shadow] 空壳
      │   │      → [DeferredLightingPass]  PIX :287-335
      │   │        SetRootSignature(m_LightingRootSig)+SetPipelineState(m_LightingPSO)
      │   │        → 4 GBuffer + Depth SRV + 3 张 IBL 描述符（:331-333 用 const_cast 转状态）
      │   │        → SetConstantBuffer(Light  :322 / Camera :324-327) → SetRenderTarget(SceneColor) → Draw(3,0)
      │   │      → [SkyPass]  PIX :337-347 → EnvironmentLightingManager::SkyboxPass (:325-356) → Draw(3,0)
      │   │      → [Transparent] 空壳 [PostProcess] 空壳 [TAA] 空壳 [Tonemap] 空壳
      │   │      PIXEndEvent (:372)
      │   │      → DeferredRenderer::EndFrame (:375-386)：只对 Models 调 ClearDirty，**不 Reset sorter**
      │   │      → gfxContext.Finish()
      │   └─ Display::Present() → PreparePresentSDR  Display.cpp:251-263
      │        GraphicsContext::Begin(L"Present") → TransitionResource ×3 → CopyBuffer →
      │        s_SwapChain1->Present(0, 0) → g_CurrentBuffer = (g_CurrentBuffer + 1) % 3
      └─ WndProc（异步）: case WM_SIZE → Display::Resize(w,h)   GameCore.cpp:158-175  ← 断链点，见 §6
```

**PIX 事件名清单（真实存在的，按出现顺序）**：`DeferredRenderer`（`DeferredRenderer.cpp:202`）→ `GBufferPass`（`:231`）→ `SSAOPass`（`:274`）→ `ShadowPass`（`:281`）→ `DeferredLightingPass`（`:289`）→ `SkyPass`（`:339`）→ `Transparent Forward Pass`（`:349`）→ `PostProcess Pass`（`:355`）→ `TAA Pass`（`:361`）→ `Tonemap Pass`（`:367`）→ `PIXEndEvent`（`:372`）。Forward 侧：`ForwardRenderer `（`ForwardRenderer.cpp:158`）→ `ShadowPass`（`:159`，**只有 Begin 没有 End，事件不成对**）→ 末尾**无 PIXEndEvent**。Display 侧：只有 context ID `Present`（`Display.cpp:253`），**无 PIX 事件**。

**两个每帧不一致的细节**：`ForwardRenderer::EndFrame` 调 `DefaultSorter.Reset()`（`ForwardRenderer.cpp:251`），`DeferredRenderer::EndFrame` **不调**（`DeferredRenderer.cpp:375-386`）→ Deferred 的 `MeshSorter` 跨帧残留（`Reset` 的实现见 `BaseRenderer.cpp:26-32`，它也不清 `m_CurrentPass`）。

#### 3.2.3 `BaseRenderer` 是否真的被复用 —— **结论：没有**

`BaseRenderer.cpp` 全文只有两处共享实现：`ModelSort`（`:19-24`）与 `MeshSorter`（`:26-106`）。以下 8 段在两份实现里几乎逐字重复（行号取自 HEAD）：

| 重复段 | ForwardRenderer.cpp | DeferredRenderer.cpp | 差异 |
| --- | --- | --- | --- |
| `Initialize` 骨架 | `:36-75` | `:60-104` | 目标创建少 4 张 RT；Deferred 多 Lighting/IBL 两步 |
| `Shutdown` | `:77-91` | `:106-120` | Deferred 多 EnvLightingManager::Shutdown |
| `OnResize` | `:93-102` | `:122-131` | 仅名字不同 |
| `BuildDescriptorHeaps` | `:254-261` | `:388-395` | 仅堆名不同（4096 / 2048 与 `m_CommonTextures` 相同） |
| `BuildRootSignature` | `:263-284` | `:397-421` | **同 6 个根参数布局**，只有名字不同（且 Forward 的 `:267/:273` 重复 `Reset(kNumRootBindings,1)`） |
| `GetPSOIndex` 的 input layout 与状态分叉 | `:346-399` | `:501-550` | Deferred 多 `kVertex_Tangent` 要求；其余分支同构 |
| `BindRenderState` | `:402-407` | `:553-560` | 仅根签名成员名不同（Forward **缺 SetPrimitiveTopology**） |
| `BindMaterial` | `:409-413` | `:562-567` | **Forward 不设 material CBV**（`:409-413`），Deferred 设（`:562-567`） |

**真正不同的只有 4 件事**：① 渲染目标（1 张 SceneColor vs 4 张 GBuffer + SceneColor + Velocity）；② shader 来源（`g_pDefaultVS/PS` vs `StaticMeshVS_cso/GBufferPS_cso`）；③ `kZPass` 是否用 `vbDepthOffset`（Forward 用 `:230`，Deferred 不用）；④ Deferred 多出 Lighting/IBL/Sky 三个阶段。

→ 所以 `BaseRenderer` 是一个**接口**，不是可复用基类；两条管线共享的是 `MeshSorter` 与 `Material` 结构体。

#### 3.2.4 `ForwardRenderer` 是否使用入库的旧 `.h` 字节码 —— **HEAD：是；WT：已改**（含 ShaderLibrary 加载方式核实）

1. **HEAD 用的是提交进仓库的预编译字节码头**：`ForwardRenderer.cpp:16-17` include `Shader/01_Default/DefaultVS.h` 与 `DefaultPS.h`；这两个文件在 HEAD 中存在，索引删除量实测为 `DefaultVS.h` 876 行、`DefaultPS.h` 649 行 —— 即内嵌 `g_pDefaultVS` / `g_pDefaultPS` 的 DXIL 字节数组头。
2. **同目录 `.hlsl` 不参与编译**：HEAD 的 `Engine/CMakeLists.txt` 只 `compile_shader` 了 `02_Deferred`、`04_PostProcess`、`05_BRDF`、`06_Environment` 等组，**从未包含 `01_Default`**；WT 的 diff 注释原话可作证（「此前从未进入 CMake … 改了 HLSL 不会重编译，Forward 永远跑旧的」）。
3. **没有任何 ShaderLibrary 加载机制**：`Engine/04_Renderer/Shader/ShaderLibrary.h` 与 `ShaderLibrary.cpp` **都是 0 行**；`grep ShaderLibrary` 无引用。Forward 的做法是把 include 进来的字节码**直接**交给 `GraphicsPSO::SetVertexShader/SetPixelShader`（`ForwardRenderer.cpp:296-299` 一带，`SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS))`）。Forward 的 `BuildPSOs` 里 `Finalize()` 与 `push_back` 都被注释（`:286-308`），`m_DefaultPSO` 只赋值未 Finalize。
4. **WT 的变化**：include 改为 `"DefaultVS.h"` / `"DefaultPS.h"`（由 `compile_shader` 生成到 `build/ShaderCache`），`Engine/Shader/01_Default/DefaultVS.h`、`DefaultPS.h` 已从索引删除，且 `Engine/CMakeLists.txt` 把 `${CMAKE_BINARY_DIR}/ShaderCache` 放到 include 顺序最前，防止源码目录里的同名旧头被优先命中。
5. 未穷举：`Assets/` 下是否还有其他 `.cso`/`.dxil`/字节码头，我未逐一扫描（§9）。

### 3.3 公共渲染功能层 —— **目前不存在这一层**

逐个定位（职责 / 使用者 / 判定）：

| 模块 | 真实职责（实测） | 使用者 | 判定 |
| --- | --- | --- | --- |
| `Engine/04_Renderer/BufferManager.h/.cpp` | 全局「名字 → `shared_ptr<ColorBuffer>/<DepthBuffer>`」映射 + `CreateColorBuffer/CreateDepthBuffer/DestroyAll/DestroyColorBuffer/DestroyDepthBuffer`；`:20-29` 一批 `#define`（`DEPTH_FORMAT`/`BASE_COLOR_FORMAT`/`NORMAL_FORMAT`/`MATERIAL_FORMAT`/`HDR_COLOR_FORMAT`/`SDR_COLOR_FORMAT`/`NORMAL_COLOR_FORMAT`/`DSV_FORMAT`）**在别处零引用（HDR/SDR 是遗留）** | `DeferredRenderer.cpp:447-479`（创建）、`:481-484`（`DestroyDeferredBufferTargets(){ BufferManager::DestroyAll(); }`）、`ForwardRenderer.cpp:311-323` | **雏形，但语义是全局资源池**：`BufferManager.cpp:36-48` 同名直接覆盖（旧 buffer 的 `shared_ptr` 当场析构）；`DestroyAll` 无视归属一键清空 → 两个渲染器不能共存 |
| `Engine/04_Renderer/EnvironmentLightingManager.h/.cpp`（389 行） | HDR 加载（`LoadHDR`）、env cube / irradiance / prefilter / BRDFLUT 四段 Compute 烘焙（`BakeEnvironmentTextures` → `:154-157`）、Skybox 绘制（`SkyboxPass` → `Draw(3,0)`） | **全局单例** `Renderer::EnvironmentLighting s_EnvironmentLightingManager`（`DeferredRenderer.cpp:38`），调用点 `:154`（烘焙）与 `:344`（Skybox）；Forward 完全不用 | **雏形，且是真实工作的功能**（IBL 四段 Compute 真的有代码）。问题：① 单例定义在渲染器 cpp 里（`:38`）；② IBL 三张 cube 的**状态转换由 DeferredRenderer 负责**（`:331-333` 用 `const_cast<ColorBuffer*>`），职责倒置；③ `SaveDDS()` 空（`:122-126`）；④ 输出 cube 尺寸与 mip 数硬编码为成员默认值（`.h:103-107`） |
| `Engine/04_Renderer/Shader/ShaderLibrary.h/.cpp` | —— | 无引用 | **死代码（两个 0 行空文件）** |
| `Engine/04_Renderer/Pipeline/PipelineStateCache.h/.cpp` | `PipelineDesc` 的**索引去重表**：`GetPipelineIndex(const PipelineDesc&)` 线性查重、`GetPipelineDesc(uint16_t)`、`Reset()`、`s_PipelineDescs`（`ASSERT(<0xffff)`） | `BaseRenderer.cpp:55-57`（`AddMesh` 里为每个 draw 求 psoIdx / depthPSO） | **雏形，但只管「索引」不管「PSO 编译与缓存」** —— 真正的 PSO 编译仍在两个渲染器各自的 `GetPSOIndex()` 里各写一份（`ForwardRenderer.cpp:346-399`、`DeferredRenderer.cpp:501-550`） |
| `Engine/04_Renderer/Pipeline/PSOBuilder.h/.cpp` | —— | 无引用 | **死代码（两个 0 行空文件）** |
| `Engine/04_Renderer/TemporalEffects.h/.cpp` + `Engine/Shader/04_PostProcess/TAACS.hlsl` | `TemporalEffects.h:11-39` 声明 `EnableTAA`/`Initialize`/`Shutdown`/`Update`/`GetFrameIndexMod2`/`GetJitterOffset`/`ClearHistory`/`ResolveImage` | 无（`GraphicsCore.cpp` 里的初始化整段被注释；`DeferredRenderer.cpp` 的 TAA 段是空壳） | **死代码（声明齐全、无实现、无调用）**；`TAACS.hlsl` 同理 |
| `Engine/04_Renderer/Material/Material.h`（48 行）+ `MaterialConstantData` | `MaterialFlags`（AlphaBlend/AlphaTest/DoubleSided）、`Material{Flags, SRVTable, SamplerTable, MaterialCBV}`、`MaterialConstantData`（与 HLSL `MaterialConstants` 手写镜像） | `BaseRenderer` 的 `MeshSorter::AddMesh`（排序键）、`ModelLoader.cpp`（建表）、两个渲染器的 `BindMaterial` | **真实共享但极薄**：只管「标志 + 两张描述符表 + CBV 地址」，不含贴图加载、不含 PBR 参数校验、不含 AlphaTest 的 HLSL 侧实现（HLSL 里根本没有 `alphaRef`） |
| `Engine/04_Renderer/Material/MaterialFlags.h` | —— | 无引用 | **死代码（0 行）** |
| `Engine/04_Renderer/RenderGraph/` | —— | —— | **空目录（死代码）** |
| `MeshSorter`（在 `BaseRenderer.h:113-170`） | 按 pass 分桶 + 排序键（`objectIdx:16/psoIdx:12/key:32/passID:4`）+ `AddMesh` 的 alpha/深度分支 | Forward 与 Deferred 都经 `ModelSort` 使用（`BaseRenderer.cpp:19-24`） | **唯一真正被两条管线复用的渲染实现** |

**结论**：不存在「公共渲染功能层」。当前只有 4 块散件 —— `MeshSorter`（真实复用）、`Material` 结构（真实复用但薄）、`BufferManager`（名字池）、`EnvironmentLightingManager`（真实功能但单例 + 职责倒置）；`ShaderLibrary`/`PSOBuilder`/`TemporalEffects`/`MaterialFlags`/`RenderGraph` 是**空文件或死代码**。这正是《约束》原则 2 的现状起点。

### 3.4 场景与资产

#### 3.4.1 模块职责与依赖方向（实测）

| 模块 | 职责（签名级） | 依赖（include 实测） | 使用者 |
| --- | --- | --- | --- |
| `Engine/05_Scene/Camera/Camera.h/.cpp` | `BaseCamera`（`Update` `SetLookDirection` `SetEyeAtUp` `SetPosition` `GetViewMatrix/GetProjMatrix/GetViewProjMatrix/GetInViewProjMatrix` `GetReprojectionMatrix`）+ `Camera`（`SetPerspectiveMatrix` `SetFOV` `SetAspectRatio` `SetZRange` `ReverseZ` `GetClearDepth`）；约定右手系/Forward=-Z/行向量/行主序/深度[0,1]/默认非 ReverseZ | `00_Core/Math` | Demo（`m_Camera`）、`DeferredRenderer`（经 `RenderFrameDesc`）、`EnvironmentLightingManager::SkyboxPass` |
| `Engine/05_Scene/Camera/CameraController.h` | `CameraController`（抽象，`Update(float)` = 0）、`FlyingFPSCamera`（动量/灵敏度/朝向俯仰）、`OrbitCamera`（**空类**，注释「暂时阉割」） | **include `01_Application/GameCore.h`（反向依赖）** | Demo（`unique_ptr<CameraController>`） |
| `Engine/05_Scene/Camera/ShadowCamera.h/.cpp` | `UpdateMatrix(LightDirection, ShadowCenter, ShadowBounds, W, H, Precision)`、`GetShadowMatrix()` | `Camera.h` | **无使用者；`.cpp:6-11` 函数体为空、`m_ShadowMatrix` 从不写入** |
| `Engine/05_Scene/Model/Model.h/.cpp` | `Mesh`（纯资源描述：`vbOffset/vbSize/vbDepthOffset/vbDepthSize/ibOffset/ibSize` + `meshCBV/materialSlotIdx/srvTable/vertexFlags/numDraws` + `Draw{primCount,startIndex,baseVertex}`）、`GraphNode`（含 `matrixIdx/hasSibling/hasChildren/staleMatrix/skeletonRoot` 位域）、`Model`（资产容器：`ByteAddressBuffer m_DataBuffer`、`m_MaterialConstants`、`m_MeshData`、`m_SceneGraph`、`textures`、`m_Materials`）、`ModelInstance`（`Update(GraphicsContext&,float)`、`GatherRenderables(MeshSorter&)`、脏标记、`UploadBuffer m_MeshConstantsCPU` + `ByteAddressBuffer m_MeshConstantsGPU`、变换与包围球） | **include RHI（`CommandContext.h`/`GpuBuffer.h`/`UploadBuffer.h`）+ 资产层（`TextureManager.h`）+ 渲染器层（`PipelineDesc.h`/`Material.h`）** | Demo（`Models[10]`）、`BaseRenderer::ModelSort`、`DeferredRenderer::Update/Render` |
| `Engine/05_Scene/Model/ModelLoader.cpp`（377 行） | `.ty` 缓存优先 → `_wstat64` 比较 mtime → 需要时 `glTF::Asset` 解析 + `ModelConvert::BuildModel` → 建材质常量与纹理表（`DefaultTextures` 见 §2.3#2）；失败只 `Printf` 后 `return nullptr` | `glTF.h`、`ModelConvert.h`、`TextureManager`、`Renderer::s_TextureHeap` | Demo（`LoadModel`） |
| `Engine/05_Scene/Lighting/LightingSystem.h/.cpp`（80 行） | `LightData`（`DirectionIntensity/DirectionalColor/PointLightPositionRange/PointLightColorIntensity`，注释要求与 HLSL 一致）、`MaxLights=1`、`InitializeResources/Shutdown/CreateLights/UpdataState(GraphicsContext&)`（**拼写 Updata**） | `02_RHI/Resource/GpuBuffer.h`、`CommandContext` | `DeferredRenderer::Initialize`（Init）、`DeferredRenderer::Render`（`:322` 绑 `m_LightGPUBuffer.GetGpuVirtualAddress()`）；Forward 只 Init 不用 |
| `Engine/03_AssetSystem/Importers/glTF.h/.cpp` | glTF 2.0 解析：`Asset`/`Mesh{primitives, skin}`/`Primitive{attributes[kNumAttribs], indices, material, attribMask, mode, minPos/maxPos, minIndex/maxIndex}`/`Material`（`flags` union 含 `alphaCutoff:16`、`emissiveFactor[3]`、`normalTextureScale`、**自己的纹理槽 enum（`:138`）**、`textures[kNumTextures]`） | `json.hpp` | `ModelLoader::LoadModel`（`:242-262` 分派） |
| `Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp`、`.../Model/ModelConvert.cpp` | 顶点/索引/深度 VB 打包（`ModelConvert.cpp:123-141`，含 §2.3#3 的偏移 bug）、材质常量填充（`:270-294`，含 §2.3#1 的槽错配） | RHI 类型（`D3D12_INPUT_ELEMENT_DESC` 等） | `ModelLoader` |
| `Engine/03_AssetSystem/Importers/Texture/TextureManager.h/.cpp` | `Initialize(RootPath)`/`Shutdown()`/`LoadDDSFromFile(path, fallback=kMagenta2D, sRGB=false)`；`TextureRef` 引用计数包装，`GetSRV()` 返回 **`D3D12_CPU_DESCRIPTOR_HANDLE`** | `02_RHI/*`、`DDSTextureLoader` | 两个渲染器（`Initialize`/`Shutdown`）、`ModelLoader`、`EnvironmentLightingManager` |
| `Engine/03_AssetSystem/Importers/Texture/DDSTextureLoader.h/.cpp` | 从内存/文件创建 DDS 纹理（MiniEngine 血缘的第三方风格代码） | **直接使用 `ID3D12Device*`、`CreateCommittedResource`、`D3D12_HEAP_PROPERTIES`、`D3D12_RESOURCE_DESC`、`D3D12_CPU_DESCRIPTOR_HANDLE`** | `TextureManager` |

**依赖方向总结（场景与资产）**：`应用层 → 场景/资产 → RHI` 的**下行**边存在；同时存在 `场景 → 渲染器`（`Model.h:10-11`）、`场景 → 应用层`（`CameraController.h:3`）、`资产 → RHI` 直连（`DDSTextureLoader.cpp:838-839`）三条**上行/旁路**边。按原则 3，场景与材质系统不应依赖具体渲染路径 —— 现状是**直接 include `04_Renderer/Pipeline/PipelineDesc.h` 与 `04_Renderer/Material/Material.h`**（`Model.h:10-11`）。

#### 3.4.2 `Material` 的双重身份（值得单列）

- `Engine/04_Renderer/Material/Material.h` 定义运行期材质（`Flags`/`SRVTable`/`SamplerTable`/`MaterialCBV`）；
- `Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h:29-58` 定义 GPU 侧 `MaterialConstants`（与 `MaterialConstantData` 手写镜像）；
- `Engine/03_AssetSystem/Importers/glTF.h:120-139` 定义导入期材质（含自己的 slot enum）；
- `Engine/Shader/01_Default/DefaultPS.hlsl:44-48` 又定义一套 HLSL 侧 flag 常量。
→ **同一概念在 4 处定义、3 套纹理槽顺序**，且没有任何编译期一致性检查（`MaterialConstantData` 注释自称「Unaligned mirror」）。这是一条**结构性债务**，也是 §2.3#1 的根因。

#### 3.4.3 资产路径解析现状：HEAD 的 18 处硬编码绝对路径（逐处 `file:line`）

HEAD（`git grep D:/CS-Self-Study HEAD`，实测恰好 18 处）：

| # | 文件:行 | 内容 |
| --- | --- | --- |
| 1 | `Demos/DeferredApp/DeferredApp.cpp:55` | `std::wstring saber_emiya = L"D:/CS-Self-Study/…/Assets/Model/Saber/saber_emiya.glb";` |
| 2 | `Demos/DeferredApp/DeferredApp.cpp:56` | `toneriko = …/Model/toneriko/toneriko1.gltf` |
| 3 | `Demos/DeferredApp/DeferredApp.cpp:57` | `saberCaster3NoArmor = …/Model/saberCaster/saberCaster3NoArmor.gltf` |
| 4 | `Demos/DeferredApp/DeferredApp.cpp:58` | `ChosenSword = …/Model/saberCaster/ChosenSword.gltf` |
| 5 | `Demos/DeferredApp/DeferredApp.cpp:60` | `KyomachiyaA = …/Model/Kyomachiya/KyomachiyaA.gltf` |
| 6 | `Demos/DeferredApp/DeferredApp.cpp:62` | `leimiaier = …/Model/leimiaier/leimiaierALL.gltf` |
| 7 | `Demos/DeferredApp/DeferredApp.cpp:64` | `merlin = …/Model/Merlin/merlin.gltf` |
| 8 | `Demos/ForwardApp/Tooiyume.cpp:53` | `saber_emiya = …/Model/Saber/saber_emiya.glb` |
| 9 | `Demos/ForwardApp/Tooiyume.cpp:54` | `toneriko = …` |
| 10 | `Demos/ForwardApp/Tooiyume.cpp:55` | `saberCaster3NoArmor = …` |
| 11 | `Demos/ForwardApp/Tooiyume.cpp:56` | `ChosenSword = …` |
| 12 | `Demos/ForwardApp/Tooiyume.cpp:58` | `KyomachiyaA = …` |
| 13 | `Demos/RayTracingApp/RayTracingApp.cpp:57` | `saber_emiya = …` |
| 14 | `Demos/RayTracingApp/RayTracingApp.cpp:58` | `toneriko = …` |
| 15 | `Demos/RayTracingApp/RayTracingApp.cpp:59` | `saberCaster3NoArmor = …` |
| 16 | `Demos/RayTracingApp/RayTracingApp.cpp:60` | `ChosenSword = …` |
| 17 | `Demos/RayTracingApp/RayTracingApp.cpp:62` | `KyomachiyaA = …` |
| 18 | `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:40` | `constexpr wchar_t HDRtexture[] = L"D:/CS-Self-Study/…/Assets/Skybox/grasslands_sunset_2k.dds";`（**引擎层**） |

**WT 状态**：第 1-17 处改为 `Asset::Resolve(L"Model/…")`（`DeferredApp.cpp:57-66`、`Tooiyume.cpp:55-60`、`RayTracingApp.cpp:59-64`）；第 18 处改为 `Asset::Resolve(L"Skybox/grasslands_sunset_2k.dds")`（`DeferredRenderer.cpp:38-48` 的 `kHDRTextureRelative` + `HDRTexturePath()`），并加了 `Asset::Exists` 的缺失提示。**全仓库仅剩 `AssetRoot.h:10` 的注释示例**。

**解析链（WT 生效时）**：`GameCore.cpp:36-41` 启动期 `CommandLineArgs::InitializeFromSystemCommandLine()` → `Asset::Initialize()` → `Asset::LogResolution()`；解析优先级 5 级见 §3.1.3。**残余风险**：`AssetRoot.cpp:190-202` 在根未解析时只报一次错并返回**相对路径**；`DeferredRenderer`/Demo 不再校验 `Exists`（只有 HDR 一处校验）→ 仍可能退化为「黑屏 + 一行日志」。

---

## 4. RHI 职责边界与细节泄漏点（逐处 `file:line`）

### 4.1 边界内各类型职责（实测签名）

| 类型 | 职责边界 | 证据 |
| --- | --- | --- |
| `Engine/02_RHI/GraphicsCore.h/.cpp` | 设备/工厂/队列/上下文管理器的**全局持有**与初始化：`void Initialize(bool RequireDXRsupport=false); void Shutdown();`（`:20-21`）、`extern ID3D12Device* g_Device`（`:26`）、`extern ComPtr<IDXGIFactory6> g_DXGIFactory`（`:27`）、`extern CommandListManager g_CommandManager; extern ContextManager g_ContextManager;`（`:28-29`）、`g_bTypedUAVLoadSupport_R11G11B10_FLOAT/R16G16B16A16_FLOAT`（`:31-32`）、`extern DescriptorAllocator g_DescriptorAllocator[]` + `inline AllocateDescriptor(Type,Count=1)`（`:34-38`） | 实测 |
| `Engine/02_RHI/Command/CommandContext.h/.cpp` | 命令录制上下文：`Begin(ID)`（`:120`）、`Finish(bool WaitForCompletion=false)`（`:124`）、屏障（`TransitionResource`/`BeginResourceTransition`/`InsertUAVBarrier`/`InsertAliasBarrier`/`FlushResourceBarriers`，`:161-166`）、动态描述符表（`:265-266`）、动态常量/顶点/索引缓冲（`:490-502`、`:511-538`）、拷贝与初始化（`:40-55`）、调试事件（`:69-73`）、`ReserveUploadMemory`（`:148`）；成员 `ComPtr<ID3D12GraphicsCommandList4> m_CommandList`（`:189`）、两个 `LinearAllocator`、两个 `DynamicDescriptorHeap`、`m_ResourceBarrierBuffer[16]` | 实测 |
| `Engine/02_RHI/Command/CommandListManager.h/.cpp` | 队列与围栏：`CommandQueue`（`IncrementFence/IsFenceComplete/StallForFence/WaitForFence/WaitForIdle(=WaitForFence(IncrementFence()))/ExecuteCommandList/RequestAllocator/DiscardAllocator`，`:11-55`）、`CommandListManager`（`GetGraphicsQueue/GetComputeQueue/GetCopyQueue/GetQueue(Type)/CreateNewCommandList/IsFenceComplete/WaitForFence/IdleGPU`，`:59-111`）；fence 高 8 位存队列 Type（`:14-15`、`:88-93`、`:150-157`） | 实测 |
| `Engine/02_RHI/Descriptor/DescriptorHeap.h/.cpp` | 描述符分配：`DescriptorAllocator`（256/堆、`sm_AllocationMutex`、`RequestNewHeap`，`:21-49`）、`DescriptorHandle`（CPU+GPU 双端，`operator D3D12_CPU/GPU_DESCRIPTOR_HANDLE`，`:82-83`）、`DescriptorHeap`（shader-visible 堆：`Create/HasAvailableSpace/Alloc/operator[]/GetOffsetOfHandle/GetHeapPointer`，`:98-132`；`Destroy()` 只置空不归还，`:105`） | 实测 |
| `Engine/02_RHI/Descriptor/DynamicDescriptorHeap.h/.cpp` | 为 root descriptor table 动态分配并 `Commit*RootDescriptorTables`（由 `CommandContext` 内部持有使用） | `CommandContext.h:98-99`、`:587-589` |
| `Engine/02_RHI/Pipeline/RootSignature.h/.cpp` | `RootParameter`（`InitAsConstants/InitAsConstantBuffer/InitAsBufferSRV/InitAsBufferUAV/InitAsDescriptorRange/InitAsDescriptorTable/SetTableRange`，`:7-92`）与 `RootSignature`（`Reset/operator[]/InitStaticSampler/Finalize(name,Flags)`，`:101-169`；含 `m_DescriptorTableBitMap/m_SamplerTableBitMap/m_DescriptorTableSize[16]`） | 实测 |
| `Engine/02_RHI/Pipeline/PipelineState.h/.cpp` | `PSO`（`SetName/SetRootSignature/GetRootSignature/GetPipelineStateObject`，`:14-35`）、`GraphicsPSO`（各类状态设置 + `Finalize()`，`:37-76`）、`ComputePSO`（`:78-93`） | 实测 |
| `Engine/02_RHI/Resource/GpuResource.h/.cpp` | 所有 GPU 资源的基类：`m_pResource(ComPtr<ID3D12Resource>)`、`m_UsageState`、`m_TransitioningState`（`-1` 哨兵）、`m_VersionID`、`m_GpuVirtualAddress`；`virtual void Destroy() { m_pResource=nullptr; m_GpuVirtualAddress=NULL; ++m_VersionID; }`（`:30-35`）；`operator->`/`GetResource()`/`GetAddressOf()`/**`GetGpuVirtualAddress()`（对外返回裸 GPUVA）**/`GetVersionID()`（`:37-47`） | 实测 |
| `Engine/02_RHI/Resource/PixelBuffer.h/.cpp` | 纹理基类：宽高深度格式、`DescribeTex2D`、`AssociateWithResource`、`CreateTextureResource`、格式换算静态函数 | `PixelBuffer.h:9-41` |
| `Engine/02_RHI/Resource/ColorBuffer.h/.cpp` | 颜色/贴图资源：`CreateFromSwapChain/Create/CreateArray/CreateCube`、`GetSRV/GetRTV/GetUAV(MipSlice)`（返回 `D3D12_CPU_DESCRIPTOR_HANDLE`）、`SetClearColor`、`GenerateMipMaps`、`CreateDerivedViews/CreateCubeDerivedViews` | `ColorBuffer.h:21-114` |
| `Engine/02_RHI/Resource/DepthBuffer.h/.cpp` | 深度资源与 4 种只读 DSV 变体：`GetDSV/GetDSV_DepthReadOnly/GetDSV_StencilReadOnly/GetDSV_ReadOnly`、`GetDepthSRV/GetStencilSRV` | `DepthBuffer.h:21-39` |
| `Engine/02_RHI/Resource/GpuBuffer.h/.cpp` | 缓冲资源：`Create`/`Create(srcData)`/`CreatePlaced`、`GetUAV/GetSRV`、`CreateConstantBufferView/RootConstantBufferView`、`VertexBufferView/IndexBufferView`、`DescribeBuffer`；派生 `ByteAddressBuffer`/`IndirectArgsBuffer`/`StructuredBuffer`（含 counter）/`TypedBuffer` | `GpuBuffer.h:10-142` |
| `Engine/02_RHI/Resource/Heap/UploadBuffer.h`、`ReadbackBuffer.h` | `UploadBuffer`（`Create/Map/Unmap/GetBufferSize`）、`ReadbackBuffer`（`Create/Map/Unmap`，`CreateDerivedViews()` 空实现） | `UploadBuffer.h:5-16`、`ReadbackBuffer.h:9-21` |
| `Engine/02_RHI/Memory/LinearAllocator.h/.cpp` | 线性分配器（`CommandContext` 的 CPU/GPU 分配器与 `DynamicDescriptorHeap` 的底层页） | `CommandContext.h:105-106` |
| `Engine/02_RHI/Pipeline/GraphicsCommon.h/.cpp` | 默认纹理/采样器/光栅化/深度/混合状态常量：`kMagenta2D`/`kBlackOpaque2D`/`kBlackTransparent2D`/`kWhiteOpaque2D`/`kWhiteTransparent2D`/`kDefaultNormalMap`/`kBlackCubeMap`（`:91-104`）、`RasterizerDefault/RasterizerFullScreen`（`:108-130`）、`DepthStateDisabled/ReadWrite/ReadOnly`（`:133-153`）、混合（`:156-167`） | 实测 |
| `Engine/02_RHI/Pipeline/SamplerManager.h/.cpp` | `SamplerDesc::CreateDescriptor()` 与采样器缓存（**缓存去重实际失效，见 §2.4**） | `SamplerManager.cpp:20-38` |
| `Engine/02_RHI/Command/CommandAllocatorPool.h/.cpp`、`CommandSignature.h/.cpp`、`Resource/Texture.h`、`Resource/ShadowBuffer.h`、`d3dx12.h` | 命令分配器池、间接命令签名、纹理包装、阴影缓冲（**后两者我未读，见 §9**）、微软 d3dx12 辅助头 | 目录实测 |

### 4.2 RHI 细节泄漏清单（逐处 `file:line`）

**(a) 上层直接写 `D3D12_RESOURCE_STATE_*`（共 42 处，全部实测）**

| 文件 | 处数 | 行号 |
| --- | --- | --- |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp` | 11 | 229, 232, 233, 315, 318, 320, 331, 332, 333, 358, 359（其中 331-333 用 `const_cast<ColorBuffer*>` 去掉 `GetIrradianceCube()/GetPrefilterCube()/GetBRDFLUT()` 的 const） |
| `Engine/04_Renderer/EnvironmentLightingManager.cpp` | 11 | 159, 160, 161, 162, 217, 246, 247, 272, 273, 317, 347 |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp` | 7 | 138, 139, 191, 195, 196, 200, 201 |
| `Engine/03_AssetSystem/Importers/Texture/DDSTextureLoader.cpp` | 4 | 839, 880, 939（`CreateCommittedResource` 的初始状态）、1189（构造 `GpuResource DestTexture(*texture, D3D12_RESOURCE_STATE_COPY_DEST)`） |
| `Engine/01_Application/Display.cpp` | 3 | 255, 256, 260 |
| `Engine/05_Scene/Model/Model.cpp` | 2 | 166（`COPY_DEST`, `FlushImmediate=true`）、169（`GENERIC_READ`） |
| `Engine/03_AssetSystem/Importers/Texture/TextureManager.cpp` | 1 | 135（`m_UsageState = D3D12_RESOURCE_STATE_GENERIC_READ`） |
| `Engine/05_Scene/Lighting/LightingSystem.cpp` | 1 | 79（在零调用的 `UpdataState` 内） |

**(b) 上层直接 `CreateCommittedResource`**
- `Engine/03_AssetSystem/Importers/Texture/DDSTextureLoader.cpp:838-839`、`:879-880`、`:938-939` —— **资产导入器**（不在 RHI 层）直接用 `d3dDevice->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, TY_IID_PPV_ARGS(&tex))`。

**(c) 上层直接使用 `D3D12_HEAP_PROPERTIES` / `D3D12_RESOURCE_DESC`**
- `DDSTextureLoader.cpp:812`（`D3D12_HEAP_PROPERTIES HeapProps;`）、`:819`（`D3D12_RESOURCE_DESC ResourceDesc;`）、`TextureManager.cpp:137`（`D3D12_RESOURCE_DESC desc = GetResource()->GetDesc();`）。

**(d) 上层直接持有/传递裸描述符句柄（`D3D12_CPU/GPU_DESCRIPTOR_HANDLE`，共 15 处）**
- `Engine/05_Scene/Model/ModelLoader.cpp` 4 处（含 `:97-104` 的 `D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures]`、`:90` 直接调 `Renderer::s_TextureHeap.Alloc`）；
- `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.h:147` `D3D12_CPU_DESCRIPTOR_HANDLE m_CPUGBuffers[4];`、`:148` `D3D12_GPU_DESCRIPTOR_HANDLE m_GBufferSRV;`；
- `Engine/03_AssetSystem/Importers/Texture/TextureManager.h` 1 处、`TextureManager.cpp:234` 1 处（`TextureRef::GetSRV()` 返回 `D3D12_CPU_DESCRIPTOR_HANDLE`）；
- `DDSTextureLoader.h:25/:34` 2 处、`DDSTextureLoader.cpp` 4 处；
- `ForwardRenderer.cpp:255-259`（`s_TextureHeap.Alloc(1)` 得到 `DescriptorHandle`，`BuildDescriptorHeaps`）、`ForwardRenderer.cpp:410-412` 与 `DeferredRenderer.cpp:564-566`（`SetDescriptorTable(..., s_TextureHeap[material.SRVTable])`）。

**(e) 上层直接构造 root signature 参数语义（`InitAsDescriptorRange`/`InitAsConstantBuffer`/`InitAsDescriptorTable`，约 40 处）**
- `DeferredRenderer.cpp` 16 处（`BuildRootSignature` `:397-421` + `BuildLightingSignature` `:587-637`）；
- `EnvironmentLightingManager.cpp` 13 处（5 个根签名：EquirectToCubemap `:59-61` 一带、Irradiance `:86-88`、Prefilter `:100-102`、BRDF `:112-113`、Skybox `:359-361`）；
- `ForwardRenderer.cpp` 6 处（`:263-284`）。
→ 即：**根签名的「语义」散落在每个渲染器/每个 IBL pass 里**，RHI 只提供 `RootParameter` 这类低层封装。

**(f) 上层直接使用 `D3D12_INPUT_ELEMENT_DESC`**
- 代码处 5 处：`Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp` 3 处、`DeferredRenderer.cpp:509-519` 1 处、`ForwardRenderer.cpp:361-364` 1 处（**MeshConvert 的 3 处我只确认了文件与计数，未逐行定位 → §9**）；
- 注释 1 处：`Engine/03_AssetSystem/Importers/glTF.cpp:206`（以及 `:465` 的类似注释）—— 旧审计计数的 6 处里包含注释，实测代码处为 5 处。

**(g) 上层直接拿 `ID3D12*` 接口**
- 自有代码（排除第三方 PIX 头）：`DDSTextureLoader.cpp` 11 处、`DDSTextureLoader.h` 4 处、`Display.cpp` 2 处、`glTF.cpp:465` 一带 1 处（注释）；
- **RHI 自身对外暴露裸指针**：`GraphicsCore.h:26` `extern ID3D12Device* g_Device;`、`:27` `extern ComPtr<IDXGIFactory6> g_DXGIFactory;`、`:34-38` `g_DescriptorAllocator[]` + `AllocateDescriptor`；`GpuResource.h:37-43` `operator->`/`GetResource()` 返回 `ID3D12Resource*`；`PipelineState.h:14-35` 的 `m_RootSignature` 是裸 `ID3D12RootSignature*`；`CommandContext.h:136` `GetCommandList()` 返回裸 `ID3D12GraphicsCommandList*`（成员其实是 `...List4`，`:189`）；`SamplerManager` 里 `g_Device->CreateSampler`。
- **应用层直接操作交换链**：`Display.cpp:134` `IDXGISwapChain1* s_SwapChain1`、`:195-201` `CreateSwapChainForHwnd`、`:220-221` `SetFullscreenState`+`Release`（**交换链不属于 RHI 层，也不属于渲染路径，落在 `01_Application`**）。

**(h) 全局 shader-visible 描述符堆被当全局变量暴露**
- `Engine/04_Renderer/Renderer/Base/BaseRenderer.h:46-47` `extern DescriptorHeap s_TextureHeap; extern DescriptorHeap s_SamplerHeap;`（定义在 `BaseRenderer.cpp:11-12`）；
- 使用者：`ModelLoader.cpp:90`（**场景/资产层直接向渲染器的全局堆分配描述符**）、`ForwardRenderer.cpp:254-260`、`DeferredRenderer.cpp:388-395`、两个 `BindMaterial`、`ModelConvert` 路径。

### 4.3 泄漏的严重度排序（我的判断）

1. **资产导入器直连 D3D12**（`DDSTextureLoader.cpp:838-939` + `TextureManager.cpp:135/137/234`）：资产层与设备/堆/状态完全耦合，换后端或加资源跟踪都要改这里。
2. **资源状态转换由上层负责**（42 处）：既违反原则 3，也是原则 5 的风险源（状态与 fence 生命周期脱节）。
3. **全局描述符堆 + 裸句柄外露**（`BaseRenderer.h:46-47`、`DeferredRenderer.h:147-148`、`ModelLoader.cpp:90`）：任何上层都能拿到堆并写入，RHI 无法保证句柄生命周期。
4. **根签名语义散落**（约 40 处）：`RootSignature` 只提供低层参数封装，没有「管线布局描述」这一层抽象。
5. **场景层直连命令上下文**（`Model.cpp:166/169` 的 `gfxContext.TransitionResource` + `GpuBuffer/UploadBuffer` 成员）：场景对象自带 GPU 资源与状态迁移。

**注意（重要）**：本条与原则 5 的「禁止为追求抽象隐藏重要 GPU 同步关系」并不冲突 —— 需要的是**把状态转换集中到受控位置并保留可见性**，不是把它藏起来。具体方向见 §8 债务 D-RHI-1/2/3。

---

## 5. 后处理与 HDR / Tonemapping / LDR 现状

### 5.1 是否存在后处理链 —— **不存在**

从场景渲染到最终 present 的**实际**步骤（含 PIX 事件名，全部实测）：

| 步骤 | 执行者 | PIX / 调试名 | 目标 | 真实/空壳 |
| --- | --- | --- | --- | --- |
| 1 | `DeferredApp::RenderScene` → `GraphicsContext::Begin(L"Scene DeferredRenderer ")` | context ID `Scene DeferredRenderer ` | —— | 真实 |
| 2 | `DeferredRenderer::Render` | `PIXBeginEvent(L"DeferredRenderer")`（`:202`） | —— | 真实 |
| 3 | GBuffer | `GBufferPass`（`:231-271`） | 4 张 GBuffer（`RendererConfig.h:8-15`）+ SceneDepth | 真实 |
| 4 | SSAO | `SSAOPass`（`:274-279`） | 无 | **空壳** |
| 5 | Shadow | `ShadowPass`（`:281-285`） | 无 | **空壳** |
| 6 | 光照合成 | `DeferredLightingPass`（`:289-334`） | `SceneColorBuffer`（`R8G8B8A8_UNORM`，`RendererConfig.h:17`） | 真实（`Draw(3,0)` + `LightingPS`） |
| 7 | 天空盒 | `SkyPass`（`:339-346`）→ `EnvironmentLightingManager::SkyboxPass`（`:325-356`） | 同一张 `SceneColorBuffer`（`Draw(3,0)`） | 真实但**有 UV 双重映射 bug**（§2.4） |
| 8 | 透明前向 | `Transparent Forward Pass`（`:349-353`） | 无 | **空壳** |
| 9 | 后处理 | `PostProcess Pass`（`:355-359`） | 无 | **空壳** |
| 10 | TAA | `TAA Pass`（`:361-365`） | 无 | **空壳** |
| 11 | Tonemap | `Tonemap Pass`（`:367-371`） | 无 | **空壳** |
| 12 | 结束 | `PIXEndEvent`（`:372`）→ `EndFrame`（`:375-386`）→ `Finish()` | —— | 真实 |
| 13 | Present | `Display::Present`（`:229-247`）→ `PreparePresentSDR`（`:251-263`），context ID `Present` | 交换链后备缓冲（`g_DisplayPlane[g_CurrentBuffer]`） | 真实，**但只是 `CopyBuffer`** |
| 14 | 上屏 | `s_SwapChain1->Present(0, 0)`（`:234`），手动 `g_CurrentBuffer = (g_CurrentBuffer + 1) % 3`（`:236`） | 屏幕 | 真实，无 vsync / 无 pacing |

**Forward 路径更短**：`ForwardRenderer::Render`（`:122-245`）→ `SceneColorBuffer` → 同一个 `Display::Present`。没有 GBuffer、没有光照（`DefaultPS.hlsl:51-57` 只采样 baseColor）。

### 5.2 HDR 中间 RT —— **不存在**

| 项 | 现状 | 证据 |
| --- | --- | --- |
| 场景颜色格式 | `R8G8B8A8_UNORM`（**既不是 `_SRGB`，也不是浮点/HDR**） | `RendererConfig.h:17`；`Display.cpp:29` `SwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM` |
| 唯一的浮点 RT | GBuffer 的 Material 通道 `R16G16B16A16_FLOAT` | `RendererConfig.h:8-15` |
| 全工程唯一的 HDR 浮点图 | IBL 的 Environment/Irradiance/Prefilter cube（`R16G16B16A16_FLOAT`）与 BRDFLUT（`R16G16_FLOAT`） | `EnvironmentLightingManager.cpp:375/377/382/384` |
| `BufferManager` 的 `HDR_COLOR_FORMAT`/`SDR_COLOR_FORMAT` 宏 | **零引用（遗留）** | `BufferManager.cpp:20-29` |
| 结论 | 光照结果被直接写进 8 位 UNORM 目标，没有「HDR 场景色 → tonemap → LDR」的两段式 | —— |

### 5.3 Tonemapping 在哪 —— **藏在 `LightingPS` 内，且是唯一的 tonemap 点**

- `Engine/Shader/02_Deferred/LightingPS.hlsl:153` `finalColor = ambient + directionalLighting + pointLighting + emission;`
- `Engine/Shader/02_Deferred/LightingPS.hlsl:155` `finalColor = ACESFilm(finalColor); // ACES校正`
- `:157` `return float4(finalColor, 1.0);` —— 结果**直接写进 `R8G8B8A8_UNORM` 的 `SceneColorBuffer`**。

由此产生 3 个真实问题：
1. **天空盒在 tonemap 之后合成**：`SkyPass`（`DeferredRenderer.cpp:337-347`）发生在 `DeferredLightingPass` 之后，天空像素只经过 `SkyboxPS.hlsl:37` 的 `gSkybox.Sample`，**没有 ACES**→ 天空与物体的色调/亮度不在同一响应曲线上。
2. **没有线性→sRGB 编码环节**：ACES 的输出按线性值直接落到非 sRGB 的 UNORM RT，`Display::PreparePresentSDR` 只做 `CopyBuffer`（`Display.cpp:257`）→ 全链缺少最终编码（这也是「画面偏暗/偏灰」这类现象的常见根因，**运行时表现我未验证**）。
3. **`emission` 参与 ACES**（`:153`），而 GBuffer 的 Emission 通道是 `R8G8B8A8_UNORM`（`RendererConfig.h:8-15`）→ 自发光亮度上限 1.0，无法做 HDR 自发光。

### 5.4 空壳清单（后处理相关）

| 空壳 | 位置 | 备注 |
| --- | --- | --- |
| SSAO Pass | `DeferredRenderer.cpp:274-279` | 只有 PIX 两行 |
| Shadow Pass | `DeferredRenderer.cpp:281-285` | 只有 PIX 两行；`ShadowCamera` 本身也是空实现（`ShadowCamera.cpp:6-11`） |
| Transparent Forward Pass | `DeferredRenderer.cpp:349-353` | 只有 PIX 两行 |
| PostProcess Pass | `DeferredRenderer.cpp:355-359` | 只有 PIX 两行 |
| TAA Pass | `DeferredRenderer.cpp:361-365` | 只有 PIX 两行；`TemporalEffects`/`TAACS.hlsl` 是死代码；`VelocityBuffer` 建了不用（`:476`） |
| Tonemap Pass | `DeferredRenderer.cpp:367-371` | 只有 PIX 两行 |
| Present Pass（后处理→屏幕） | `Display.cpp:210-215` 的 `s_PresentRS`/`PresentSDRPS` 初始化全被注释；`Display.cpp:227-228` 注释自述「原本是最终 Present Pass（后处理→屏幕）；目前不需要」 | `PresentSDRPS` 是未 Finalize 的死对象 |

**结论**：原则 4 要求「区分 HDR 阶段 / Tonemapping / LDR；每个效果明确输入纹理/输出纹理/分辨率/格式/资源状态/执行顺序；必要时 Ping-Pong」——**现状为 0 分起点**：既没有 HDR 阶段，也没有独立 tonemap pass，也没有 LDR 阶段，也没有任何 RT 契约（后处理段连「读哪张、写哪张」都没定义）。

---

## 6. Resize 调用链与 GPU 同步现状

### 6.1 resize 的完整调用链（逐段 `file:line`）与断链点

```
WM_SIZE（用户拖动窗口 / 最小化）
└─ GameCore.cpp WndProc  case WM_SIZE:                 GameCore.cpp:158-175
   Display::Resize((UINT)((UINT64)lParam & 0xFFFF), (UINT)((UINT64)lParam >> 16));
   └─ Display::Resize(width, height)                    Display.cpp:140-168
      ├─ g_CommandManager.IdleGPU();                    :143   ← 等 GPU 空闲（正确）
      ├─ g_DisplayWidth = width; g_DisplayHeight = height;  :145-146
      ├─ for 3: g_DisplayPlane[i].Destroy();            :150-151
      ├─ s_SwapChain1->ResizeBuffers(3, w, h, SwapChainFormat, 0);  :154
      ├─ for 3: GetBuffer + CreateFromSwapChain;        :156-161
      ├─ g_CurrentBuffer = 0;                           :163
      ├─ g_CommandManager.IdleGPU();                    :165
      └─ // ResizeDisplayDependentBuffers(g_NativeWidth, g_NativeHeight);   :167  ← 唯一挂点，被注释；且该函数无实现（Display.h:24 只有声明）

   ✗ 断链点 A：场景 RT 没有任何重建。SceneColorBuffer / GBuffers[4] / SceneDepthBuffer / VelocityBuffer
     只在 Initialize 时创建：DeferredRenderer::CreateDeferredBufferTargets  DeferredRenderer.cpp:447-479
     ForwardRenderer::CreateForwardBufferTargets                ForwardRenderer.cpp:311-323
     两个渲染器都有 OnResize 实现（:139 / :95）但全仓库零调用者。
   ✗ 断链点 B：相机纵横比未更新。Camera::SetAspectRatio（Camera.h:82）零调用；
     默认 aspect = 9.0f/16.0f（Camera.h:129-133）→ 窗口比例改变后画面被拉伸。
   ✗ 断链点 C：渲染路径与场景完全不知道窗口变了（无任何通知机制，OnResize 也没被调用）。
   ✗ 断链点 D：0 尺寸未过滤。最小化时 width/height 为 0 会进入 ResizeBuffers(3, 0, 0, …)。
   ✗ 断链点 E：Display 自己的 g_NativeWidth/g_NativeHeight 与 SetNativeResolution 路径也是断的
     （Display.cpp:109 的 InitializeRenderingBuffers 被注释；:112-129 的 SetDisplayResolution 整段在
      #ifdef _GAMING_DESKTOP 内，:244-246 的动态分辨率调用同样被注释）。
```

**后果（可推断，未运行时验证）**：交换链尺寸变化后，`SceneColorBuffer` 仍是旧尺寸 → `Display::PreparePresentSDR` 的 `CopyBuffer`（`Display.cpp:257`）在尺寸不匹配时会被 D3D12 调试层报错或行为未定义；`Camera` 的 aspect 未变则即便 RT 重建也会拉伸。

### 6.2 `CommandContext` 的 wait/fence 机制与实际使用

| 机制 | 实现（实测） | 实际使用 |
| --- | --- | --- |
| 提交与计时 | `ExecuteCommandList`：`ExecuteCommandLists` → `Signal(m_pFence, m_NextFenceValue)`，返回自增值（`CommandListManager.cpp:112-121`） | 每次 `Finish()` 一次 |
| 等待 | `CommandQueue::WaitForFence`：先 `IsFenceComplete`，否则 `SetEventOnCompletion` + `WaitForSingleObject(INFINITE)`（`:159-175`）；`CommandListManager::WaitForFence` 按 fence 高 8 位选队列（`:177-181`）；`IdleGPU()` = 三队列各 `WaitForIdle()`（`:98-103`） | `Finish(bool)` 默认 **false**（`CommandContext.h:124`、`.cpp:121-145`）→ 每帧路径**不等** |
| 跨队列同步 | `StallForFence` 让消费队列 `Wait(Producer.m_pFence, FenceValue)`（GPU 侧等待，`:150-157`） | 当前单队列（Graphics）为主，未见实际跨队列等待点**（未穷举）** |
| 分配器回收 | `RequestAllocator()` 用 `m_pFence->GetCompletedValue()` 找可复用 allocator（`:183-188`）；`CommandAllocatorPool` 管理 | 每次 `Begin` 借出 |
| 每帧常量缓冲 | `m_CpuLinearAllocator.Allocate(...)`（UPLOAD 堆线性分配，`CommandContext.h:490-502`） | 每帧 `SetDynamicConstantBufferView` |
| 每帧描述符 | `DynamicDescriptorHeap` + `Commit*RootDescriptorTables`（`CommandContext.h:98-99`、`:587-589`） | `SetDynamicDescriptor` |
| 首帧同步阻塞点 | `BakeEnvironmentTextures` 内 `computeContext.Finish(true)`（`EnvironmentLightingManager.cpp:164`） | 每帧被调用但只有首帧真烘 |
| 初始化期同步 | `InitContext.Finish(true)` 多处（`CommandContext.cpp:501/564/605/625`） | 初始化时（正确） |
| 关闭期同步 | `GameCore.cpp:79-86` `TerminateApplication`：**先** `g_CommandManager.IdleGPU()` **再** `game.Cleanup()` | 顺序正确 |

**缺口（原则 5 关注点）**
1. **无帧间 pacing / 无「等第 N-2 帧」**：`Present(0, 0)` 不等待（`Display.cpp:234`），也不查询 `GetCurrentBackBufferIndex`（手写 `%3`，`:236`）。当前不会崩，但一旦引入一致性帧（或帧数超过 fencing 深度能力）就会出问题。
2. **上层的资源状态转换与 fence 生命周期脱节**：状态字面量散落在上层 42 处（§4.2a），RHI 无法在「资源将被回收」时检查它是否仍在 GPU 队列里。
3. **UAV/Alias 屏障攒批可越界**（`CommandContext.cpp:442-450/457-466`，Release 下 ASSERT 被消掉）。

### 6.3 GPU 资源销毁时机 —— 「GPU 仍在用就释放」的风险**成立**

| 现象 | 证据 | 风险 |
| --- | --- | --- |
| `GpuResource::Destroy()` 立即置空 `ComPtr` 并 `++m_VersionID`，**无延迟释放队列/无 in-flight 检查** | `Engine/02_RHI/Resource/GpuResource.h:30-35` | 任何「上层先 Destroy、GPU 后读」的时序都会释放在用资源 |
| `BufferManager::CreateColorBuffer/DepthBuffer` **同名直接覆盖**（旧 `shared_ptr` 当场析构） | `BufferManager.cpp:36-48`、`:50-60` | 未来若通过重建 RT 实现 resize，会立刻释放旧 RT 而不等 GPU |
| `DeferredRenderer::DestroyDeferredBufferTargets()` = `BufferManager::DestroyAll()`（**销毁全局所有 buffer**） | `DeferredRenderer.cpp:481-484` | 两个渲染器对象不能共存；跨模块资源会被误杀 |
| `ForwardRenderer::DestroyForwardBufferTargets()` 直接 `->Destroy()`，**不判空** | `ForwardRenderer.cpp:325-329` | 二次调用或部分创建失败时崩溃 |
| `Display::Shutdown()` `Release()` 交换链后**未置空** `s_SwapChain1` | `Display.cpp:218-225` | 二次 Shutdown / Resize 会悬垂（`Resize` 里有 `ASSERT(s_SwapChain1 != nullptr)`，Release 下 ASSERT 为空） |
| 正确的对照点：`Display::Resize` 在销毁交换链缓冲**之前**调 `IdleGPU()`（`:143`）、之后又调一次（`:165`） | `Display.cpp:143/165` | 说明项目已有「先等 GPU 再销毁」的正确模式，只是没有推广 |

### 6.4 PIX 事件与 barrier 的责任方

| 关注点 | 责任方（现状） | 证据 |
| --- | --- | --- |
| PIX 事件 | **渲染器自己发**（Deferred 10 段；Forward 1 段且 `ShadowPass` 只有 Begin 没有 End、函数末尾无 End）；RHI 只提供 `PIXBeginEvent/PIXEndEvent/PIXSetMarker`（`CommandContext.h:71-73`、`.cpp:629-641`） | `DeferredRenderer.cpp:202/231/274/281/289/339/349/355/361/367/372`；`ForwardRenderer.cpp:158-162` |
| Present 阶段的 PIX | **无**（`Display.cpp` 全文 0 处 `PIXBeginEvent`；`Begin(L"Present")` 只是上下文调试名） | 实测 |
| barrier 的「语义」 | **上层负责**（42 处状态字面量，§4.2a）；RHI 只负责攒批与 flush（`CommandContext.cpp:333-338`、`:385-466`） | 实测 |
| barrier 的「正确性保护」 | **无**：`InsertUAVBarrier`/`InsertAliasBarrier` 没有 `==16` 自动 flush（`:442-450`/`:457-466`），且 Release 下 ASSERT 失效 | 实测 |
| 交换链 present 前的状态 | 上层负责（`Display.cpp:255-260` 三次 TransitionResource） | 实测 |

---

## 7. 六条目标架构原则的现状满足度评分

评分口径：0 = 完全没有；1 = 有骨架/雏形但未形成边界；2 = 边界基本成立但有明确缺口；3 = 原则已落实并有可验证证据。

| # | 原则 | 现状满足度 | 证据（HEAD） | 主要缺口 |
| --- | --- | --- | --- | --- |
| 1 | 应用层与渲染路径分离 | **1 / 3** | 已有 `RenderFrameDesc{camera, models, delatT}`（`BaseRenderer.h:66-78`）与 `IGameApp` 回调骨架（`GameCore.h:12-27`） | ① Present 与输出契约在应用层：`GameCore.cpp:72` 调 `Display::Present()`；`DeferredApp.cpp:76` 由 Demo 把渲染器 RT 挂到 `Display`；② 渲染器生命周期由 Demo 管（`DeferredApp.cpp:57/75/141`），`RendererRegistry` 死代码；③ 场景提交（Camera/Models）在 Demo；④ 帧循环把 `Update→RenderScene→Present` 写死（`GameCore.cpp:58-77`）；⑤ `RenderUI` 零调用（`GameCore.h:24`） |
| 2 | 渲染路径与公共渲染功能分离 | **1 / 3** | 只有 `MeshSorter`（`BaseRenderer.h:113-170`）、`Material`（`Material.h:23-36`）、`BufferManager`、`EnvironmentLightingManager` 四块散件 | ① Forward 与 Deferred **8 段逐字重复**（§3.2.3）；② `ShaderLibrary`/`PSOBuilder`/`MaterialFlags` 是 0 行空文件、`RenderGraph/` 空目录（§3.3）；③ `TemporalEffects`/`TAACS.hlsl` 死代码；④ `EnvironmentLightingManager` 是定义在渲染器 cpp 里的全局单例（`DeferredRenderer.cpp:38`），且 IBL 状态转换由 DeferredRenderer 负责（`:331-333`）；⑤ `PipelineStateCache` 只管索引不管 PSO 编译；⑥ `GetFeatures()` 两者取值相同且都不正确 |
| 3 | 场景、材质与底层 RHI 解耦 | **0 / 3** | —— | ① 场景层 include RHI 与渲染器层：`Model.h:5-12`；② 场景层直接调用命令上下文转状态：`Model.cpp:166/169`；③ 场景层自带 GPU 成员（`UploadBuffer m_MeshConstantsCPU`、`ByteAddressBuffer m_MeshConstantsGPU`，`Model.h:98-138`）；④ 资产导入器直接 `CreateCommittedResource`：`DDSTextureLoader.cpp:838-939`；⑤ 上层直接写 `D3D12_RESOURCE_STATE_*` **42 处**（§4.2a）；⑥ 全局 shader-visible 堆被当全局变量外露：`BaseRenderer.h:46-47`；⑦ 场景层反向依赖应用层：`CameraController.h:3`；⑧ 材质概念在 4 处定义、3 套纹理槽顺序（§3.4.2） |
| 4 | 后处理采用明确的输入输出约定 | **0 / 3** | —— | ① 不存在后处理链（PostProcess/TAA/Tonemap 三段空壳，`DeferredRenderer.cpp:355-371`）；② 无 HDR 中间 RT（`RendererConfig.h:17` 场景色是 `R8G8B8A8_UNORM`）；③ tonemap 藏在 `LightingPS.hlsl:155`，且天空在其之后合成（`DeferredRenderer.cpp:337-347`）；④ 无 linear→sRGB 编码（`Display.cpp:257` 只 CopyBuffer）；⑤ 没有任何「输入纹理/输出纹理/分辨率/格式/状态」契约；⑥ 无 Ping-Pong（`PostProcess` 段连注释都没有） |
| 5 | 资源生命周期与 GPU 执行同步 | **1 / 3** | fence 高 8 位存队列类型、`WaitForFence` 先查再等、`RequestAllocator` 用 `GetCompletedValue` 回收（`CommandListManager.cpp:14-15/112-175/183-188`）；`Display::Resize` 销毁前 `IdleGPU`（`Display.cpp:143/165`）；`TerminateApplication` 先 `IdleGPU` 再 `Cleanup`（`GameCore.cpp:79-86`）；每帧 CB 走 UPLOAD 线性分配（`CommandContext.h:490-502`） | ① `GpuResource::Destroy` 无延迟释放队列（`GpuResource.h:30-35`）；② resize 断链（§6.1 五个断点）；③ `BufferManager` 同名覆盖 + `DestroyAll` 无视归属（`BufferManager.cpp:36-48`、`DeferredRenderer.cpp:481-484`）；④ UAV/Alias 屏障可越界（`CommandContext.cpp:442-466`）；⑤ 无帧间 pacing、手写 `%3` 而非 `GetCurrentBackBufferIndex`（`Display.cpp:234-236`）；⑥ 无 in-flight 资源跟踪 |
| 6 | 为未来管线保留合理扩展能力 | **1 / 3** | `RendererFeature` 枚举与 `HasFeature`（`BaseRenderer.h:80-111`）；`RendererRegistry` + 自动注册模板（`RendererRegistry.h:29-59`）；`MeshSorter` 与 `PipelineDesc` 抽象 | ① `RendererRegistry::CreateRenderer` 零调用（§2.2#1）；② DXR 完全不生效（`RayTracingApp.cpp:36/66` 缺 const、用 DeferredRenderer、`DispatchRays` 0 命中、`ShaderCompiler.cmake` 无 `lib_*` profile，WT 仍未补）；③ 两条管线不共享功能，新管线（Neural/Gaussian）只能再复制一份；④ 无 RenderGraph/无 pass 生命周期抽象；⑤ 无场景描述与管线的解耦接口（场景层反而直接 include PipelineDesc/Material） |

---

## 8. 债务清单（按影响排序）

字段说明：**现象 / 证据 / 影响 / 最小修复方向（仅描述方向，本阶段不改代码）/ 关联原则**。

### 第一梯队：会阻断「画面正确」与「架构演进」的债务

**D-01 审计/施工基线不可冻结（工程风险，P0）**
- 现象：审计期间工作树被另一个并行开发会话持续写入；出现过文件被清空到 30 行的中间态（`DeferredRenderer.cpp` 17:37 实测 1007 B / 30 行 → 17:48:05 恢复 655 行）。
- 证据：§1.2 逐文件表；`git status` 未暂存 28 项（455+/548-）+ 索引删除 3 项（1533-）；lead 18:17:49 复测合计 38 项。
- 影响：多写入者共享同一工作树；写入未走原子替换；在截断态构建/运行会得到与源码不符的二进制，且审计结论随时过期。
- 最小修复方向：构建只在干净提交点进行；施工会话独占工作树；改动先落提交再改下一处。
- 关联原则：全部（过程约束）。

**D-02 Resize 全链断链（画面/稳定性）**
- 现象：窗口尺寸变化后，场景 RT（SceneColor/GBuffer/Depth/Velocity）不重建，相机 aspect 不更新。
- 证据：`GameCore.cpp:158-175` → `Display.cpp:140-168`（挂点 `:167` 被注释）；`ResizeDisplayDependentBuffers` 在 `Display.h:24` 只有声明；`ForwardRenderer.cpp:95` / `DeferredRenderer.cpp:139` 的 `OnResize` 零调用；`Camera.h:82 SetAspectRatio` 零调用。
- 影响：CopyBuffer 尺寸不匹配（D3D12 调试层报错/画面异常）；比例拉伸；后续任何「输出分辨率无关」的后处理都无法建立。
- 最小修复方向：建立「窗口尺寸变化」单一事件 → 依次通知 Camera（aspect）、渲染路径（OnResize/重建 RT）、Display（交换链）；并过滤 0 尺寸。
- 关联原则：1、5（并涉及 4 的分辨率契约）。

**D-03 纹理槽三套顺序 + 默认槽错位（画面正确性，P0）**
- 现象：材质贴图被绑定到错误槽位；缺省贴图语义错误。
- 证据：`glTF.h:138`、`ConstantBuffers.h:18-26`、`DefaultPS.hlsl:44-48` 三套顺序；`ModelConvert.cpp:343/345/347/349/351`（5 处只对 1 处）；`ModelLoader.cpp:97-104`（5 槽只对 1 槽）；后果推演见 §2.3#1/#2（法线、AO、metallic/roughness 全错）。
- 影响：PBR 材质表现系统性错误；`DeferredCommon.hlsli` 的「正确 PBR 数学」被错误输入抵消。
- 最小修复方向：定义**唯一的槽位枚举**（放在被各方共同引用的头里），让 glTF 导入、C++ 常量、HLSL 全部引用它（或加 `static_assert` 校验）；默认贴图按语义逐槽校正。
- 关联原则：3（并直接影响画面验收）。

**D-04 后处理链 / HDR / Tonemap / sRGB 编码缺失（画面正确性 + 架构）**
- 现象：光照结果直接进 8 位 UNORM；ACES 只在 `LightingPS` 内；天空在 tonemap 之后画；present 只做拷贝。
- 证据：`DeferredRenderer.cpp:355-371` 三段空壳；`RendererConfig.h:17`；`LightingPS.hlsl:153-157`；`DeferredRenderer.cpp:337-347`；`Display.cpp:251-263`。
- 影响：色调/亮度不统一、高光截断、无法接任何后处理；原则 4 从 0 起步。
- 最小修复方向：先建立「HDR 场景色 → Tonemap → LDR → Present」两段式与显式 RT 契约（输入/输出/格式/分辨率/状态），再谈具体效果。
- 关联原则：4（并拉动 1 的 Present 归属）。

**D-05 两条渲染路径 8 段逐字重复，`BaseRenderer` 不是可复用基类**
- 现象：Initialize/Shutdown/OnResize/BuildDescriptorHeaps/BuildRootSignature/GetPSOIndex(input layout)/BindRenderState/BindMaterial 各写一份。
- 证据：§3.2.3 的逐段行号对照；`BaseRenderer.cpp` 全文只有 `ModelSort` 与 `MeshSorter` 是共享实现；真正差异仅 4 项。
- 影响：任何公共功能（材质、阴影、后处理、调试视图）都要改两遍；新管线必须再复制一遍。
- 最小修复方向：先抽「公共渲染功能」的**最小集合**（材质绑定/根签名布局/PSO 描述与缓存/描述符堆管理），保留两条路径各自专用实现（不强行统一算法）。
- 关联原则：2。

### 第二梯队：分层与泄漏相关

**D-06 场景层双向依赖（RHI + 渲染器 + 应用层）**
- 现象：`Model.h:5-12` 同时 include RHI、资产层、`04_Renderer/Pipeline/PipelineDesc.h`、`04_Renderer/Material/Material.h`；`Model.cpp:166/169` 直接转资源状态；`CameraController.h:3` include `01_Application/GameCore.h`。
- 影响：场景无法脱离 D3D12 与具体渲染器复用；换后端/加管线的成本极高。
- 最小修复方向：把「节点/网格/材质描述」与「GPU 资源」分离（资源描述用 POD，GPU 侧句柄由渲染路径持有的表提供）；去掉 `CameraController` 对应用层的依赖。
- 关联原则：3。

**D-07 资产导入器直连 D3D12**
- 证据：`DDSTextureLoader.cpp:812/819/838-839/879-880/938-939/1189`、`TextureManager.cpp:135/137/234`、`DDSTextureLoader.h:19-34`。
- 影响：资产层与设备/堆/状态耦合；资源跟踪无法集中。
- 最小修复方向：把「创建纹理资源」下沉为 RHI 的一个受控接口（上传堆 + 状态由 RHI 负责），资产层只产出像素数据与格式描述。
- 关联原则：3。

**D-08 资源状态转换散落上层 42 处**
- 证据：§4.2a 逐处行号。
- 影响：状态与 fence 生命周期脱节；新增 pass 时极易漏转/错转。
- 最小修复方向：把状态转换收敛到「pass 声明其资源读写意图、由引擎统一插入转换」的机制（可先做最小版：每帧的 RT 初末状态集中到一处）；**保留可见性**，不要隐藏。
- 关联原则：3、5。

**D-09 资源销毁无延迟释放，且池化语义危险**
- 证据：`GpuResource.h:30-35`；`BufferManager.cpp:36-48`（同名覆盖）、`:62-66`（DestroyAll）；`DeferredRenderer.cpp:481-484`（DestroyAll 无视归属）；`ForwardRenderer.cpp:325-329`（不判空）；`Display.cpp:218-225`（Release 后不置空）。
- 影响：释放在用资源、跨模块误杀、悬垂。
- 最小修复方向：引入「按 fence 值延迟释放/回收」的队列，并要求销毁前经过它；`DestroyAll` 改为按归属销毁。
- 关联原则：5。

**D-10 全局描述符堆与裸句柄外露 + 采样器去重失效 + 分配器无锁**
- 证据：`BaseRenderer.h:46-47`（extern 全局堆）、`BaseRenderer.cpp:11-12`；`ModelLoader.cpp:90`；`DeferredRenderer.h:147-148`；`SamplerManager.cpp:20-32`（从不 insert）；`DescriptorHeap.cpp:40-62`（递增路径无锁）、`:14-18`（DestroyAll 不回退 allocator 状态）。
- 影响：描述符生命周期失控、描述符泄漏、并发不安全。
- 最小修复方向：堆由渲染路径/RHI 内部持有，对外只给「句柄申请」接口；补采样器缓存写回；加锁或明确单线程约束。
- 关联原则：3、5。

**D-11 UAV/Alias 屏障攒批可越界**
- 证据：`CommandContext.cpp:442-450`、`:457-466`（无 `==16` 自动 flush），对比 `:408-409`/`:436-437` 有守卫；Release 下 `ASSERT` 被消掉（`Utility.h:139`）。
- 影响：Release 下缓冲区越界写；UB。
- 最小修复方向：给这两条路径补上与 Transition 相同的「满 16 即 flush」判定（或改为容器/断言改为运行时检查）。
- 关联原则：5。

### 第三梯队：确定性画面 bug（旧审计 T1 系列，HEAD 全部成立）

| 编号 | 现象 | 证据 | 影响 | 最小修复方向 | 关联原则 |
| --- | --- | --- | --- | --- | --- |
| D-12 | 深度预通道 VB 偏移不递增 | `ModelConvert.cpp:133`（`:139` 仅 Mesh 末尾累加） | 同 Mesh ≥2 draw 时深度 VB 互相覆盖 → 阴影/深度预通道错误 | 与 `curVertOffset` 同构地累加 `curDepthVBOffset` | 3 |
| D-13 | 切线未还原 | `StaticMeshVS.hlsl:49-50` 算了却用 `:56` 的未还原值 | 法线贴图 TBN 错误 | 用已还原的 `Tangent` | 3 |
| D-14 | 光照 pass 硬编码 1920×1080 | `LightingPS.hlsl:85-86` | 非该分辨率下世界坐标重建错误 | 改为常量缓冲传入的 RT 尺寸 | 4 |
| D-15 | 天空盒 UV 双重映射 | `SkyboxVS.hlsl:19-21` 与 `SkyboxPS.hlsl:22-26` | 天空方向错误/采样出界 | 统一「VS 出 NDC、PS 直接用」的约定 | 3、4 |
| D-16 | IBL LOD 上限与实际 mip 不符 | `LightingPS.hlsl:143`（8.0） vs `EnvironmentLightingManager.cpp:382`（5 mip） | 粗糙度映射错误 | 用 `GetPrefilterMipCount()` 传入 | 2 |
| D-17 | `GBufferPS` B 通道恒 0.5、无 alpha 裁剪 | `GBufferPS.hlsl:121`、`:88-124` | F0 恒 0.04；AlphaTest 无效（`alphaCutoff` 未接） | 传入 `specularFactor`/`alphaRef` 并加 clip | 3 |
| D-18 | 默认纹理槽错位 | `ModelLoader.cpp:97-104` | 见 D-03 | 同 D-03 | 3 |
| D-19 | 硬编码绝对路径 18 处 | §3.4.3 全表 | 换机器/CI 失败且表现为黑屏 | 已由 WT 的 `AssetRoot` 覆盖（待提交） | 3 |
| D-20 | `RELEASE` 宏从未定义 + 断言宏无分号 | `Utility.h:128-144`（HEAD）；`Engine/CMakeLists.txt`（WT 补定义与注释） | Release 构建行为与 Debug 不一致；断言形同虚设或语法错误 | 已由 WT 覆盖（待提交与验证） | 5 |
| D-21 | Forward 使用入库旧字节码头、无 ShaderLibrary | `ForwardRenderer.cpp:16-17`（HEAD）；`Engine/Shader/01_Default/DefaultVS.h`(876)/`DefaultPS.h`(649)；`ShaderLibrary.*`=0 行 | 改 HLSL 不生效 | HEAD：WT 已改为生成头（`compile_shader` + ShaderCache 优先）；仍需真正实现或删除 `ShaderLibrary` | 2 |

### 第四梯队：工程卫生与可诊断性

| 编号 | 现象 | 证据 | 影响 | 最小修复方向 | 关联原则 |
| --- | --- | --- | --- | --- | --- |
| D-22 | `RendererRegistry` / `RenderUI` / `TemporalEffects` / `ShadowCamera` / 空文件与空目录 | §2.2 #1/#3/#7/#9/#10 | 死代码掩盖真实结构；读者难辨「规划」与「已实现」 | 逐项标注「实现 / 删除 / 暂缓」，不要保留无声明的空壳 | 2、6 |
| D-23 | DXR 完全不生效 | `RayTracingApp.cpp:36/66`（缺 const）、`:57`（用 DeferredRenderer）、`DispatchRays` 0 命中、`ShaderCompiler.cmake` 无 `lib_*` | 第三条管线实际不存在 | 先修 const + 导出 `g_DeviceDXR`/`g_bRaytracingSupport`，再扩 `lib_6_4` profile，最后 BLAS/TLAS/RTPSO | 6 |
| D-24 | `GameCore` 承担 Present 与输出契约 | `GameCore.cpp:72`；`DeferredApp.cpp:76` | 应用框架与渲染路径耦合 | 把 Present 与输出目标绑定移交渲染路径（配合 D-04） | 1 |
| D-25 | `LightingSystem` 灯光常量硬编码、`UpdataState` 零调用且用 ComputeContext 给图形用途转状态 | `LightingSystem.cpp:33-74`（写死方向光/点光）、`:76-80` | 场景灯光不可配置；SRV 更新路径未生效 | 加 setter 与「每帧上传一次」的明确路径 | 3、6 |
| D-26 | IBL 管理器单例化 + 不缓存 + 首帧同步阻塞 + 状态转换职责倒置 | `DeferredRenderer.cpp:38/154/331-333/344`；`EnvironmentLightingManager.cpp:122-126/150/164` | 无法复用/无法启动期预热/启动卡顿 | 移出渲染器 cpp、加落盘缓存（实现 `SaveDDS` 或换缓存格式）、状态转换归 IBL 自己 | 2、5 |
| D-27 | `MeshSorter` 在 Deferred 侧跨帧不 Reset；`GetSortkeys/GetSortObjects` 按值返回整表 | `DeferredRenderer.cpp:375-386`（无 Reset） vs `ForwardRenderer.cpp:251`；`BaseRenderer.h:113-170` | 排序残留导致错绘风险；每帧两次 vector 拷贝 | Deferred 补 `Reset()`；改为返回引用/视图 | 2、5 |
| D-28 | 模型加载失败只打日志并 `return nullptr` | `ModelLoader.cpp:211-215`（WT 已增强信息，但仍只 return） | 黑屏难以定位 | 失败向上传播到应用层的可诊断错误通道 | 1 |
| D-29 | `Assets/Model` 被 `.gitignore:386` 忽略 | §2.4 | clean clone 缺模型 → 无法验证画面 | 明确资产获取方式（LFS / 下载脚本 / 文档），使 clean clone 可复现 | 过程 |
| D-30 | `AssetRoot::LooksLikeAssets` 死代码；`GetInteger` 用 `std::stoi`（空串抛异常） | `AssetRoot.cpp:78-82`；`CommandLineArg.cpp:78-84` | 小的维护风险 | 删除死代码；`GetInteger` 做空串保护 | 3 |

---

## 9. 第一阶段未确认项（每条写明「需要读哪个文件 / 做什么」才能确认）

| # | 未确认项 | 为什么重要 | 确认方式 |
| --- | --- | --- | --- |
| U-01 | `MeshConvert.cpp` 里 3 处 `D3D12_INPUT_ELEMENT_DESC` 的具体行号与内容 | 顶点布局是 D-13/D-17 的共同输入；旧审计断言「三方布局一致」我未逐行核对 | 读 `Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp`（含 `:308-327` 一带）并与 `DeferredRenderer.cpp:509-519`、`StaticMeshVS.hlsl:23-29` 对齐 |
| U-02 | 顶点布局三方是否真的一致（旧审计结论） | 不一致会导致属性错位（比 D-13 更严重） | 同上，三处并排比对（POSITION/NORMAL/TANGENT/UV 的格式与语义名） |
| U-03 | `GBufferPS.hlsl:1-88`（`VSOutput`、常量、`GetNormalWS`、`EncodeOctNormal`） | 决定法线编码/解码正确性与 `GBufferSlot::GBuffer_Normal` 注释是否兑现 | 读 `Engine/Shader/02_Deferred/GBufferPS.hlsl` 全文（130 行左右） |
| U-04 | `DefaultVS.hlsl:44-71` 主函数体（`worldPos`、`TEXCOORD` 语义） | Forward 路径的输入语义（`:30` 输入用 `TEXCOORD`、`:41` 输出用 `TEXCOORD0`）可能影响 IA 匹配 | 读 `Engine/Shader/01_Default/DefaultVS.hlsl` |
| U-05 | `SkyboxPSO` 的深度状态、`SkyboxPS` 的 `gInvProj/gInvView` 绑定来源 | 决定 SkyPass 是「画满屏」还是「按深度遮挡」，以及 §5.3 的色调问题范围 | 读 `Engine/04_Renderer/EnvironmentLightingManager.cpp:30-120`（`CreateSkyboxPipeline`）与 `Engine/Shader/06_Environment/EnvironmentCommon.hlsli` |
| U-06 | 运行期画面表现（尺寸不匹配的调试层报错、色调偏暗、天空） | 本报告所有「画面影响」都是源码推断；本阶段禁止运行 | 在干净提交点构建运行并抓 D3D12 调试层输出（需 lead 批准，且须先固定提交点） |
| U-07 | 每帧 UPLOAD 线性分配器的页回收与「GPU 仍读」保护 | 决定是否存在「常量缓冲被覆盖」的隐性竞态 | 读 `Engine/02_RHI/Memory/LinearAllocator.cpp` 全文 + `CommandContext.cpp` 的 `Begin/Reset/Finish` 全段 + `CommandAllocatorPool.cpp` |
| U-08 | 命令录制的线程模型（`ContextManager` 借出策略、`m_ContextPool` 复用） | 决定 `DescriptorAllocator` 无锁（D-10）的真实风险等级 | 读 `Engine/02_RHI/Command/CommandContext.h:195-330` 与 `ContextManager` 定义处（`GraphicsCore.h:29` 指向的实现） |
| U-09 | `Assets/` 下是否还有其他预编译着色器/字节码资产 | 决定 D-21 是否只涉及 `01_Default` 两个头 | `glob Assets/**/*.{cso,dxil,bin,h}`（我只确认了 `git ls-files Assets` 的 3 个文件） |
| U-10 | `TextureConvert.cpp` 的 BC7/缓存键问题（旧审计 T2 项） | 影响显存占用与重烘焙行为 | 读 `Engine/03_AssetSystem/Importers/Texture/TextureConvert.cpp`（尤其 `:223` 与 `:9-30`） |
| U-11 | glTF 主链 `glTF.cpp:622-755` 的真实解析范围（`JOINTS/WEIGHTS`、`COLOR_0`、`normalTexture.scale`） | 决定「蒙皮缺失」「自发光/法线缩放」是否成立 | 读 `Engine/03_AssetSystem/Importers/glTF.cpp` 全文（同步核对 `:301-305`、`:332-365`） |
| U-12 | `LoadHDR` 全段（`EnvironmentLightingManager.cpp:170-215`）与 WT 的 3/1 改动内容 | 决定 IBL 首帧加载路径与 `m_HDRLoaded` 语义 | 读该函数 + `git diff -- Engine/04_Renderer/EnvironmentLightingManager.cpp` |
| U-13 | `Engine/CMakeLists.txt` 的 `RELEASE` 定义后半段，以及是否接入 ImGui | 决定 D-20 是否已闭环、以及 ImGui 接入（原则 1 的调试视图）现状 | 读 `Engine/CMakeLists.txt` 全文（WT 221 行）、根 `CMakeLists.txt`、`vcpkg.json` |
| U-14 | `Engine/01_Application/GameInput.cpp`（WT +83/-26）与 `Display.cpp` 的其它 2 行改动 | 输入系统改动可能影响应用层边界判断 | 读 `GameInput.cpp` 全文 + `git diff -- Engine/01_Application/Display.cpp`（后者已确认只是 2 行日志） |
| U-15 | `Engine/02_RHI/Resource/Texture.h/.cpp`、`ShadowBuffer.h/.cpp`、`CommandSignature`、`CommandAllocatorPool`、`DynamicDescriptorHeap` 的内部实现 | §4.1 的职责边界表对这几项只给了签名级描述 | 逐个读上述文件 |
| U-16 | 构建是否仍全绿、3 个 exe 是否产出（旧审计 83/83） | 本报告所有结论都建立在「能编译」的前提上 | 在干净提交点执行一次全量构建（本阶段禁止） |
| U-17 | 「本次审计期间并行会话的改动清单」是否就是 WT 全部差异 | 我按快照记录的 WT 差异可能仍不完整 | 让 lead 在并行会话暂停后重新 `git status/diff` 并与 §1.2 表对齐 |
| U-18 | `Engine/20_cmake` 其余脚本与 `scripts/*.bat` 的行为 | 影响「构建可复现性」的判断 | 读 `Engine/20_cmake/*.cmake`、`scripts/build.bat`、`scripts/run.bat` |

---

## 附录 A：本报告的取证方法与可复现命令

**命令（全部只读）**
```
git rev-parse HEAD; git log --oneline -5
git status --porcelain=v1; git diff --stat; git diff --cached --stat; git diff --numstat
git diff -- <file>; git show HEAD:<file> | wc -l; git show HEAD:<file> | sed -n "A,Bp"
git grep -n "D:/CS-Self-Study" HEAD -- Demos Engine
grep -rn "D3D12_RESOURCE_STATE_|CreateCommittedResource|InitAs|D3D12_CPU_DESCRIPTOR_HANDLE" Engine ...
grep -rIl "TAACS|ShaderLibrary|OnResize|ResizeDisplayDependentBuffers|Asset::Resolve" Engine Demos
for f in ...; do echo "[$(wc -l < $f)] $f"; done      # 空文件/空目录核查
```

**口径与限制**
1. 所有 `file:line` 都来自 HEAD 或（对未被并发改动的文件）直接读取的工作树；凡以 HEAD 为准之处均在正文注明。
2. 「画面现象」类结论一律是**源码推断**，本阶段未运行、未构建。
3. 重复度、严重度排序、崩溃后果属于**判断**而非实测，已在正文标注「推断」。
4. 详细过程（读了哪些文件、各多少行、哪些结论是实测/推断、剩余未读清单、踩到的坑）见调查日志：`docs/AIWorkLogs/audit-tooiyume_调查日志.md`。

**审计边界声明**：本次审计只读，未修改 `Engine/`、`Demos/`、`Assets/`、`Shader/` 下任何文件；未执行 `git checkout/restore/stash`；未运行构建。输出文件仅本报告与同名调查日志。
