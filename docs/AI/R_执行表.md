# TooiyumeRender 升级执行表（交接文档）

> 用途：**跨会话复用**。新会话直接读本文件即可恢复全部上下文，无需重读长对话。
> 最后更新：2026-10-28 ｜ 当前进度：**R0 ✅ R1 ✅ R2 ✅ R3 ✅ R4 ✅ R5 ✅ VS 环境验证 ✅ R6（D3 光线效果）✅ R7（D4 接材质）✅ → 待定（DXR IBL/点光源 / Soft Shadow / alpha test / 多模型）**
> 📘 **想从零读懂整条光追管线**：先读 `docs/TooiyumeRender_光追管线教程.md`（14 章 / 约 2490 行，面向初学者，`路径:行号` 全部相对仓库根且已核对）。本文件偏「施工档案 + 交接清单」，教程偏「因果解释 + 动手练习」，两者互补。

---

## 0. 一句话状态

项目是 DX12 渲染器（C++20 / HLSL / CMake / vcpkg）。R0 建立了可重复构建与资产基线，R1 修复了确定性渲染错误。**Deferred 路径现已正确渲染模型**（修复前模型完全不可见）。R2 已修复 Resize/最小化/相机宽高比/资源重建/backbuffer 索引 7 个缺陷并完成长宽比遍历验证。R3 已建成统一入口 `RendererDemoApp`：渲染器注册表可运行时切换 Forward/Deferred、Dear ImGui 六面板、`RenderParams` 成为参数唯一真源，并顺带修掉 Forward 全黑与一批 RHI 生命周期缺陷。**R4 已打通显示编码链**：Exposure→ACES→sRGB 成为 Lighting 与 Skybox 共用的唯一实现（修复前 Skybox 完全绕过曝光，天空 52% 像素硬截断）；Prefilter mip 上界改取真实值（4，原写死 8.0）；`m_HDRLoaded` 死标志语义修正；glTF MASK 材质接通 AlphaTest。模型区截断 23.0%→0.1%，天空 52.0%→0.0%。**R5 已把 DXR 接入统一入口**：设备能力查询与 App 请求解耦、`RequiresRaytracingSupport()` 补齐 `const override`、Shader 链支持 `lib_6_4`、BLAS/TLAS/RTPSO/SBT 全通、`DispatchRays` 输出法线色诊断画面并经统一 Present 上屏、`RayTracingRenderer` 可在 UI 切换且切回光栅无回归。**VS 环境验证已完成**：VS 2022 生成器树 Debug+Release 双配置构建通过、MSBuild 直构 `.sln` exit 0、DXR/光栅画面与 Ninja 版逐字节相同、Debug Layer（含 GPU-Based Validation）零 ERROR；过程中修掉 `DeployRuntimeDlls.cmake` 多配置选错 DLL 变体、MSBuild 被重复 proxy 环境变量打断两个真实缺陷。**R6 / D3 已完成**：`RayTracing.hlsl` 从「法线色诊断」升级为「硬阴影射线 + 单次反射」——独立 `ShadowPayload`（8 B）+ SBT miss 表第二条 `ShadowMiss`、`SKIP_CLOSEST_HIT_SHADER` 避免载荷类型错配、递归闸门 `Depth>=1` 早退（越界会移除设备）、唯一偏差为沿法线的 `RTShadowNormalBias`（0.002 m）、Schlick Fresnel 反射混合、4 条调试通道；`RenderParams` 新增 6 个 RT 字段并接到独立「Ray Tracing (R6)」面板（开关/偏差/通道/发射数统计）。`-R6Test` 12 步单变量 A/B 证明：阴影开关改像素 1.61%、反射强度 0 与关闭像素级等价、来回切换后精确复原、Debug/Release 11 张渲染图 md5 全同。**并修掉一个致命的构建系统缺陷**：`msvc_deps_prefix` 在 ACP≠控制台代码页时被双重编码 → `ninja -t deps` 全 `#deps 0` → 改任何 `.h` 都不重编（静默、编译全绿但运行时布局错位）。**R7 / D4 已完成**：DXR 的 `ClosestHit` 从「中性灰 albedo + Lambert」升级为「真实 BaseColor 贴图 × 材质因子 + GGX/Cook-Torrance」——根签名扩 6 参数（t3 无界 SRV 表 + s0 静态采样器）、输出 UAV 与贴图 SRV 合并进同一张 `s_TextureHeap`、每实例数据 32 B → 96 B（UV 偏移/顶点标志/材质索引/SRV 表偏移/5 个材质标量）、`LoadVertexUv` 手拆半字 + `f16tof32`、`NonUniformResourceIndex` 动态索引。**两个正交开关 `EnableMaterialTextures` / `EnableGGX` 做出 2×2 可自我证伪矩阵**：全关（A5a）逐字节等于 R6 基线 `a43b6059…`，四个 md5 互不相同证明两变量可分离归因；**A7 对照通过**（DXR BaseColor vs Deferred BaseColor：`mean|diff| 2.9` / `corr 0.993` / `identical≤2 占 81.9%`，修复前是 67.2 / 0.52）；**A6 光栅零回归**（`16c8aeec…` 与 R5/R6ctl 完全相同）；Debug/Release 9 张渲染图 md5 全同。本轮最隐蔽的坑：`ByteAddressBuffer::Load<float2>` 读 4 字节 `R16G16_FLOAT` UV → 越界拼入下一属性、uv 变 denormal ≈(0,0)、整张贴图退化成纯色且**不报任何错**。下一步：DXR IBL/点光源（U1 观感偏暗的主因）、Soft Shadow、alpha test、多模型验证。

---

## 1. 关键路径

| 项 | 路径 |
|---|---|
| 项目根 | `D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender` |
| 引擎 | `Engine/`（00_Core / 01_Application / 02_RHI / 03_AssetSystem / 04_Renderer / 05_Scene / Shader / 20_cmake） |
| Demo | `Demos/`（ForwardApp / DeferredApp / RayTracingApp / **RendererDemoApp**） |
| 资产 | `Assets/Model`（7 个模型目录）、`Assets/Skybox` |
| 文档 | `docs/AI/` |
| 构建脚本 | `scripts/build.bat`（Debug/Release）、`scripts/run.bat` |
| 构建产物 | `out/build/{Debug,Release}/Demos/*/*.exe` |
| RenderDoc | `D:\CS-Self-Study\Computer_Graphics\RenderDoc` |

---

## 2. R 阶段执行表

| 阶段 | 优先级 | 目标 | 状态 |
|---|---|---|---|
| **R0** 基线与可复现性 | P0 | 资产、路径、CMake、Shader 编译链、构建脚本 | ✅ **完成** |
| **R1** 核心渲染正确性 | P0 | 材质槽、默认贴图、深度 VB、TBN、分辨率、sorter、CB 契约 | ✅ **完成** |
| **R2** 生命周期与窗口稳定性 | P0 | Resize、最小化、相机宽高比、资源重建、backbuffer 索引 | ✅ **完成** |
| **R3** 统一入口与交互工作台 | P1 | RendererDemoApp、RendererRegistry 接通、ImGui、RenderParams | ✅ **完成** |
| **R4** 光栅 PBR 闭环 | P1 | 曝光/ACES/sRGB 统一链、Prefilter mip 上界、IBL 状态机、AlphaTest | ✅ **完成** |
| R5 DXR 最小闭环 | P1 | D0 设备能力 → D1 AS 构建 → D2 DispatchRays | ✅ **完成**（含 VS 环境验证，见 §6d） |
| R6 / D3 光追算法 | P2 | 硬阴影射线、单次反射、偏差策略、性能统计、开关 | ✅ **完成**（见 §6e） |
| R7 / D4 光追接材质 | P2 | BaseColor 贴图 + GGX/Cook-Torrance、2×2 正交开关、调试通道对照 | ✅ **完成**（见 §6f） |
| R6 扩展与加固 | P2 | Shadow Map、透明、SSAO、TAA、热重载、文档清理 | ⬜ 未开始 |
| P1 光追 IBL 与多光源 | P2 | IBL 预过滤接进 DXR、点光源、alpha test（R7 遗留 L1-L3） | ⬜ 未开始 |

**硬性要求**：每个阶段必须通过「干净构建 + 实际运行 + 可观察证据」三项验收后才能进入下一阶段。每次施工完成后在 `docs/AI` 产出学习文档。

---

## 3. R0 已完成内容（✅）

| 项 | 内容 |
|---|---|
| 资产 | 从 GitHub 补齐 `Saber` / `saberCaster` / `toneriko` 三个模型目录 |
| 资源根 | 新增 `Engine/00_Core/Utility/AssetRoot.{h,cpp}`，五级回退解析：命令行 `-AssetsRoot` → `TOOIYUME_ASSETS_ROOT` → exe 上溯 → 工作目录上溯 → 编译期兜底 |
| 硬编码 | 移除 18 处 `D:/CS-Self-Study/...`；Demo 全局路径改为惰性函数（避免静态初始化顺序问题） |
| CMake | `CMakePresets.json` 改 `$env{VCPKG_ROOT}`；删除非法 `CMakeSettings.json`；`vcpkg.json` 补 `builtin-baseline` |
| Shader | `01_Default` 纳入编译；删除仓库内旧字节码头；Forward 改用生成头 |
| 脚本 | `scripts/build.bat`（自动探测 VS/cmake/ninja/vcpkg）、`run.bat` |
| 其他 | `RELEASE` 宏补全；`forceRebuild` 改 `-RebuildModels` 开关；日志落盘 + `LOG_INFO`/`LOG_TRACE` 分级；资产缺失分级诊断 |

**文档**：`R0_构建与运行基线.md`、`R0_变更清单与复现.md`

---

## 4. R1 已完成内容（✅）

| ID | 修复 | 要点 |
|---|---|---|
| R1.1 | 统一材质纹理槽语义 | 三处枚举统一到 glTF/HLSL 顺序；BaseColor 修正为 sRGB |
| R1.2 | 修正默认纹理表 | Normal→平面法线、AO→白、Emissive→黑、MR→白 |
| R1.3 | 深度 VB per-draw 偏移 | 补 `curDepthVertOffset` 累加 |
| R1.4 | Tangent 解码 | 用已还原的 UNORM 值构造 TBN；补手性符号还原 |
| R1.5 | 移除 1920×1080 硬编码 | 扩展 `CameraData` 传 `RenderTargetSize`/`InvRenderTargetSize` |
| R1.6 | sorter 生命周期 | `Update` 开头 `Reset` + **每帧重建列表**（变换仍受 IsDirty 门控） |
| R1.7 | CB 契约 | `GlobalConstants` 全 16 字节对齐 + `static_assert` |
| 额外 | Shader 生成头依赖 | 新增 `shader_attach_header_dependency`（CMake `OBJECT_DEPENDS`） |
| 额外 | WASD 输入失效 | R0 引入的回归：`KbmZeroInputs()` 每帧擦除键位；改为三路取或 + WndProc 转发 |

**文档**：`R1_核心渲染正确性.md`、`R1_变更清单与验证.md`
**证据**：`R1_evidence/{final_r1, ch_basecolor, ch_metallic}.png`（GPU 回读）

---

## 5. R2 已完成（✅）

主题：**Resize 与 GPU 资源生命周期**。7 个缺陷已全部修复，详见 `R2_生命周期与窗口稳定性.md`。

### 已核实的 7 个缺陷（✅ 均已修复）

| # | 缺陷 | 证据位置 |
|---|---|---|
| 1 | `OnResize` **零调用**，`ResizeDisplayDependentBuffers` 被注释 | `Display.cpp:167` |
| 2 | `Display::SceneColorBuffer` 不跟随重建，仅 Startup 赋值 3 处 | `Display.cpp` + 3 个 Demo |
| 3 | `WM_SIZE` 不过滤 0×0；0 尺寸时 `CreateForwardBufferTargets` return 而 `Destroy...` 不判空 | `GameCore.cpp:164` |
| 4 | `SetAspectRatio` **零调用**，投影恒为 9:16 | `Camera.h:82` |
| 5 | backbuffer 手工 `+1 % 3`，应用 `GetCurrentBackBufferIndex()` | `Display.cpp:237` |
| 6 | `GpuResource::Destroy` **不重置描述符句柄** → 二次 Create 沿用旧描述符 | `GpuResource.h:30-35` |
| 7 | Present 状态转换顺序需复核（与 #5 叠加可能渲染到正在显示的 buffer） | `Display.cpp:249-262` |

### 实施顺序建议

1. `IGameApp` 加 `OnResize` 虚函数 → `Display::Resize` 末尾转发
2. Renderer `OnResize` 重建 RT 并重绑 `Display::SceneColorBuffer`
3. `WM_SIZE` 过滤 0 尺寸
4. 相机 aspect 跟随
5. 换 `GetCurrentBackBufferIndex`
6. `Destroy` 重置描述符句柄（**全 RHI 行为变更，需确认无别处依赖旧语义**）

### 验收（✅ 全部通过）

| 验收项 | 结果 |
|---|---|
| 不同长宽比窗口连续拖动：不崩、不拉伸、无尺寸不匹配 | ✅ `-ResizeTest` 遍历 6 种长宽比（含 4:1 与竖屏），截帧均为原始分辨率、内容无拉伸 |
| 最小化/还原/重复调分辨率可恢复 | ✅ `WM_SIZE` 过滤 0×0，最小化后恢复正常 |
| 连续 100 次 Resize：描述符不持续增长、无旧画面残留 | ✅ `-ResizeStress` 无 `DXGI_ERROR`、无断言 |
| Debug Layer 无资源状态或 RTV 维度错误 | ✅ 全程无相关告警 |

---

## 6. R3 已完成（✅）

主题：**统一入口与交互工作台**。四个交付物：`RendererDemoApp`（唯一入口）、`RendererRegistry` 真创建 + 运行时切换、Dear ImGui 1.91.9b（Win32/DX12 后端）、`RenderParams` 参数唯一真源。详见 `R3/R3_统一入口与交互工作台.md`。

### 6.1 计划内交付

| ID | 内容 | 落点 |
|---|---|---|
| R3.1 | 新增 `Demos/RendererDemoApp`（1102 行）：Registry 取实例、`unique_ptr<BaseRenderer>` 持有、六面板、`-R3Test` 自测 | `Demos/RendererDemoApp/RendererDemoApp.cpp` |
| R3.2 | `RendererRegistry` 接通：`Instance()` 内一次性**显式注册** Deferred/Forward；补 `GetRegisteredNames()` / `HasRenderer()` | `Engine/04_Renderer/Renderer/Base/RendererRegistry.cpp:24-44` |
| R3.3 | `ForwardRenderer` 修名（去尾随空格）、`GetFeatures()` 只返 `Forward`、`OnResize` 补视口、`EndFrame` 补 `ClearDirty` | `ForwardRenderer.cpp:34 / :59 / :106 / :342` |
| R3.4 | Dear ImGui：`ThirdParty/imgui`（1.91.9b）+ `Engine/04_Renderer/ImGuiLayer.{h,cpp}`（自建 64 槽 shader-visible SRV 堆、Alloc/Free 回环） | `ImGuiLayer.cpp:88 / :180` |
| R3.5 | `RenderParams` + `LightData` 扩到 96 字节 + `LightingPS.hlsl` cbuffer 扩字段（Exposure / EnableTonemap / EnableSun|PointLight / DebugView） | `RenderParams.h`、`LightingSystem.cpp:111`、`LightingPS.hlsl:41/:47` |
| R3.6 | `GameCore` 加 `HasRenderUI()` / `SetWndProcHook()` / UI 插入点（RenderScene 与 Present 之间） | `GameCore.cpp:123-134 / :238-240` |
| R3.7 | `GpuTimer`（TIMESTAMP 查询 + 回读 + 围栏三段式），Performance 面板显示真实 GPU 帧时间 | `Engine/04_Renderer/GpuTimer.{h,cpp}` |

### 6.2 计划外修复（13 条，均为「跑起来才暴露」）

| # | 缺陷 | 修法 |
|---|---|---|
| 1 | 注册表恒为空（静态库 dead-strip，静态初始化器从未运行） | `Instance()` 内显式 `RegisterRenderer`（不依赖静态对象） |
| 2 | `RESOURCE_BARRIER_INVALID_HEAP #741`：对 UPLOAD 堆资源发 `COPY_SOURCE` | `TransitionResource` 对 UPLOAD/READBACK 早退 |
| 3 | 切 Renderer 后 0xC0000005：`CommandContext` 缓存了已销毁堆指针 | `Reset()` 里 `ZeroMemory(m_CurrentDescriptorHeaps, ...)` |
| 4 | 运行时换模型后模型消失 | `ModelInstance::operator=` 补 `m_IsDirty = true` |
| 5 | 换模型命中 `#921 OBJECT_DELETED_WHILE_STILL_IN_USE` | `UnloadModel()` 先 `IdleGPU()` |
| 6 | `GpuBuffer::Destroy` 不归还 SRV/UAV 描述符 | 加 `Destroy()` override + `Free`，恢复 `TypedBuffer` 分配守卫 |
| 7 | `EnvironmentLightingManager` 标志跨 Renderer 不复位（二次 Initialize 不烘焙） | Initialize/Shutdown 复位 `m_Baked/m_Ready/m_HDRLoaded` 并释放 cubemap |
| 8 | `LightingSystem::Shutdown()` 零调用者 | 两个 Renderer 的 `Shutdown` 各补一行 |
| 9 | `ModelLoader` 采样器缓存跨 Renderer 失效（堆重建后偏移含义变了） | `DescriptorHeap` 加 `GetGeneration()`，缓存存 `{generation, offset}` 并校验 |
| 10 | `TextureManager` 的 SRV 从不归还（切 Renderer 持续涨描述符） | `ManagedTexture` 加析构 `Free`（`m_OwnsDescriptor` 区分默认纹理） |
| 11 | `GpuResource` 不记堆类型，导致 #741 无处判断 | 加 `m_HeapType` + `GetHeapType()` |
| 12 | `ForwardApp` 从不调 `Renderer.Update`、`Models` 恒空 | 补接线（`Tooiyume.cpp`） |
| 13 | 宽字符 `printf` 用 `%s` 打 `std::string` / `%hs` 打 `wchar_t*`，日志乱码 | 统一 `%hs`（窄）/ `%ws`（宽） |

### 6.3 验收（✅ 全部通过）

| 验收项（路线图 §6.3） | 结果 |
|---|---|
| 同一入口切 Forward / Deferred，不崩不残留 | ✅ `-R3Test` 11 步全通：Deferred → Forward → Deferred，描述符 RTV/SRV/pool = 13/65/8 → 13/61/8 → 4/29/8 → 13/61/8，**堆池恒 8** |
| 切模型无残留、无崩溃、无泄漏 | ✅ 运行时由 merlin 切 saber_emiya 正常出图；`r3_06`（切模型后）与 `r3_08`（切回 Deferred 后）**逐字节相同** md5 `488a8074f730` |
| FPS / 帧时间 / Debug View 可用，且区分 CPU 与 GPU 时间 | ✅ Performance 面板 CPU frame 3.60 ms / CPU FPS 277.7 / **GPU frame 2.346 ms**（`GpuTimer`）；Debug View 8 项下拉全部出图 |
| 拖已接通参数画面可预测变化 | ✅ 差分 bbox 恒为模型区域 `(749,117,1138,577)`；Sun 3→8 模型区平均亮度 184.36→188.84、关灯→179.34、Exposure 1→3→213.10；10 张截图 md5 各不相同 |
| Debug Layer 干净 | ✅ 日志 `grep -c "D3D12 ERROR\|WARNING\|CORRUPTION\|LIVE OBJECT"` = 1，唯一命中是 InfoQueue 配置行 |

**证据**：`docs/AI/R3/r3_00_initial_deferred.png` … `r3_09_imgui_panels.png`（10 张，全部 1920×1080 GPU 回读）

### 6.4 R3 顺带记录的新经验（已并入 §8）

- 静态库里的**文件级静态对象**（自动注册器）会被 dead-strip 静默丢弃，且 **dumpbin 对已链接 exe 无符号表**，只能靠扫二进制里的 UTF-16LE 字面量取证。
- 判断「是渲染错还是状态错」时，**给清屏色一个与黑不同的值**能一眼区分「没 draw」与「链路坏」。本阶段正是靠 `Color(0.10,0.12,0.16)` 定位到「Forward 只画了 ClearColor」。
- `-Screenshot` 目录参数**必须反斜杠**；正斜杠会让 `SaveToWICFile` 返回 `0x8007007B`。

---

## 6b. R4 已完成（✅）

主题：**光栅 PBR 闭环 —— 显示编码链 / IBL 状态机 / AlphaTest**。详见 `docs/AI/R4/R4_光栅PBR闭环.md`。

### 6b.1 交付内容

| ID | 内容 | 落点 |
|---|---|---|
| R4.1 | **显示编码链统一**：新增 `ColorPipeline.hlsli`（`LinearToSRGB` / `ACESFilm` / `EncodeDisplayColor`），Lighting 与 Skybox **共用**。修复 Skybox 完全绕过曝光（旧实现直接 `return float4(hdrColor)`） | `Engine/Shader/02_Deferred/ColorPipeline.hlsli`（新增）、`LightingPS.hlsl`、`SkyboxPS.hlsl` |
| R4.2 | **CB 契约 96 → 112 字节**：追加 `float4 DisplayParams`（x=EnableSRGBEncode y=PrefilterMaxMip）。三处同步 + `static_assert` | `RenderParams.h`、`LightingSystem.h/.cpp`、`LightingPS.hlsl` |
| R4.3 | **Prefilter mip 上界**：写死 `8.0` → 烘焙后回填真实值 `4`（5 mip − 1）。该值由渲染器派生，不进用户真源 | `DeferredRenderer.cpp:Update`、`LightingSystem::SetPrefilterMaxMip` |
| R4.4 | **IBL 状态机**：`m_HDRLoaded` 从「死标志 + 语义反了」改为真实语义；烘焙短路条件不再包含它 | `EnvironmentLightingManager.cpp` |
| R4.5 | **AlphaTest**：`GBufferPS` 位域解码 + `clip`，`alphaRef` 用 `f16tof32` 解 half | `GBufferPS.hlsl` |
| R4.6 | **验收脚本** `-R4Test`：10 步单变量 A/B，全部 GPU 回读 | `Demos/RendererDemoApp/RendererDemoApp.cpp` |

### 6b.2 验收（✅ 全部通过）

| 验收项 | 结果 |
|---|---|
| Debug + Release 构建 | ✅ 均 exit 0；3 个 Shader 实际重编译 |
| 模型区硬截断 | ✅ **23.0% → 0.1%** |
| 天空区硬截断 | ✅ **52.0% → 0.0%** |
| sRGB A/B | ✅ 模型区 176.9（开）vs 132.3（关），md5 不同 |
| Tonemap A/B | ✅ 关闭后模型 32.2%、天空 67.2% 硬截断 |
| Skybox 跟随曝光 | ✅ 曝光 4.0 → 天空 250.2（100% 截断），证明天空进链 |
| AlphaTest A/B | ✅ 差异只在镂空 bbox 67 px；关 clip 时眼睛为实心黑块 |
| IBL 贡献 | ✅ 只留 IBL 167.1 / 关 IBL 146.5 |
| IBL 状态机日志 | ✅ `HDRLoaded=1 PrefilterMips=5 (roughness 映射 0..4)` |
| 可重复性 | ✅ 首帧与末帧 md5 完全相同 `16c8aeec…` |
| Debug Layer | ✅ 0 ERROR |
| 描述符泄漏 | ✅ 10 步切换 RTV/SRV/pool 恒定 |

**证据**：`docs/AI/R4/evidence/`（10 张 GPU 回读 + `r4_10_alphatest_ab_zoom.png` 放大对照）；基线 `docs/AI/R4/baseline/`。

### 6b.3 R4 纠正的两处旧结论

- ~~「IBL 四个烘焙 Pass 是空壳」~~ → 四个 Pass 均有真实 `Dispatch`（`EnvironmentLightingManager.cpp:254/:284/:322/:352`）。旧结论来自更早版本，`git show HEAD:` 同样已含 Dispatch。
- ~~「模型偏白主因是 IBL 直接采样 HDR」~~ → 真因是显示编码链缺失（`*_UNORM` + 无 sRGB 编码 + Skybox 绕过曝光）。

---

## 6c. R5 已完成（✅）

主题：**DXR 最小闭环 —— 设备能力 / 加速结构 / DispatchRays 统一入口**。详见 `docs/AIWorkLogs/TR-20261027-01_R5-DXR最小闭环.md`。

### 6c.1 交付内容

| ID | 内容 | 落点 |
|---|---|---|
| R5.1 (D0) | **能力查询与请求解耦**：`CheckFeatureSupport(OPTIONS5)` 从「仅在 App 请求时查」改为**无条件执行**并记录 `g_RaytracingTier` / `g_bRaytracingSupported`；只有 `RequireDXRSupport && !supported` 才中止。新增 `QueryRaytracingCapability()` 供 UI 使用 | `Engine/02_RHI/GraphicsCore.{h,cpp}` |
| R5.2 (D0) | **`RequiresRaytracingSupport()` 补齐 `const override`**。旧写法 `virtual bool RequiresRaytracingSupport();` 少 const，与基类 `IGameApp::RequiresRaytracingSupport() const` 是**两个不同函数** → 请求被基类默认 `false` 静默吞掉 | `Demos/RayTracingApp/RayTracingApp.cpp:44/:74` |
| R5.3 (D0) | `RendererFeature::Raytracing = 1u << 7`，UI 可读的能力位 | `Engine/04_Renderer/Renderer/Base/BaseRenderer.h:100` |
| R5.4 (D1) | **Shader 链支持 DXIL library**：新增 `compile_raytracing_library()`（`lib_6_4`，**不传 `-E`**），一份 hlsl 里的 RayGen/Miss/ClosestHit 全编进同一库 | `Engine/20_cmake/ShaderCompiler.cmake:173`、`Engine/CMakeLists.txt:116` |
| R5.5 (D1) | **`RaytracingPSO`**（State Object 独立封装，**不复用** GraphicsPSO 缓存）：DXIL_LIBRARY + HIT_GROUP + SHADER_CONFIG + PIPELINE_CONFIG + GLOBAL_ROOT_SIGNATURE，失败带 `GetLastError()` | `Engine/02_RHI/Pipeline/RaytracingPSO.{h,cpp}`（新增） |
| R5.6 (D1) | **BLAS/TLAS 构建与生命周期**：GPU→GPU `CopyBufferRegion` 聚合几何；每 `(实例,mesh,draw)` 一个单几何 BLAS（`InstanceID` 即可定位，Tier 1.0 可用）；静态几何复用 BLAS，只在实例变化时重建 TLAS；scratch 各段互不重叠 | `Engine/04_Renderer/Renderer/RayTracing/RaytracingScene.{h,cpp}`（新增） |
| R5.7 (D1) | **AS 资源正确创建**：`Flags = ALLOW_UNORDERED_ACCESS` + `InitialState = RAYTRACING_ACCELERATION_STRUCTURE`，全程不发 ResourceBarrier（三个 Debug Layer 错误的最终解） | `RaytracingScene.h:111`、`RaytracingScene.cpp:53` |
| R5.8 (D2) | **`RayTracingRenderer`**：RayGen/Miss/ClosestHit + SBT；命中返回法线色、未命中返回深蓝背景；输出 `R8G8B8A8_UNORM` 经统一 Present 上屏；设备不支持时 `Initialize` 返回 false 而非崩溃 | `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.{h,cpp}`（新增） |
| R5.9 (D2) | **注册与切换**：`RendererRegistry` 登记工厂（不检查能力）；`BindRendererOutput` 加 `dynamic_cast<RayTracingRenderer*>` 分支；UI 面板替换硬编码 `"Not implemented (R5)"` 为真实 Tier 显示 | `RendererRegistry.cpp:46`、`RendererDemoApp.cpp:418/:1410-1434` |
| R5.10 | `CommandContext::GetCommandList4()` 暴露 CommandList4（内部本就是它） | `Engine/02_RHI/Command/CommandContext.h:147` |
| R5.11 | `ModelInstance::GetModel()` / `GetLocalToWorld()` 只读访问器 | `Engine/05_Scene/Model/Model.h:121/:129` |

### 6c.2 验收（✅ 全部通过）

| 验收项 | 结果 |
|---|---|
| Debug + Release 构建 | ✅ 均 `[build] OK` |
| DXR 能力日志 | ✅ `[DXR] capability: supported=1 tier=0.12 inline=1`（`0.12` 是打印格式问题，实为 Tier 1.2） |
| BLAS 构建 | ✅ `RebuildGeometry 完成：模型=1 唯一几何=1 BLAS=15 三角=71803 scratch=1540096 B` |
| SBT | ✅ `SBT 构建完成：RayGen=RayGen Miss=Miss HitGroup=HitGroup 记录=32 B` |
| TLAS | ✅ `加速结构就绪：BLAS=15 TLAS实例=15 三角=71803` |
| DispatchRays | ✅ `首帧 DispatchRays 已提交：1920x1080，SBT RayGen=0x337A91000` |
| **DXR 画面** | ✅ GPU 回读 108371 B，非背景像素 bbox `x[752..1136] y[120..576]`，彩色法线着色模型 |
| 切换链路 | ✅ DXR → Forward → Deferred 均成功，切回光栅画面正常 |
| 光栅基线未回归 | ✅ Deferred 对照 1734511 B 画面正常 |
| Debug Layer | ✅ **0 ERROR** |

**证据**：`docs/AI/R5/screenshot_90.png`（DXR 画面）、`docs/AI/R5ctl/screenshot_90.png`（光栅对照）、`docs/AI/R5switch/`（切换链路 3 张）。

### 6c.3 R5 踩到的四个「零报错」黑屏根因（按隐蔽程度排序）

1. **`DispatchRays` 读的是计算根签名**，用 `SetGraphicsRootSignature` 绑定时根参数不生效 → 全黑无报错。
2. **HLSL 默认 `column_major`**，`RayTracing.hlsl` 漏 `#pragma pack_matrix(row_major)` → `InvViewProj` 等价转置 → 射线全打空（画面是 miss 背景色而非纯黑）。
3. **`shader_attach_header_dependency` 漏挂 `RayTracingRenderer.cpp`** → 改 HLSL 后 `.cpp` 不重编 → exe 里是旧字节码，**前几轮 shader 修改全部静默失效**。
4. **`DispatchRays` 的 `Width/Height` 被 `RebuildTopLevel` 清成 0 后从未填回** → 派发 0 条射线。

**这四条的共同点：D3D12 不报任何错误、日志全绿、程序正常退出。** 只能靠「GPU 回读 + 像素值反推阶段」定位。

---

## 6d. VS 环境排错与功能验证（✅ 2026-10-27 完成）

### 6d.1 做了什么

用 **VS 2022 生成器**（`Visual Studio 17 2022` + `-A x64`）在 `out/build/vs2022/` 建了第三条构建树，与 `build.bat` 的 Ninja 两条树并存，验证「VS 里点生成 / 按 F5」这条路径是否真的可用。

| 验证项 | 命令 | 结果 |
|---|---|---|
| 配置 | `cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=...` | ✅ `MSVC 19.38.33145.0`，Configuring/Generating done |
| 构建（cmake 驱动） | `cmake --build out/build/vs2022 --config Debug` / `--config Release` | ✅ 两个配置全目标成功 |
| **构建（VS 原生入口）** | `MSBuild.exe out/build/vs2022/TooiyumeRender.sln /p:Configuration=Debug /p:Platform=x64` | ✅ **exit 0**，与 VS 里 F7 完全等价 |
| 运行 Debug | `out/build/vs2022/Demos/RendererDemoApp/Debug/RendererDemoApp.exe -StartRenderer RayTracingRenderer -Screenshot ...` | ✅ DXR 全链路日志齐全，截图 108371 B |
| 运行 Release | 同上，Release 目录 | ✅ 同上 |
| **画面等价性** | md5 对比 VS 树 vs Ninja 树 | ✅ **逐字节相同**：DXR `1bea89b2…`、Deferred `16c8aeec…`、切换 `r3_07`/`r3_08` 全一致 |
| 调试器下运行 | `cdb -o -g -G -c 'sxe 87a; sxe av; g'`（等价 F5） | ✅ **零 `D3D12 ERROR`、零异常中断**（GPU-Based Validation 开启状态） |
| 切换链路 | `-StartRenderer RayTracingRenderer -R3Test` | ✅ DXR→Forward→Deferred 全成功，`RayTracingRenderer 已关闭（累计 DispatchRays 帧数=70）` |
| 光栅回归 | `-StartRenderer DeferredRenderer` | ✅ 画面与 Ninja 版 md5 相同，PBR+IBL 正常 |

### 6d.2 修掉的两个真实缺陷

**缺陷 1：`DeployRuntimeDlls.cmake` 在多配置生成器下选错 DLL 变体**

- 位置：`Engine/20_cmake/DeployRuntimeDlls.cmake:34`（原代码）
- 原代码：`if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND EXISTS "${_root}/debug/bin")`
- 为什么错：VS 生成器是**多配置**生成器，`CMAKE_BUILD_TYPE` 在配置期是固定的单值（本项目 configure 时写的是 `Debug`），**与用户随后选的 `--config Release` 无关** → 判断恒真 → Release 产物目录里拷的一直是 `debug/bin` 的调试版 DLL。
- 实测证据：修复前 `out/build/vs2022/Demos/RendererDemoApp/Release/DirectXTex.dll` md5 = `fb49132c`（= `vcpkg_installed/x64-windows/debug/bin`），而正确值应为 `e8622bdc`（= `bin`）。
- 后果：Release exe 链接调试版 CRT 的第三方库（`/MTd` 与 `/MT` 混用），本机碰巧因为导出接口兼容而能跑，但这是 UB。
- 修复：改用生成器表达式 `"$<IF:$<CONFIG:Debug>,${_root}/debug/bin,${_root}/bin>"` —— 单配置生成器（Ninja）下 `$<CONFIG>` 就是 `CMAKE_BUILD_TYPE`，多配置下取当前正在构建的配置，两种生成器都正确。
- 修复后验证：VS Release → `e8622bdc` / `d7827b47`（正确）；VS Debug → `fb49132c` / `d6fa60ed`（正确）；Ninja 两条树不变（Debug 用 debug/bin、Release 用 bin）。

**缺陷 2：重复 proxy 环境变量让 MSBuild 报假错「找不到编译器」**

- 现象：`cmake -G "Visual Studio 17 2022"` 报 `No CMAKE_CXX_COMPILER could be found`，看起来像 VS 装错了。
- 真实根因（藏在 `out/build/vs2022/CMakeFiles/CMakeConfigureLog.yaml` 里）：
  ```
  error MSB6001: "CL.exe" 的命令行开关无效。
  System.ArgumentException: 已添加项。字典中的关键字:"NO_PROXY"所添加的关键字:"no_proxy"
    在 System.Collections.Hashtable.Insert(Object key, Object nvalue, Boolean add)
    在 System.Diagnostics.ProcessStartInfo.get_EnvironmentVariables()
  ```
- 机理：Windows 环境变量名**大小写不敏感**，但本机 shell 里同时存在大写与小写两套（`NO_PROXY`+`no_proxy`、`HTTP_PROXY`+`http_proxy`、`HTTPS_PROXY`+`https_proxy`）→ MSBuild 的 `ProcessStartInfo.EnvironmentVariables` 是大小写敏感的 `Hashtable` → 抛重复键 → 工具执行失败 → **被 CMake 误报为「找不到编译器」**。
- 修复（会话级，不改系统环境）：PowerShell 里
  ```powershell
  foreach($n in @('no_proxy','http_proxy','https_proxy','all_proxy')){
      [System.Environment]::SetEnvironmentVariable($n,$null,'Process')
  }
  ```
- **注意**：`Get-ChildItem env:` 本身也会因重复键抛 `An item with the same key has already been added`；用 `[System.Environment]::GetEnvironmentVariables()` 枚举才安全。
- **注意**：在 bash 里 `unset no_proxy` 对 MSBuild 无效 —— bash 只改自己的环境，`cmake --build` 派生的 MSBuild 继承的仍是父进程（这里是 DSH 宿主）的环境。必须在**同一个 PowerShell 进程**里清掉再调 MSBuild。

### 6d.3 VS 里怎么用（已配好）

`.vs/launch.vs.json` 已补 5 条配置（该文件被 `.gitignore:37` 忽略，属本机配置）：

| 配置名 | 参数 | 用途 |
|---|---|---|
| `RendererDemoApp - Deferred (R5 统一入口)` | `-StartRenderer DeferredRenderer` | 光栅 PBR 基线 |
| `RendererDemoApp - RayTracing (R5 DXR)` | `-StartRenderer RayTracingRenderer` | DXR 诊断画面 |
| `RendererDemoApp - DXR + GPU 回读截图` | `-StartRenderer RayTracingRenderer -Screenshot ...\docs\AI\R5final` | 出证据图 |
| `RendererDemoApp - R3 切换链路回归` | `-R3Test ...\docs\AI\R5switch` | 切换链路回归 |
| `RayTracingApp.exe (...)` | — | 原 RayTracingApp（D0 的 `const override` 修复对象） |

打开方式二选一：① `devenv.com out/build/vs2022/TooiyumeRender.sln`（VS 生成器工程，F7 生成 / F5 调试）；② VS 里「打开文件夹」→ 选 CMake 预设 `x64-debug`（走 `CMakePresets.json`，产物落 `out/build/x64-debug`，**与 `build.bat` 的 `out/build/Debug` 不同目录**，不要混用）。

### 6d.4 未验证项（必须标注）

| 项 | 状态 |
|---|---|
| 在真·VS IDE 图形界面里点 F5（而非 MSBuild/cdb 等价路径） | **未验证**（AI 无法操作 GUI；已用 MSBuild 直构 + cdb 调试器加载两条等价路径覆盖） |
| VS 里「打开文件夹」CMake 预设 `x64-debug` 路径（`out/build/x64-debug`） | **未验证**（该树是历史遗物，按 AGENTS.md 规定禁用；本轮只验证 VS 生成器树） |
| 多模型 DXR 场景（验证 TLAS 行列转置修复） | **未验证**（见 §7） |
| VS 的 IntelliSense / 代码导航 | **未验证**（不属构建与运行正确性） |

---

## 6e. R6 / D3 光追算法（✅ 2026-10-27 完成）

> 施工日志：`docs/AIWorkLogs/TR-20261027-03_R6-光追算法.md`
> 路线图来源：`docs/AI/ToiyumeRender_升级路线图_2026-10-24.md:173-175`

### 6e.1 路线图 D3 六条要求 → 落地情况

| D3 原文要求 | 落地 | 证据 |
|---|---|---|
| 先做硬阴影射线 | `RayTracing.hlsl` 的 `ClosestHit` 发 `ShadowPayload` 射线（`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH \| SKIP_CLOSEST_HIT_SHADER`） | `r6_02_debug_shadow.png`（黑白分明的自遮挡） |
| 再做反射 | 同函数内一次 `RayPayload` 反射射线（`Depth=1`）+ Schlick Fresnel + 强度混合 | `r6_04` vs `r6_05`（有/无反射的调试通道对照） |
| 可比较的光栅/光追场景 | `RendererDemoApp` 运行时切 Deferred/Forward/RayTracing，模型与相机不变 | `docs/AI/R6ctl/screenshot_90.png` = `16c8aeec…`（光栅对照） |
| 明确采样与偏差策略 | 每像素 1 条主射线（无累积/抖动）；**唯一偏差** = 沿法线 `RTShadowNormalBias`（0.002 m），阴影与反射共用；阴影/反射各 1 次弹射 | `r6_09_shadow_bias_large.png`（偏差放大到 0.05 m 后接触阴影消失） |
| 性能统计 | `RayTracingFrameStats`（primary/shadow/reflection/total 发射数 + 三角/BLAS/实例数）+ GPU 帧时间 | 面板截图 `r6_11_imgui_panel.png` |
| 开关 | `EnableRTShadow` / `EnableRTReflection` / `RTDebugView` + 独立「Ray Tracing (R6)」面板 | 同上 |

### 6e.2 交付内容

1. **`Engine/04_Renderer/RenderParams.h`**：新增 `RayTracingDebugView` 枚举（Shaded/Normal/Shadow/Reflection/HitDistance）与 6 个字段：`EnableRTShadow=true`、`EnableRTReflection=false`、`RTShadowNormalBias=0.002f`、`RTShadowMaxDistance=100.0f`、`RTReflectionMaxDistance=200.0f`、`RTReflectionIntensity=0.6f`、`RTDebugView=Shaded`。理由是 DXR 没有 GBuffer，不能复用 `DebugView`（见 §8 取舍 B）。
2. **`RayTracingRenderer.h/.cpp`**：根常量 5→9 字段（128→192 B / 32→48 DWORD）、`RayTracingFrameStats` + `GetFrameStats()`、RTPSO `exports=4`（加 `ShadowMiss`）、`MaxPayloadSizeInBytes=32`、`MaxTraceRecursionDepth=2`、统计写入、开关变化日志。
3. **`RaytracingScene.cpp`**：`BuildShaderBindingTable` 加 `shadowMissExport` 参数，miss 表 1→2 条（`kShadowMissOffset=96`、`kHitGroupOffset=128`、`kTotalBytes=192`）。拿不到 ShadowMiss 标识符时**直接失败**（不能静默降级为主 Miss —— 32 B 载荷被当 8 B 解释会越界写）。
4. **`Engine/Shader/07_RayTracing/RayTracing.hlsl`**（468 行，整文件重写）：`ShadowPayload`、`SkyColor(dir)` 梯度天空、`TraceShadowRay`、`TraceReflectionRay`、递归闸门、4 条调试通道。
5. **`RendererDemoApp.cpp`**：`RunR6Test()`（12 步单变量 A/B）、`DrawRayTracingPanel()`（独立面板）、面板重排（`Material/Lighting` 移到第二列下方、R6 面板放右上）、`Material/Lighting` 的 DXR 门控修正（太阳参数在 DXR 下可编辑，点光源/IBL 保持禁用）。
6. **`.vs/launch.vs.json`**：6 条 → 4 条（`DeferredApp` / `TooiyumeApp` / `RayTracingApp` / `RendererDemoApp`）。

### 6e.3 三条硬证据

**① 射线计数随开关变化（算法真在跑）**

| 参数 | primary | shadow | reflection | total |
|---|---|---|---|---|
| shadow off / reflect off | 2073600 | 0 | 0 | 2073600 |
| shadow on / reflect off | 2073600 | 2073600 | 0 | 4147200 |
| shadow on / reflect on | 2073600 | 2073600 | 2073600 | 6220800 |

`2073600 = 1920×1080`。**统计滞后一帧**（`Render` 末尾写、`Update` 开头读），读日志时要注意。

**② md5 等价关系（像素级证明）**

- `r6_00_baseline_shadow_on` == `r6_06_reflect_intensity_0` == `r6_10_restored` = `a43b605977a626e747c91559dcb1bb7f`
  → 「反射强度=0」与「反射关」像素级等价；11 步来回切参数后**精确复原**。
- `r6_00` ≠ `r6_01_shadow_off`（1.61% 采样点不同，maxd=649）→ 阴影开关真的改像素。
- `r6_00` ≠ `r6_03_reflection_on` ≠ `r6_07_reflect_intensity_1` → 反射强度 0/0.6/1.0 三档都生效。

**③ Debug / Release / 光栅 三方一致**

- Debug 与 Release 的 11 张渲染图 md5 **完全相同**（只有含 FPS 文本的 ImGui 面板图不同）。
- 光栅对照 `docs/AI/R6ctl/screenshot_90.png` = `16c8aeec294844b9e3fedde4df78a349`，与 R5 的 `R5ctl` **同 md5** → R6 对光栅路径零影响。
- `-R3Test` 切换链路：`RayTracingRenderer → ForwardRenderer → DeferredRenderer` 全程正常，`r3_07_forward` / `r3_08_back_deferred` 与 R5 同名文件 md5 相同。
- 全程 `grep -cE "D3D12 ERROR"` = 0。

### 6e.4 顺带修掉的致命构建缺陷（比 R6 本身更重要）

**`msvc_deps_prefix` 双重编码 → 头文件依赖全失效。**

- 症状：R6 首跑新增字段全读成 0，`EnableRTReflection` 默认 false 却打成「开」。
- 根因：`ninja -t deps` 显示 74 个 obj 全 `#deps 0`。`rules.ninja:17` 的 `msvc_deps_prefix` 是乱码（「注意: 包含文件: 」被存成 `娉ㄦ剰: 鍖呭惈鏂囦欢:`）。机理：`cl /showIncludes` 按控制台代码页输出，CMake 在 configure 期抓这段写进 `rules.ninja`，Ninja 之后用这个字节串去 grep；系统 ACP=936、控制台 65001 → 前缀乱码 → 一行都匹配不到 → **所有依赖静默丢失**。
- 后果：编译链接全绿，但改结构体后其他 obj 保留旧布局 → 运行时内存错位，且完全静默。
- 修复（`scripts/build.bat`）：① 步骤 0 `chcp 65001`（同时包住 configure 与 build）；② 代码页哨兵 `_tooi_codepage.txt`，哨兵缺失/不符则整树重配；③ 步骤 6b `ninja -t deps` + `findstr /R "#deps [1-9]"` 自检（结果文件字节数=0 才警告）；④ 两条退出路径恢复原代码页。
- 验证：`msvc_deps_prefix` 恢复正确、`RayTracingRenderer.cpp.obj` `#deps 53`、`touch RenderParams.h` 后 `ninja -n` 报 9 个构建步骤。

### 6e.5 未验证项

| # | 项 | 原因 |
|---|---|---|
| U1 | 真 VS GUI 里点 F5 | AI 无 GUI 操作能力，需用户手动确认 |
| U2 | VS 下拉菜单是否还剩自动生成的重复项 | VS 会为每个 target 自动生成默认启动项；`launch.vs.json` 已精简到 4 条，但 VS 侧的自动项需目视确认 |
| U3 | 反射画质「好不好看」 | 人肉审美 |
| U4 | 多模型场景的阴影/反射 | 本轮只有单模型 merlin |
| U5 | 性能基准（射线/秒、MVPS） | 未做基准测试，不属 D3 |

---

## 6f. R7 / D4 光追接材质（✅ 2026-10-28 完成）

**一句话**：DXR 的 `ClosestHit` 从「中性灰 albedo + Lambert」升级为「真实 BaseColor 贴图 × 材质因子 + GGX/Cook-Torrance」，并用**两个正交开关**做出 2×2 可自我证伪的验收矩阵。
施工日志：`docs/AIWorkLogs/TR-20261028-01_R7-材质接驳.md`（527 行）。设计方案：`docs/AI/R7_材质接驳_设计方案.md`（576 行，含拍板记录）。

### 6f.1 拍板结果（m03148）

| 编号 | 决策 | 结果 |
|---|---|---|
| D1 | sRGB 采样语义 | **(a) 保持 linear** —— 与光栅现状一致（光栅也不走 sRGB，属既有缺陷，单独立项） |
| D2 | 光照模型 | **(b) P0 就上 GGX/Cook-Torrance**（不是先 Lambert） |
| D3 | 贴图范围 | **(a) 只做 BaseColor**，MR / AO / Emissive / Normal 留 P1 |
| D4 | alpha test | **(a) 留到 P2** |
| D5 | 调试通道 | **采纳** `RayTracingDebugView::BaseColor = 5` |
| D6 | 贴图寻址 | **方案 A（无界 SRV 表）施工**，方案 B（SBT 本地根签名）留演进 |

因为 D2 让 P0 同时改「接贴图」与「换光照模型」两个变量，为可归因必须拆出两个正交开关 —— 这是本轮设计的核心。

### 6f.2 交付内容

| # | 项 | 落点 |
|---|---|---|
| 1 | 两个正交开关 | `RenderParams.h`：`EnableMaterialTextures` / `EnableGGX`（默认均 true） |
| 2 | 每实例数据 32 B → 96 B | `RaytracingScene.h`：+`UvByteOffset` / `VertexFlags` / `MaterialIndex` / `SrvTableIndex` / `BaseColorFactor[4]` / `EmissiveFactor[3]` / `NormalTextureScale` / `MetallicFactor` / `RoughnessFactor` / `MaterialFlags` / `_padding` |
| 3 | 材质 CPU 副本 | `Model.h` 新增 `m_MaterialConstantData`（建 BLAS 时要烘进每实例数据，GPU 常量缓冲在 DEFAULT 堆无法 Map） |
| 4 | UV 读取 | `RayTracing.hlsl::LoadVertexUv`：`Load<uint>` 取 32 位字 → 拆半字 → `f16tof32` |
| 5 | 贴图采样 | `LoadInstanceAlbedo`：重心插值 3 个顶点 UV → `g_Textures[NonUniformResourceIndex(srvBase)]` → `SampleLevel(...,0)` × `baseColorFactor` |
| 6 | GGX/Cook-Torrance | `DistributionGGX` + `VisibilitySmithGGXCorrelated`（已含 1/(4·nDotV·nDotL)）+ `FresnelSchlick`；`diffuseBRDF = kd*albedo/π` |
| 7 | 根签名扩 6 参数 | `RayTracing.cpp:288`：`InitAsDescriptorRange(SRV, 3, UINT_MAX)` + `InitStaticSampler(0)`；b0 从 48 → 52 DWORD |
| 8 | 描述符堆合并 | 输出 UAV 与贴图 SRV 同进 `s_TextureHeap`（D3D12 一个命令列表只能绑一个 CBV_SRV_UAV 堆）；0 号槽 = 输出 UAV |
| 9 | 调试通道不过编码链 | `bypassDisplay = (debugView != 0) && (HitDistance >= 0)` → 与 Deferred 的调试通道同样是「原始缓冲区数值」 |
| 10 | UI + 脚本化验收 | 面板加两个 Checkbox + 6 项通道下拉；`-R7Test` 11 步 |

### 6f.3 三条硬证据

**① A5a 逐字节回归（可自我证伪点）**

| 对照 | md5 | 结论 |
|---|---|---|
| A5a（贴图关 + GGX 关） | **`a43b605977a626e747c91559dcb1bb7f`** | **== R6 基线，逐字节** |
| A5b（贴图关 + GGX 开） | `fdd4d354135f4ba57316a15f93012c93` | ≠ 基线 → GGX 真的生效 |
| A5c（贴图开 + GGX 关） | `3c952bcbe770b9b66e59b8b4889661da` | ≠ 基线 → 贴图真的生效 |
| A5d（都开） | `2a52634f375a0d8bddc338e79c56e289` | ≠ 其余三者 → 两变量正交 |

四个 md5 互不相同 + 全关等于 R6 → **两个开关各自独立生效且可分离归因**。

**② A7：Deferred ↔ DXR 的 BaseColor 一致（通过）**

模型 mask（`|defBase − Deferred最终画面| > 10`，81174 px）：
```
DXR  base mean [178.13, 178.53, 205.39]
DEF  base mean [177.52, 177.90, 204.75]
mean|diff|  2.9      median 1.0      identical(<=2) 81.9%      diff>30 2.1%
亮度相关系数 0.993
```
修复前是 `mean|diff| 67.2 / corr 0.52` → **提升 23 倍**。人工判读（`docs/AI/R7test2/a7_compare_3way.png` 三栏 2× 放大）两张图肉眼几乎不可区分。
残留 2.1% 已归因：R7 用 `SampleLevel(...,0)` 强制 mip0，光栅用 `Sample` 自动 LOD（含各向异性）；且 R7 写 alpha=1，光栅写 alpha=ao。

**③ A6：光栅零回归（通过）**
```
docs/AI/R7ctl/screenshot_90.png  =  16c8aeec294844b9e3fedde4df78a349
docs/AI/R6ctl/screenshot_90.png  =  16c8aeec294844b9e3fedde4df78a349   ← 相同
docs/AI/R5ctl/screenshot_90.png  =  16c8aeec294844b9e3fedde4df78a349   ← 相同
```
R7 对光栅路径**零字节影响**。

**④ 其他一致性**
- `r7_00 == r7_08`（11 步来回切参数后精确复原）
- `r7_06 == r7_04`（切 Deferred 再切回 DXR，描述符偏移重算正确，零漂移）
- Debug / Release 9 张渲染图 md5 **全同**（见 `docs/AI/R7test_rel/README.md`）
- `grep -c "D3D12 ERROR"` = **0**

### 6f.4 本轮最隐蔽的坑（值得单独记）

`ByteAddressBuffer::Load<T>` 是**按 T 的字节宽度裸位拷贝，不做格式转换**。
`LoadVertexUv` 第一版写 `g_SceneData.Load<float2>(addr & ~3u)` 想读 4 字节的 `R16G16_FLOAT` UV —— 实际读了 8 字节，把**下一个属性的 4 字节**拼进来当第二个 float 解释 → uv 变成 ~1e-38 的 denormal，等价于 `(0,0)`。

后果：整张贴图退化成「每个材质一片纯色」，布料褶皱与脸上画好的眼睛全部消失，**但不报任何错、不崩、日志正常、截图看起来"确实有颜色"**。极易误判为"已经接上了"。
正确写法：`Load<uint>` → 拆半字 → `f16tof32`（与 `LoadIndex` 处理 16 位索引完全一致）。

### 6f.5 未验证项

| # | 项 | 说明 |
|---|---|---|
| U1 | **DXR 最终着色比 Deferred 暗** | 模型区中位亮度 DXR `47` vs Deferred `211`（0-255）。三条原因已定位：无 IBL 预过滤贴图、无点光源、无 BRDF LUT。是 D3「只做 BaseColor」的合成结果，不是 bug，归 **P1** |
| U2 | alpha test 未做 | D4 拍板留 P2，`GEOMETRY_FLAG_OPAQUE` 恒真 |
| U3 | 单模型 / 单 draw 未验证 | `StartIndex` / `BaseVertex` 填充时恒 0；`LoadVertexUv` 里用 `baseVertex + iN`，若索引本身已是绝对索引会重复偏移 —— 单模型无影响，**多模型场景需复核** |
| U4 | 真 VS GUI 里点 F5 | AI 无 GUI 能力 |
| U5 | 仅 merlin 一个模型 | Saber 未跑 DXR 路径 |

---

## 7. 已知遗留问题

| 问题 | 说明 | 归属 |
|---|---|---|
| ~~模型整体偏白~~ | **R4 已解决**。根因**不是** IBL 采样 HDR，而是显示编码链缺失：SceneColor/swapchain 都是 `*_UNORM`，全链路无 sRGB 编码，且 Skybox 完全绕过曝光。修复后模型区硬截断 23.0%→0.1%，天空 52.0%→0.0% | ✅ R4 |
| ~~IBL 烘焙 4 个 Pass 是空壳~~ | **旧结论有误，R4 施工时已核实**：四个 Pass 都有真实 `Dispatch3D/Dispatch2D`（`EnvironmentLightingManager.cpp:254/:284/:322/:352`）。真正的问题是 `m_HDRLoaded` 死标志语义反了 + Prefilter mip 上界写死，均已修 | ✅ R4 |
| Forward 只输出 baseColor | `DefaultPS.hlsl::MainPS` 只返回 baseColor，不消费 `Light b0` → Forward 面板的光照控件被 `BeginDisabled` 禁用。**R4 范围外，顺延未做** | **R4 后续** |
| 光照硬编码 | R3 已改为 `RenderParams` 单点真源 | ✅ R3 已解决 |
| `TextureRef::operator=` 不减 Unload | 只减引用计数不 `Unload`，缓存条目残留（不会 double free） | R4 观察项（本轮未触碰） |
| `PipelineStateCache::s_PipelineDescs` 的 `Reset()` 无调用者 | 跨 Renderer 单调增长（仅 CPU 描述，含 `<0xffff` 断言）。**R6 未处理（R6 实际做的是 D3 光追算法）** | 顺延 D4+ |
| AlphaBlend（glTF BLEND） | 路线图 §7.1 明确与 AlphaTest 分开排期。R4 只做了 MASK。**R6 未做** | R6 扩展（未开始） |
| 架构自检 6 项 RED | ai-check.ps1 报 R1/R2/R3/R4/R13/R9 红灯，**R4 之前即已存在**，属 WP0–WP8 排期债 | WP0–WP8 |
| **VS 环境验证（R5）** | **✅ 已完成 2026-10-27**（详见 §6d）：VS 生成器树 `out/build/vs2022` 配置+构建成功、MSBuild 直构 `.sln` exit 0、Debug/Release 双配置运行通过、DXR 画面与 Ninja 版逐字节相同（md5 `1bea89b2…`）、Debug Layer + GPU-Based Validation 零 ERROR。**过程中修掉 2 个真实缺陷**：`DeployRuntimeDlls.cmake` 的 `CMAKE_BUILD_TYPE` 判断在多配置生成器下恒真（Release 误部署调试版 DLL）、MSBuild 被重复 proxy 环境变量（`NO_PROXY`/`no_proxy`）打断报假错「找不到编译器」 | ✅ 完成 |
| **R6 光追算法（D3）** | **✅ 已完成 2026-10-27**（详见 §6e）：硬阴影射线 + 单次反射 + 4 条调试通道 + 独立「Ray Tracing (R6)」面板 + `-R6Test` 12 步 A/B。md5 证明阴影开关改像素 1.61%、反射强度 0 与关闭像素级等价、来回切换精确复原、Debug/Release 11 张渲染图全同、光栅对照 `16c8aeec…` 无回归。**同轮修掉致命构建缺陷**：`msvc_deps_prefix` 在 ACP≠控制台代码页时双重编码 → `ninja -t deps` 全 `#deps 0` → 改任何 `.h` 都不重编（静默，编译全绿但运行时布局错位） | ✅ 完成 |
| **R7 光追接材质（D4）** | **✅ 已完成 2026-10-28**（详见 §6f）：BaseColor 贴图 + GGX/Cook-Torrance + 两个正交开关 + `RayTracingDebugView::BaseColor` 通道 + `-R7Test` 11 步。A5a 逐字节等于 R6 基线 `a43b6059…`、A5b/A5c/A5d 四 md5 互不相同、A7 对照 `mean|diff| 2.9 / corr 0.993`、A6 光栅 `16c8aeec…` 零回归、Debug/Release 9 张全同。**同轮修掉最隐蔽的 bug**：`Load<float2>` 读 4 字节 `R16G16_FLOAT` UV → uv 变 denormal ≈(0,0) → 整张贴图退化成纯色且不报错 | ✅ 完成 |
| DXR Tier 日志格式 | `GraphicsCore.cpp:219-220` 用 `tier=%d.%d`（`>>4` / `&0xF`），Tier 1.2 打印成 `0.12`。`QueryRaytracingCapability()` 返回原值，UI 不受影响。**R6/R7 未修（仅可读性）** | 顺延（未修） |
| ~~DXR 未接材质~~ | **✅ R7 已解决**：BaseColor 贴图 × `baseColorFactor` 已接进 `ClosestHit`，A7 对照与 Deferred 的 BaseColor 通道逐像素一致（`corr 0.993`）。**MR / AO / Emissive / Normal 贴图仍未采样**（D3 拍板只做 BaseColor），观感上 roughness 全 0.5 无空间变化 | ✅ BaseColor；其余 **P1** |
| DXR 最终着色偏暗 | 模型区中位亮度 DXR `47` vs Deferred `211`（0-255）。三条原因已定位：无 IBL 预过滤贴图、无点光源、无 BRDF LUT。是 D3「只做 BaseColor」+ 无 IBL 的合成结果，**不是 bug**，但观感差距大 | **P1（DXR IBL）** |
| BLAS 未去重 | BLAS 数量 = 实例数 × mesh 数 × draw 数；同一模型加载两次会重复建。**R6/R7 未处理** | 顺延 |
| 多模型 DXR 未视觉验证 | `Transform[i][j] = m[j][i]` 的行/列转置修复只在单模型下不显现（平移为 0），多模型场景尚未跑。另 `LoadVertexUv` 里用 `baseVertex + iN`，若索引本身已是绝对索引会重复偏移 —— 单模型无影响 | 待补测（R7 U3） |
| 未用 `RAYTRACING_ACCELERATION_STRUCTURE` 资源标志 | 本机 `d3d12.dll` 10.0.19041 不认该标志（#719），只能用 `ALLOW_UNORDERED_ACCESS` + AS 初始状态 | 保持现状（已写注释） |
| DXR 反射噪点 | 每像素 1 条反射射线、无累积缓冲、无降噪 → 有可见噪点。**刻意的**：只要求算法链路通；降噪需要时间累积 | 后续 |
| DXR 阴影不做 alpha test | 阴影射线用 `SKIP_CLOSEST_HIT_SHADER`，不采样贴图 alpha → 镂空/叶片/布料会投实心影。**R7 仍未做**（D4 拍板留 P2） | **P2** |
| DXR 不消费点光源与 IBL | HLSL 只有方向光 + 程序化 `SkyColor`。UI 上这两段在 DXR 下已 `BeginDisabled` 并注明原因，不放假控件。**R7 仍是只消费太阳** | **P1** |
| R6/R7 统计滞后一帧 | `RayTracingFrameStats` 在 `Render` 末尾写、`LogState` 在 `Update` 开头读 → 日志里射线数比当前帧参数晚一帧。读数现象非缺陷，已在日志与文档注明 | 保持现状 |
| `DrawDebugViewPanel()` 每帧无条件覆盖 `m_Params.Debug` | `RendererDemoApp.cpp:2062` 每帧执行 `m_Params.Debug = (DebugView)m_DebugViewIndex`。脚本化测试只设 `m_Params.Debug` 会被立刻覆盖回 `Final`（R7 曾因此拍出无效对照图）。**既有缺陷，R7 用「同时设两处」绕过，未修** | 记录，未修（R7 L8） |
| sRGB 采样语义缺失 | `LoadDDSFromFile(ddsFile)` 从不传 `sRGB` 参数，`textureOptions` 里的 `kSRGB` 位被丢弃 → **光栅与光追都是 linear 读取 sRGB 编码的贴图数据**。R7 的 D1 拍板「保持 linear」是为了两边可比，缺陷本身仍在 | 独立立项 |

---

## 8. 本轮沉淀的关键经验（务必保留）

1. **外部截图不能作为 GPU 画面证据**。实测 `PrintWindow` 抓两次完全不同的 Shader 输出，得到 md5 相同的 PNG。必须用 `ReadbackBuffer` 回读 —— 已封装为 `Engine/04_Renderer/DebugCapture.{h,cpp}`，支持 `R8G8B8A8_UNORM` / `R16G16B16A16_FLOAT`。

2. **区分「代码改了」和「改动生效了」**。生成头、PCH、缓存都可能让改动静默失效。验证方式是看构建日志里对应 `.obj` 是否重编。

3. **printf 打印自定义类型必须显式转 float**。`Vector3::GetX()` 返回 `Scalar`，直接传给可变参数会读出垃圾值（曾据此误判相机位置为 0）。写 `(float)p.GetX()`。

4. **生命周期改动必须实际跑起来验证**。sorter 加 `Reset()` 局部看是对的，与 `IsDirty` 门控结合后产生"只有第一帧有画面"。

5. **修复前先确认现象是真的**。本轮因截图和日志两处失真，浪费大量时间在不存在的问题上。

6. **R3：静态库里的文件级静态对象会被 dead-strip**。`RendererAutoRegister<T>` 从未运行 → 注册表恒为空。且**已链接的 exe 没有 COFF 符号表**，`dumpbin -symbols` 只会输出 18 行 section summary，看不出任何东西。可靠的取证方式是扫二进制里的 UTF-16LE 字面量（如 `Deferred Texture Descriptors` 从 0 变 1）。结论：**不要用静态初始化器做关键注册**，改在 `Instance()` 里显式注册。

7. **R3：清屏色用非黑值，是区分「没 draw」与「链路坏」的最快手段**。Forward 的清屏色 `Color(0.10,0.12,0.16)` 让 `r3_07_forward.png` 的「全幅同一色」一眼可判 = 清了屏但没画东西。

8. **R3：`-Screenshot` 的目录参数必须反斜杠**。正斜杠会让 `SaveToWICFile` 返回 `0x8007007B (ERROR_INVALID_NAME)`，日志里的路径还会被截断。

9. **R3：只在「渲染循环跑起来之后」才会暴露的状态机缺陷**。同一个 `ModelInstance`，启动时加载正常、运行时赋值却消失——因为 `operator=` 没置脏而 `Update` 有 `IsDirty` 门控。**任何「重建资源」的赋值/移动都要显式置脏**。

10. **R4：同一个输出目标的所有 Pass 必须共用同一条显示编码链**。修复前 `LightingPS` 有自己的 ACES，而 `SkyboxPS` 直接 `return float4(hdrColor)` —— 天空成了整幅画面里唯一绕过曝光/色调映射的区域，实测 52% 像素硬截断成纯白。**把编码链抽成 `ColorPipeline.hlsli` 供所有消费者引用**，是唯一能让「漏用」在代码结构上变难的做法。

11. **R4：`*_UNORM` 目标不会自动做 gamma 编码**。`R8G8B8A8_UNORM` 与 `R8G8B8A8_UNORM_SRGB` 是两件事；用前者就必须在 Shader 里显式做 sRGB 编码。本次 A/B 实测：不编码时模型区平均亮度 132.3，编码后 176.9 —— 差 34%，是肉眼可见的整体偏暗。

12. **R4：硬编码的资源属性上界会「看起来能用」**。`maxPrefilterMip = 8.0` 而立方体只有 5 个 mip，超界采样被硬件 clamp 到最后一个 mip —— 不报错、不崩溃，只是粗糙度响应在 0.6 之后被压平。**凡是「从资源派生」的常量，都应在资源创建后回填，而不是写在 Shader 里**。

13. **R4：死标志会掩盖语义错误**。`if (m_Baked || m_HDRLoaded || ...)` 里 `m_HDRLoaded` 从未置 true，于是「HDR 已加载就禁止烘焙」这个反了的语义永远不生效。**见到「某标志只被置 false 从不置 true」就该怀疑整条判断链**。

15. **R4.1：CMakePresets.json 不容忍任何多余字段**。根对象里一个 `_comment` 就会让 CMake 拒绝解析整个文件（`Invalid extra field`），VS 的 CMake 集成因此完全无法配置。注释必须写进各字段的 `description`。

16. **R4.1：VS 的 CMake 集成不继承用户 shell 的环境变量**。在 cmd 里 `set VCPKG_ROOT=...` 对 VS 无效。凡是"构建必需"的路径信息，要么写进 presets，要么做成仓库内可自发现的（如 `vcpkg_installed` + `CMAKE_PREFIX_PATH`）。

17. **R4.1：`add_custom_command(TARGET ...)` 要求命令与 target 在同一个 CMakeLists 目录**。跨目录集中管理会报 `TARGET 'xxx' was not created in this directory`。正确做法是：父目录定义**函数**，子目录创建 target 后自行调用。

18. **R4.1：多处 `vcpkg_installed` 副本必然分叉，且极难排查**。本轮 LNK2019 的真因是仓库根目录那份 DirectXMesh 缺少 `dx12` feature（缺少 D3D12 重载），而 `out/build/Debug/vcpkg_installed` 那份是新的、带 feature 的。**排查方法是直接 `grep` 两个 .lib 的目标符号**，比对 md5 与符号数比看链接命令有效得多。

19. **R4.1：验证"构建等价性"要用渲染输出比，而不是"能编译"**。VS 构建与脚本构建产出的 10 张截图 md5 逐字节相同，才能排除"能跑但画错"（例如 DLL 变体混用、宏不一致）。

20. **R4.2（最重要的一条）：GPU 回读正常 ≠ 能正常显示。** 回读只走 `Render()` 后的 SceneColor，**完全不经过** `PreparePresentSDR → CopyResource → Present`。上一轮我用回读图判定"通过"，结果窗口是黑的、点击就退出，而所有日志都正常 —— 因为真正的失败点 `Present skipped` 用的是 `LOG_TRACE`，被编译器整个删掉了。**渲染验收必须分两层：渲染层用 GPU 回读，呈现层必须看窗口是否真的可见 + Present 返回值。**

21. **R4.2：失败路径绝不能用 `LOG_TRACE`。** `Utility.h` 里非 `TOOIYUME_VERBOSE_LOG` 时它展开为 `do{}while(0)`。把"出错了就 return"的日志放进去，等于把这个 bug 从日志里抹掉。本次全黑问题排查 30 分钟里，绝大部分时间是被"日志没有报错"误导。

22. **R4.2：资源格式比较要区分「位布局」与「视图语义」。** 引擎把 ColorBuffer 创建成 `*_TYPELESS`（`PixelBuffer::DescribeTex2D` 里做 `GetBaseFormat` 归一化），再按需建具体视图。判断两个资源能否 `CopyResource`，要看**位布局是否同一家族**（`R8G8B8A8_TYPELESS`/`_UNORM`/`_UNORM_SRGB` 都是 4 字节同布局），而不是枚举值相等。用后者会得到"永远不匹配"，且**不报任何 D3D12 错误**。

23. **R4.2：黑屏要先用 GPU 回读切分责任。** 若回读正常 → 问题是呈现路径（Present/CopyResource/交换链）；若回读写出的图也是黑的 → 才是渲染管线问题。这一步能把排查范围砍掉一半，是本次最快见效的手段。

24. **R4.3：「闪退」先看退出码，别默认是崩溃。** 退出码 **0** = 程序自己决定退出（读 `while` 的判定条件）；非 0（如 0xC0000005）= 真崩溃（上调试器）。本次点击退出是前者，我先按崩溃方向查（cdb + `SetUnhandledExceptionFilter`）完全扑空，最后发现根因是 `IsDone()` 读到了一个假键位。**判断依据只有一条：退出码。**

25. **R4.3：DIK 扫描码 ≠ VK 虚拟键码，混用会撞出致命巧合。** DirectInput 用 DIK（物理位置），`GetAsyncKeyState` 用 VK（逻辑键）。`DIK_ESCAPE = 0x01` 而 `VK_LBUTTON = 0x01`——于是「按鼠标左键」被判成「按 Escape」，程序正常退出且**没有任何错误日志**。同一张错表还导致 WASD 全部错位（`DIK_W=0x11` vs `VK_W=0x57`），这就是历史遗留的「WASD 时灵时不灵」。**映射表的变量名必须体现它存的是哪套编码**（`s_DXKeyMapping` 存 DIK 却叫 DX，是埋雷点）。

26. **R4.3：模拟输入要选对层级。** `PostMessage(WM_LBUTTONDOWN)` 只走 WndProc/ImGui，**不经过** `GetAsyncKeyState`；`mouse_event`/`SetCursorPos` 是系统级注入，走完整硬件输入路径。验证输入相关缺陷时必须用后者——本次用 PostMessage 复现不出来，换 mouse_event 立刻复现。

27. **R4.6：断言不是错误处理，写之前先问"失败是否影响核心功能"。** `GpuTimer` 里两处 `ASSERT(false)`（回读 Map 失败、Begin 未配对 End）直接导致"拉伸窗口闪退"。性能统计拿不到样本属于**功能降级**，必须跳过这一帧而不是终止进程。判断标准：影响核心渲染/资源状态 → 断言合理；只影响诊断信息 → 必须降级。

28. **R4.6：D3D12 资源描述里的值，事后改成员变量是改不动的。** `D3D12_CLEAR_VALUE` 在 `CreateTextureResource` 时就固化到资源上；`ColorBuffer::Create` 之后调用 `SetClearColor` 只改 CPU 副本，于是 `ClearRenderTargetView` 每帧报 #820 并走慢路径。**凡是被固化进资源描述的参数，都必须作为创建参数传入。**

29. **R4.6：`ResizeBuffers` 有两个必须与创建时一致的项。** ① `SwapChainFlags`（如 `ALLOW_MODE_SWITCH`），不一致会返回 `DXGI_ERROR_DEVICE_REMOVED`；② Resize 之后**必须重新调用 `GetCurrentBackBufferIndex()`** —— 用旧索引会对错误状态的 buffer 发屏障，触发 `EXECUTION ERROR #232` 设备移除。后者尤其隐蔽：崩溃信息是 `DXGI_ERROR_ACCESS_DENIED`，看不出和索引有关。

30. **R4.6：容器的元素指针不能跨越修改操作。** `RenderFrameDesc.Models` 存 `ModelInstance*`，而 `vector` 扩容会搬移元素。所有改容器的操作（增删模型）必须集中到**帧边界**，渲染期间只读。另外 `ModelInstance` 无拷贝赋值（含 UploadBuffer），直接放 `vector` 值类型编译不过 —— 用 `unique_ptr` 持有即可，让编译期帮你守住这个约束。


14. **R4：A/B 取证要能证明缺陷存在，而不只是证明修复后好看**。AlphaTest 的验收特意做了一次「临时注释掉 `clip`」的对照构建：关闭时角色眼睛是实心黑块，开启后露出虹膜，差异只落在镂空 bbox（67 px）。**没有「缺陷态」的对照，就无法排除「本来就是这个样子」**。

31. **R5：`DispatchRays` 读的是计算根签名，不是图形根签名。** 用 `SetGraphicsRootSignature` 绑定时根参数**不生效**，且 D3D12 **不报任何错** —— 状态对象能建、SBT 能建、DispatchRays 返回 void、日志全绿，就是整屏纯黑。必须 `context.GetComputeContext()` 后走 `SetComputeRootSignature` / `SetComputeRootShaderResourceView` / `SetComputeRootDescriptorTable` / `SetComputeRoot32BitConstants`。官方 `D3D12RaytracingHelloWorld` 同理。

32. **R5：HLSL 默认 `column_major`，漏 `#pragma pack_matrix(row_major)` 会让 `float4x4` 等价转置。** 项目其余 shader 有 pragma 保护，`RayTracing.hlsl` 只 include 了纯函数头 `ColorPipeline.hlsli` 就漏了。现象：射线方向全错 → 全打空。**判据：整屏 `(26,38,69)` = miss 背景色 `(0.02,0.03,0.06)` 经 ACES+sRGB 的精确值，说明「RayGen 在跑、UAV 在写、射线全打空」；纯黑 `(0,0,0)` 才是「没派发」。**

33. **R5：`BuildRaytracingAccelerationStructure` / `DispatchRays` 不会自动 `FlushResourceBarriers()`。** `CommandContext::TransitionResource` 只把屏障攒进 `m_ResourceBarrierBuffer[16]`，`DrawInstanced` 内部会 flush，这两个 DXR 入口不会。漏了报 `EXECUTION ERROR #538 INVALID_SUBRESOURCE_STATE`（几何还在 `COPY_DEST`）。

34. **R5：加速结构资源必须创建时即为 AS 状态，之后永不发 ResourceBarrier。** 三个连环错误：① 事后 Transition 进 AS → `#526 RESOURCE_BARRIER_INVALID_COMBINATION`；② 用 `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`(0x100) → `#719 CREATERESOURCE_UNRECOGNIZEDLAYOUT`（本机 `d3d12.dll` 10.0.19041 不认，属 Agility SDK 1.7xx 后标志）；③ `RESOURCE_FLAG_NONE` + AS 初始状态 → `#524 MISMATCHING_MISC_FLAGS`。**正解：`Flags = ALLOW_UNORDERED_ACCESS` + `InitialState = RAYTRACING_ACCELERATION_STRUCTURE`。**

35. **R5：生成头漏挂依赖 = shader 修改静默失效。** `shader_attach_header_dependency` 原本只挂了 3 个 `.cpp`，漏了 `RayTracingRenderer.cpp`。改 HLSL → `.cso/.h` 更新 → `.cpp` 不重编 → exe 里是旧字节码。**前几轮 row_major 与调试输出全部无效**，排查很久的黑屏其实是调试着色器根本没进 exe。验证：`ls -la out/build/<Cfg>/Engine/CMakeFiles/TooiyumeEngine.dir/<path>.obj` 看时间戳。

36. **R5：`D3D12_RAYTRACING_INSTANCE_DESC.Transform` 是列向量约定，项目 `Matrix4` 是行向量约定。** 按行直拷 `Transform[i][j] = m[i][j]` 会丢平移（`m[i][3]` 对仿射矩阵恒 0）。正解 `Transform[i][j] = m[j][i]`、平移走 `Transform[i][3] = m[3][i]`。单模型平移为 0 时不显现，**多模型必现**。

37. **R5：`ModelLoader` 无条件 `Alloc` 全局纹理/采样器堆。** 任何 Renderer 的 `Initialize` 都必须建 `s_TextureHeap` / `s_SamplerHeap`，否则切过去后加载模型在空堆上 `Alloc` → `ASSERT(HasAvailableSpace)` → `STATUS_BREAKPOINT`（exit `-2147483645`）。**DXR 不用材质也必须建。**

38. **R5：`CommandContext::WriteBuffer` 要求源指针 16 字节对齐**（`ASSERT(Math::IsAligned(BufferData, 16))`），`std::vector::data()` 只保证 `alignof(T)`。每帧上传用 `UploadBuffer` + `Map` + `memcpy`，且上传缓冲必须是成员（局部变量会在命令列表提交前析构）。

39. **R5：`SetBufferSRV` 的 ASSERT 会挡住加速结构。** AS 永远不处于 `*_SHADER_RESOURCE` 状态 → 绑 TLAS 必须直接 `GetCommandList()->SetComputeRootShaderResourceView(...)`。

40. **R5：`DispatchRays` 的 `Width/Height/Depth` 必须每次派发前填。** `RebuildTopLevel` 会把 `D3D12_DISPATCH_RAYS_DESC` 清成 0；不填就是**派发 0 条射线** —— 不报错、不崩溃、日志照常、截图纯黑。

41. **R5：根级 SRV（`D3D12_ROOT_PARAMETER_TYPE_SRV`）不携带 `StructureByteStride`。** HLSL 侧声明 `StructuredBuffer<T>` 会按错误跨距读。**改 `ByteAddressBuffer` + 手算字节偏移。**

42. **R5：`D3D12_RAYTRACING_TIER_NONE` 不存在。** SDK 里是 `_NOT_SUPPORTED = 0` / `_1_0 = 10` / `_1_1 = 11` / `_1_2 = 12`。

43. **R5：`DescriptorHandle` 有 `operator D3D12_CPU_DESCRIPTOR_HANDLE()` 与 `GetCpuPtr()` 两条转换路径**，直接当函数实参传会 C2664 歧义 → 必须先赋给局部变量。

44. **R5：Windows 环境变量名大小写不敏感，但允许同时存在 `NO_PROXY` 与 `no_proxy`。** MSBuild 的 `ProcessStartInfo.EnvironmentVariables` 因此抛 `ArgumentException: 已添加了具有相同键的项`，`cmake -G "Visual Studio 17 2022"` 表现为 **`No CMAKE_CXX_COMPILER could be found`**（误导性极强，真因只在 `CMakeFiles/CMakeConfigureLog.yaml` 里）。`Get-ChildItem env:` 本身也会抛同样错，要用 `[System.Environment]::GetEnvironmentVariables()`。

45. **R5：`lib_6_x` 与 `*_6_x` 的唯一区别是不传 `-E`。** 一份 `.hlsl` 里所有 `[shader("...")]` 入口编进同一 DXIL 库，由 State Object 的 `DXIL_LIBRARY` 子对象按导出名挑选。

46. **R5-VS：`CMAKE_BUILD_TYPE` 在多配置生成器下是「配置期的固定单值」，不是「当前构建的配置」。** `DeployRuntimeDlls.cmake` 用它选 DLL 变体，在 VS 生成器下判断恒真 → **Release 构建把 `debug/bin` 的调试版 DLL 拷过去**（实测 md5 与 `debug/bin` 相同）。正解：`$<IF:$<CONFIG:Debug>,...debug/bin,...bin>` 生成器表达式，单配置/多配置都正确。**凡是用 `CMAKE_BUILD_TYPE` 做「按配置分支」的逻辑，在 VS 生成器下都要重新审视。**

47. **R5-VS：`cmake -G "Visual Studio 17 2022"` 报「找不到编译器」时，先怀疑 proxy 环境变量。** 见第 44 条。**误判成本极高** —— 会去查 VS 安装、Windows SDK、工具集，实际是环境变量大小写重复。诊断入口：`out/build/<dir>/CMakeFiles/CMakeConfigureLog.yaml` 里的 `MSB6001` + `ArgumentException`。

48. **R5-VS：`unset` 在 bash 里对 MSBuild 无效。** bash 只改自己进程的环境；`cmake --build` 派生的 MSBuild 继承的是**调用 bash 的那个父进程**（DSH 宿主）的环境。必须在同一个 PowerShell 进程里用 `[System.Environment]::SetEnvironmentVariable($n,$null,'Process')` 清掉，再调 MSBuild/cmake。

49. **R5-VS：`add_custom_command(TARGET ... POST_BUILD)` 只在 target 真的重新链接时才跑。** 改了 `DeployRuntimeDlls.cmake` 的 DLL 路径后，如果 `.exe` 本身没变，POST_BUILD 不会执行、旧 DLL 留在原地。验证修复必须**手动删掉产物 DLL 或 touch 一个源文件强制重链接**。

50. **R5-VS：「构建等价性」要拿渲染输出比，不是拿「能编译、能跑」比。** VS 树与 Ninja 树的 DXR 截图 md5 逐字节相同（`1bea89b2…`）、Deferred 相同（`16c8aeec…`）、切换链路相同 —— 这才排除了「能跑但画错」（例如 DLL 变体混用、宏不一致、优化等级差异）。

51. **R5-VS：`--config` 与 `CMAKE_BUILD_TYPE` 是两套东西。** `cmake --build out/build/vs2022 --config Release` 里的 `--config` 是多配置生成器的配置选择；`CMAKE_BUILD_TYPE` 是配置期缓存变量。两者可以「不一致」（本项目就是 `CMAKE_BUILD_TYPE=Debug` + 跑 `--config Release`）。**不要指望它们联动。**

52. **R6：`msvc_deps_prefix` 双重编码会让头文件依赖**全部**失效，且完全静默。** 症状不是编译报错，而是「改了结构体字段，运行时的值还是旧的/是 0」。诊断：`ninja -C <dir> -t deps <某个>.obj` 看 `#deps` 行数。根因：`cl.exe /showIncludes` 按**控制台代码页**输出提示行，CMake 在 configure 期抓这段前缀写进 `rules.ninja` 的 `msvc_deps_prefix`，Ninja 之后用这个字节串去 grep cl 的输出；当系统 ACP（本机 936=GBK）与构建时会话代码页（65001=UTF-8）不一致时，前缀被双重编码成乱码（「注意: 包含文件: 」→ `娉ㄦ剰: 鍖呭惈鏂囦欢:`）→ 一行都匹配不到 → **所有 .obj 的 `#deps` 变成 0**。正解：`build.bat` 里 `chcp 65001` 并**同时包住 configure 与 build**（cl 是在 build 期打印前缀的），另加代码页哨兵文件强制旧树重配（`CMakeCXXCompiler.cmake` 只有 clean configure 才重生成）。**单点改 `rules.ninja` 无效**，下次 configure 又被覆盖。

53. **R6：`DispatchRays` 的阴影射线必须用 `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` + 独立 `ShadowPayload` + SBT 里第二条 miss 记录。** 命中着色器签名按**载荷类型**绑定：只加第二条 miss 记录但让阴影射线走主 hit group，主 `ClosestHit` 会按 `RayPayload`（32 B）解释只传了 `ShadowPayload`（8 B）的载荷 → 越界写坏内存（可能只是画面乱，也可能崩）。加了 `SKIP_CLOSEST_HIT_SHADER` 后语义变成「命中 → 不调用任何着色器、载荷保持初值 0 = 被遮挡；只有未命中才调 `ShadowMiss` 置 1」，一个 hit group 就够。

54. **R6：`MaxTraceRecursionDepth` 越界不报错，设备直接把进程移除（`DEVICE_REMOVED 0x887A0005`）。** 所以递归闸门必须写在 `ClosestHit` 的**最前面**（几何寻址后立即 `if (payload.Depth >= 1u) return;`），而不是「算完再判断要不要递归」。主射线 `Depth=0`、反射射线 `Depth=1`、上限设 2 刚好够。

55. **R6：同一根常量块要改时，四处必须同步。** ① HLSL 的 `cbuffer` 字段数；② C++ 的 `static_assert(sizeof(...) == N)`；③ 根签名 `InitAsConstants(0, N_DWORD)`；④ 提交时的 `SetConstantArray(..., N_DWORD, ...)`。漏改任意一处 → **后续字段整体错位、画面异常但完全不报错**（因为根常量本质是裸字节）。本轮 32→48 DWORD 就是这条。

56. **R6：光追的偏差（bias）只留一个旋钮，是为了可归因。** 阴影与反射共用同一个沿法线的 `RTShadowNormalBias`；反射射线的 `TMin` 也用它（从 0 起算会立刻命中自己所在三角形 → 全场条纹）。同时给「法线偏移 + 光线偏移」两个参数，出问题时分不清是谁的锅。**先证明链路通，再谈旋钮数量。**

57. **R6：`SunDirection` 存的是「光的传播方向」，不是「指向光源」。** 光栅侧（`LightingPS.hlsl:118`）与光追侧（`toLight = normalize(-g_LightDirection.xyz)`）都必须取反。`RenderParams.h:57-60` 有注释警告。**两侧任一处漏取反 → 主光高低翻转，且不报错。**

58. **R6：ImGui 的 `imgui.ini` 会覆盖 `ImGuiCond_FirstUseEver`。** 改了 `SetNextWindowPos/SetNextWindowSize` 但画面没动 → 先删可执行文件目录下的 `imgui.ini`。另外 **ImGui 滑块标签画在控件右侧**，面板靠屏幕右缘时标签会被裁 → 用 `PushItemWidth()` 收窄控件。

59. **R6：A/B 验证要挑「md5 必须相等」的对照组，而不只是「应该不同」的实验组。** 本轮最有说服力的三条里，两条是等价关系：`反射强度=0` 与 `反射关` 像素级等价（`a43b6059…`）、11 步来回切参数后 `r6_10_restored` 与基线完全相同。**「能改」只能证明参数传到了 shader，「能还原」才证明没有隐藏状态残留。**
60. **R7：`ByteAddressBuffer::Load<T>` 是「按 T 的字节宽度裸位拷贝」，不做任何格式转换。** 想读 4 字节的 `R16G16_FLOAT` UV 却写 `Load<float2>`（8 字节）→ 把**下一个属性的 4 字节**拼进来当第二个 float 解释 → uv 变成 ~1e-38 的 denormal（≈`(0,0)`）。后果：整张贴图退化成「每个材质一片纯色」，布料褶皱与脸上画好的眼睛全部消失，**但不报错、不崩、日志正常、截图看起来"确实有颜色"**。正确做法与 `LoadIndex` 处理 16 位索引一致：`Load<uint>` → 拆半字 → `f16tof32`。**这类"通了一半"的错比全黑更难查**，因为全黑会立刻怀疑；"有颜色"会被当成已完成。**本轮花了最多时间的就是这一个 bug。**
61. **R7：`DescriptorHeap::GetHeapPointer()` 返回 `ID3D12DescriptorHeap*`，不是 GPU 描述符句柄。** 写成 `(D3D12_GPU_DESCRIPTOR_HANDLE)heap.GetHeapPointer()` **编译通过但值完全错** → 采样越界 → 全黑。必须用 `heap[offset]`（`operator[]` 才返回正确的 `DescriptorHandle`）。凡是"编过了但画面全黑"的，优先怀疑这类句柄强转。
62. **R7：D3D12 一个命令列表同时只能绑一个 CBV_SRV_UAV 堆 + 一个 SAMPLER 堆。** 所以「输出 UAV 用一张堆、贴图 SRV 用另一张堆」是不可能的 —— 必须合并。合并时要注意**分配顺序**：输出 UAV 必须在加载模型前占住 0 号槽，否则会被第一个材质的 BaseColor 抢走。这条约束是「先有 R5 的独立小堆、R7 才要接贴图」时最容易漏掉的架构债。
63. **R7：`VisibilitySmithGGXCorrelated` 返回值里**已经含 `1/(4·nDotV·nDotL)`。再乘一遍分母会让高光暗 4 倍以上。同理，Lambert 漫反射必须 `/π`（`albedo/π`），漏了会亮 3.14 倍 —— R6 的 `albedo * radiance * nDotL` 就是漏了 `/π`，这也是 A5a 与 A5b/A5c 亮度差的一部分原因。
64. **R7：调试通道必须在显示编码链（曝光/ACES/sRGB）之前 return。** 光栅侧 `LightingPS.hlsl:188-216` 一直如此（注释写明"要显示的是缓冲区的原始数值"），但 DXR 侧原先无条件过 `EncodeDisplayColor` → **同一个通道名在两条路径下画出的不是同一个东西**，A7 对照因此完全无效（比出来 `mean|diff| 67.2`，修完是 `2.9`）。**做跨路径比对前，先确认两边比的是同一个量。**
65. **R7：`DrawDebugViewPanel()` 每帧无条件覆盖 `m_Params.Debug`。** 脚本化测试里只设 `m_Params.Debug = BaseColor` 会被下一帧 UI 覆盖回 `Final`。必须**同时**设 `m_DebugViewIndex` 与 `m_Params.Debug`。**凡是"UI 每帧重算"的参数，脚本化测试都要考虑覆盖问题** —— 这是自动化测试与"人点 UI"最大的差异。
66. **R7：脚本化验收必须显式指定 `-StartRenderer`。** 忘了传 → 第 0–4 步全在 Deferred 下跑，5 张 PNG 全是同一个 Deferred 画面（1734511 B）。**日志里每步都打了 `renderer=...`，只要看一眼就能发现** —— 加这类"回声"日志的成本极低，收益极高。
67. **R7：GPU-Based Validation 开启时加载 1 个模型约需 40 秒。** 35 秒杀进程会误判成"崩溃"（日志停在初始化完成）。正确等待 ≥ 75 秒。**判"崩没崩"要看 cdb 有没有 `sxe 87a` / `sxe av` 触发，不要看"日志停在哪"。**
68. **R7：材质标量必须烘进 `ByteAddressBuffer`，不能指望 ray shader 读 GPU 常量缓冲。** ray shader 只能解引用描述符（CBV/SRV）或根级句柄；`Model::m_MaterialConstants` 的 GPU 地址是运行时 `uint64` 值，HLSL 无法把它当地址解引用。代价：每实例 96 B（+64 B × 15 实例 = 960 B，可忽略）。**这也正是"方案 B（SBT 本地根签名）"存在的理由** —— 它能把材质放进常量而不是每实例数据里。

---

## 9. 构建与运行

### 9.1 三条入口（均可独立工作）

| 入口 | 命令 | 适用场景 | 是否需 VCPKG_ROOT |
|---|---|---|---|
| **Visual Studio** | 打开文件夹 → 选 `x64-debug` → 构建 | 日常开发/调试 | **否**（用仓库内 `vcpkg_installed`） |
| CMake presets | `cmake --preset x64-debug` + `--build --preset build-debug` | 命令行/CI | 否 |
| 项目脚本 | `scripts\build.bat Debug` | 与历史一致 | 是 |

> **R4.1 变更**：VS 里不再需要设环境变量。根 `CMakeLists.txt` 在拿不到 toolchain 时
> 自动使用仓库内 `vcpkg_installed/x64-windows`。
> 详见 `docs/AI/R4/R4.1_VisualStudio构建修复.md`。

```bat
rem ---- 方式 A：Visual Studio（推荐，无需环境变量）----
rem 在 VS 中"打开文件夹" → CMake 配置下拉选 x64-debug → 生成

rem ---- 方式 B：CMake presets（命令行，无需环境变量）----
cmake --list-presets
cmake --preset x64-debug
cmake --build --preset build-debug

rem ---- 方式 C：项目脚本（需 VCPKG_ROOT）----
set VCPKG_ROOT=F:\VS\rep\vcpkg
scripts\build.bat Debug clean
scripts\build.bat Release clean

rem 运行
scripts\run.bat Debug Deferred
scripts\run.bat Debug Forward
scripts\run.bat Debug RayTracing
scripts\run.bat Debug RendererDemo        rem R3 统一入口（ImGui 六面板）

rem 强制重建模型缓存（改了纹理槽顺序后必须）
scripts\run.bat Debug Deferred -RebuildModels

rem GPU 回读截帧（目录用反斜杠，且必须已存在）
DeferredApp.exe -Screenshot D:\shots

rem R3 验收自测（11 步，自动退出）
scripts\run.bat Debug RendererDemo -R3Test D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender\docs\AI\R3

rem R6 验收自测（12 步，硬阴影/反射 A/B）
scripts\run.bat Debug RendererDemo -StartRenderer RayTracingRenderer -R6Test D:\...\docs\AI\R6test

rem R7 验收自测（11 步，贴图/GGX 2x2）
rem   *** 必须与 -StartRenderer RayTracingRenderer 同用 ***
rem   否则第 0-4 步全在 Deferred 下跑，5 张 PNG 全是同一个 Deferred 画面
scripts\run.bat Debug RendererDemo -StartRenderer RayTracingRenderer -R7Test D:\...\docs\AI\R7test2
```

> **R7 运行注意**：开启 GPU-Based Validation 时加载 1 个模型约需 **40 秒**，
> 截图脚本的等待时间必须 ≥ 75 秒，否则会误判成"崩溃"（日志停在初始化完成）。
> 判"崩没崩"看 cdb 的 `sxe 87a` / `sxe av` 有没有触发，不要看"日志停在哪"。

---

## 10. 关键文件速查

| 文件 | 职责 | 相关阶段 |
|---|---|---|
| `Engine/00_Core/Utility/AssetRoot.*` | 资源根解析 | **新增**（R0） |
| `Engine/00_Core/Utility/Utility.h` | 日志落盘、`LOG_INFO`/`LOG_TRACE`、Release `ASSERT` | R0 |
| `Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h` | 材质/全局常量权威定义 | R1（槽位 + 对齐） |
| `Engine/05_Scene/Model/ModelLoader.cpp` | 模型加载、默认纹理表 | R1 |
| `Engine/03_AssetSystem/Importers/Model/ModelConvert.cpp` | glTF → .ty 烘焙 | R1 |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp` | Deferred 主链 | R1（sorter、分辨率） |
| `Engine/Shader/02_Deferred/LightingPS.hlsl` | 光照合成 | R1（分辨率、CB 契约） |
| `Engine/04_Renderer/DebugCapture.*` | GPU 回读截帧 | **新增**（R1） |
| `Engine/20_cmake/ShaderCompiler.cmake` | Shader 编译 + 依赖追踪 | R0/R1 |
| `Engine/CMakeLists.txt` | 构建配置 | R0/R1/R3 |
| `Demos/RendererDemoApp/RendererDemoApp.cpp` | R3 统一入口：Registry 切换 + 六 ImGui 面板 + RenderParams | **新增**（R3） |
| `Engine/04_Renderer/Renderer/Base/RendererRegistry.cpp` | 渲染器注册表（显式注册，规避 dead-strip） | R3 |
| `Engine/04_Renderer/ImGuiLayer.{h,cpp}` | ImGui Win32/DX12 后端（自建 SRV 堆） | **新增**（R3） |
| `Engine/04_Renderer/GpuTimer.{h,cpp}` | GPU 时间戳查询（TIMESTAMP + 回读 + 围栏） | **新增**（R3） |
| `Engine/04_Renderer/RenderParams.h` | 渲染参数唯一真源 | **新增**（R3） |
| `Engine/05_Scene/Lighting/LightingSystem.cpp` | 参数打包/上传（`PackParams` / `ApplyParams`） | R3 |
| `ThirdParty/imgui/` | Dear ImGui 1.91.9b（含 backends） | **新增**（R3） |
| `Engine/Shader/02_Deferred/ColorPipeline.hlsli` | **显示编码链唯一实现**（Exposure→ACES→sRGB），Lighting 与 Skybox 共用 | **新增**（R4） |
| `Engine/Shader/02_Deferred/GBufferPS.hlsl` | GBuffer 写入 + MASK AlphaTest `clip` | R4 |
| `Engine/04_Renderer/EnvironmentLightingManager.cpp` | IBL 烘焙 + Skybox（接收显示参数）+ 状态机 | R4 |
| `docs/AI/R4/R4_光栅PBR闭环.md` | R4 技术文档（架构/数据流/学习路线/回忆题） | **新增**（R4） |
| `CMakePresets.json` | VS/命令行 presets（R4.1 修复：删除非法字段） | R4.1 |
| `Engine/20_cmake/DeployRuntimeDlls.cmake` | 运行时 DLL 统一部署函数 | **新增**（R4.1） |
| `docs/AI/R4/R4.1_VisualStudio构建修复.md` | VS 构建问题诊断与修复记录 | **新增**（R4.1） |
| `Engine/Shader/07_RayTracing/RayTracing.hlsl` | **DXR 着色器库**（RayGen/Miss/ClosestHit），`lib_6_4` 编译，含 `#pragma pack_matrix(row_major)` | **新增**（R5） |
| `Engine/02_RHI/Pipeline/RaytracingPSO.{h,cpp}` | **RTPSO 独立封装**（State Object + Shader Identifier），不复用 GraphicsPSO 缓存 | **新增**（R5） |
| `Engine/04_Renderer/Renderer/RayTracing/RaytracingScene.{h,cpp}` | **BLAS/TLAS/SBT/每实例数据** 的构建与生命周期 | **新增**（R5） |
| `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.{h,cpp}` | **DXR Renderer**（DispatchRays + 输出缓冲 + 根签名） | **新增**（R5） |
| `Engine/02_RHI/GraphicsCore.cpp:180-235` | **DXR 能力查询**（与 App 请求解耦）+ `QueryRaytracingCapability()` | R5（D0） |
| `docs/AIWorkLogs/TR-20261027-01_R5-DXR最小闭环.md` | R5 施工日志（含 17 条踩坑教训） | **新增**（R5） |
| `docs/AIWorkLogs/TR-20261027-02_R5-VS环境验证.md` | VS 环境排错与验证日志（含 6 条环境坑） | **新增**（R5） |
| `docs/AI/R5/screenshot_90.png` | R5 DXR 画面证据（GPU 回读，法线色模型） | **新增**（R5） |
| `out/build/vs2022/` | VS 2022 生成器树（Debug+Release 全目标构建成功，MSBuild 直构 `.sln` 通过） | **新增**（R5） |
| `Engine/20_cmake/DeployRuntimeDlls.cmake:34-55` | DLL 变体选择改 `$<CONFIG>` 生成器表达式（修多配置 bug） | R5-VS 修复 |
| `.vs/launch.vs.json` | VS 调试启动配置（**R6 精简为 4 条**：DeferredApp / TooiyumeApp(前向) / RayTracingApp / RendererDemoApp） | R6 更新（gitignored） |
| `docs/AI/R5final/` | 四棵树构建等价性证据（`ninja_dbg`/`ninja_rel`/`vs_dbg`/`vs_rel`/`vs_rel_r`/`vs_switch`） | **新增**（R5） |
| `Engine/Shader/07_RayTracing/RayTracing.hlsl` | **R6 重写**：硬阴影射线（`ShadowPayload`+`ShadowMiss`+`SKIP_CLOSEST_HIT_SHADER`）、单次反射（Schlick Fresnel）、`SkyColor` 梯度天空、递归闸门、4 条调试通道 | R6（D3） |
| `Engine/04_Renderer/Renderer/RayTracing/RaytracingScene.cpp:597-680` | `BuildShaderBindingTable` miss 表 1→2 条（`kShadowMissOffset=96` / `kHitGroupOffset=128`） | R6 |
| `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.cpp` | 根常量 32→48 DWORD、`exports=4`、`payload=32`、`depth=2`、`RayTracingFrameStats` 统计 | R6 |
| `Engine/04_Renderer/RenderParams.h` | `RayTracingDebugView` 枚举 + 6 个 RT 字段（阴影/反射开关、偏差、距离、强度、调试通道） | R6 |
| `Demos/RendererDemoApp/RendererDemoApp.cpp` | `RunR6Test()`（`-R6Test` 12 步 A/B）+ `DrawRayTracingPanel()` + 面板重排 | R6 |
| `scripts/build.bat` | **代码页统一 + 依赖自检**（修 `msvc_deps_prefix` 双重编码 → `#deps 0`） | R6（独立缺陷修复） |
| `docs/AIWorkLogs/TR-20261027-03_R6-光追算法.md` | R6 施工日志（13 节 + 8 条踩坑） | **新增**（R6） |
| `docs/AI/R6test/` | 12 步 A/B 证据（Debug） | **新增**（R6） |
| `docs/AI/R6test_rel/` | Release 版 `-R6Test`：11 张渲染图 md5 与 Debug 相同（已去重只留 README + 唯一不同的 `r6_11_imgui_panel.png`） | **新增**（R6） |
| `docs/AI/R6ctl/` | 光栅对照（`16c8aeec…`，与 R5ctl 同 md5） | **新增**（R6） |
| `docs/AI/R6switch/` | `-R3Test` 切换链路证据（`r3_00`~`r3_06` + README；`r3_07`/`r3_08` 与 R5 同 md5 故去重） | **新增**（R6） |
| `Engine/Shader/07_RayTracing/RayTracing.hlsl` | **R7 再改**：`Texture2D<float4> g_Textures[] : register(t3)`（无界）、`SamplerState g_MaterialSampler : register(s0)`、`LoadVertexUv`（**手拆半字 + `f16tof32`**，不能用 `Load<float2>`）、`LoadInstanceAlbedo`、GGX 三函数、`ClosestHit` 双分支、`bypassDisplay` | R7（D4） |
| `Engine/04_Renderer/Renderer/RayTracing/RaytracingScene.h:60-115` | `RaytracingInstanceData` **32 B → 96 B**（+UV 偏移/顶点标志/材质索引/SRV 表偏移/5 个材质标量）；`kTexturesPerMaterial=5` / `kMaxMaterialEntries=512` | R7 |
| `Engine/04_Renderer/Renderer/RayTracing/RaytracingScene.cpp:300-360` | 填新字段（UV 偏移推导 `12+4+(hasTangent?4:0)`；用 `mesh.materialSlotIdx` 查 `m_Materials[]`，因为 `mesh.srvTable` 是死字段） | R7 |
| `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.cpp:288` | 根签名 6 参数 + `InitAsDescriptorRange(SRV, 3, UINT_MAX)` + `InitStaticSampler(0)`；b0 48→52 DWORD | R7 |
| `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.cpp:120-130` | **输出 UAV 与贴图 SRV 合并进同一张 `s_TextureHeap`**（D3D12 一个命令列表只能绑一个 CBV_SRV_UAV 堆），0 号槽 = 输出 UAV | R7 |
| `Engine/04_Renderer/RenderParams.h` | 两个**正交开关** `EnableMaterialTextures` / `EnableGGX` + `RayTracingDebugView::BaseColor = 5` | R7 |
| `Engine/05_Scene/Model/Model.h` | 新增 `m_MaterialConstantData`（材质 CPU 副本，建 BLAS 时烘进每实例数据） | R7 |
| `Demos/RendererDemoApp/RendererDemoApp.cpp` | `RunR7Test()`（`-R7Test` 11 步 2×2 验收）+ 面板两个 Checkbox + 6 项调试通道 | R7 |
| `docs/AI/R7_材质接驳_设计方案.md` | R7 设计方案（含拍板记录 D1-D6、三条硬约束、方案 A/B/C 对比） | **新增**（R7） |
| `docs/AIWorkLogs/TR-20261028-01_R7-材质接驳.md` | R7 施工日志（13 节 + 9 条踩坑） | **新增**（R7） |
| `docs/AI/R7test2/` | 11 步 2×2 验收证据（A5a/A5b/A5c/A5d/A7 对照） | **新增**（R7） |
| `docs/AI/R7test_rel/` | Release 版 `-R7Test`：9 张渲染图 md5 与 Debug 相同（已去重只留 README + 唯一不同的 `r7_09_imgui_panel.png`） | **新增**（R7） |
| `docs/AI/R7/`、`docs/AI/R7ctl/` | DXR 最终着色 / 光栅对照（各 1 张，已按 md5 去重） | **新增**（R7） |
| `docs/TooiyumeRender_光追管线教程.md` | **面向初学者的光追管线「设计 + 实现」教程**（14 章 / 约 2490 行 / 77 小节）：观念转换 → DXR 六个核心概念 → 架构分层 → R5/R6/R7 逐阶段实现 → **陷阱大全（按症状索引 12 条零报错坑）** → 取舍记录 → 6 个动手练习 → 源码地图（含 22 行关键行号速查表）。`路径:行号` 全部相对仓库根、已逐条核对 | **新增**（R7 后补） |

---

## 11. 新会话如何恢复

1. 读本文件 `docs/AI/R_执行表.md`
1b. **想系统理解光追管线本身**（而不是只想接着施工）：读 `docs/TooiyumeRender_光追管线教程.md` —— 第 1–3 章建立世界观与架构，第 4–6 章按 R5/R6/R7 顺序讲实现，第 7 章按「症状」索引 12 条零报错陷阱，第 9 章 6 个动手练习，第 10 章源码阅读顺序 + 关键行号速查表
2. 需要细节时再读对应阶段文档：`R0_*.md` / `R1_*.md` / `R2/R2_生命周期与窗口稳定性.md` / `R3/R3_统一入口与交互工作台.md`
3. **R5、VS 环境验证、R6（D3 光追算法）、R7（D4 光追接材质）均已完成**（R5/R6 于 2026-10-27，R7 于 2026-10-28）。R6 细节见 §6e（日志 `docs/AIWorkLogs/TR-20261027-03_R6-光追算法.md`）；**R7 细节见 §6f**（日志 `docs/AIWorkLogs/TR-20261028-01_R7-材质接驳.md`、方案 `docs/AI/R7_材质接驳_设计方案.md`）。**接手前必看两条**：① `scripts/build.bat` 的代码页修复（否则所有「改了代码没生效」都会被误判成代码 bug）；② `ByteAddressBuffer::Load<T>` 不做格式转换（R7 因此把 UV 读成 denormal，整张贴图退化成纯色且不报错）。VS 侧：`out/build/vs2022`（VS 2022 生成器）Debug+Release 双配置构建通过、MSBuild 直构 `.sln` 通过、运行通过、画面与 Ninja 版逐字节一致、Debug Layer 零 ERROR。详见 §6d
4. 施工日志模板：`docs/AIWorkLogs/_TEMPLATE_WP_施工日志.md`；最近示例：`docs/AIWorkLogs/TR-20261027-01_R5-DXR最小闭环.md`（含 17 条 R5 教训）、`docs/AIWorkLogs/TR-20261027-02_R5-VS环境验证.md`（含 VS 环境 6 条教训）、`docs/AIWorkLogs/TR-20261028-01_R7-材质接驳.md`（含 9 条 R7 踩坑）
5. 注意：**不要重复踩过的坑**（见 §8）。R5 新增第 31–45 条（其中「DispatchRays 用计算根签名」「HLSL 缺 row_major」「shader 头依赖漏挂」三条各花了数小时）；R6 新增第 52–59 条（**第 52 条 `msvc_deps_prefix` 最危险 —— 它让所有头文件改动静默失效**）；**R7 新增第 60–68 条**（**第 60 条 `Load<float2>` 读 4 字节 UV 是本仓最隐蔽的 bug —— 画面"有颜色"却完全错，且不报任何错**）。
6. **R7 之后的可开工项**：DXR IBL/点光源（§7 表「DXR 最终着色偏暗」，观感差距最大的一项）、alpha test（`AnyHit` + `IgnoreHit()`）、MR/Normal/AO/Emissive 贴图、多模型 DXR 验证（`baseVertex` 语义复核）、Soft Shadow。