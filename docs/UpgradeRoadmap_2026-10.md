# ToiyumeRender 未来功能升级路线图（基于仓库现状）

> 评估对象：`wow425/ToiyumeRender` 的公开默认分支 `master`。
> 原则：区分“代码结构/接口已存在”“调用路径存在”“功能经运行验证”。下文不把 TODO、PIX 标记或 Feature mask 自动视为功能已实现。

## 0. 当前基线（仓库代码检查）

### 已存在的工程基础
- C++20、CMake、vcpkg、DXC 预编译 Shader 流程。
- DX12 GPU 基础设施：Command Context/List、Fence/Allocator 管理、资源与 Descriptor、PSO/Root Signature。
- 应用和渲染器结构：ForwardApp、DeferredApp、BaseRenderer、RendererRegistry、PipelineDesc/PSO Cache。
- 场景模型/材质导入与管理、相机、光照系统。
- Deferred 的 GBuffer 与 Lighting 路径、环境光照管理器、TAA 相关模块。
- 有测试模型资产（如 Saber、Kyomachiya、saberCaster）。

### 按代码路径观察到的功能状态
| 子系统 | 目前判断 | 依据/备注 |
|---|---|---|
| Forward | 有实际绘制路径：清理颜色/深度、绑定全局状态、按 MeshSorter 绘制 | `ForwardRenderer.cpp` |
| Deferred GBuffer | 有实质绘制路径和 GBuffer 写入 | `DeferredRenderer.cpp` |
| Deferred Lighting | 有实质全屏 Lighting Draw，绑定 GBuffer、深度、IBL 资源和灯光/相机常量 | `DeferredRenderer.cpp` |
| IBL | 已写 Compute Pass 调用骨架与资源状态转换，但关键 Shader/算法是否正确必须逐个验证；存在多处 TODO 注释 | `EnvironmentLightingManager.cpp` |
| Skybox | 有绑定与 Draw(3,0) 路径，但函数注释仍列出待实现事项，需验证实际画面 | `EnvironmentLightingManager.cpp` |
| TAA | 有 TemporalEffects 接口与 Compute PSO 等代码，但是否完整接入最终渲染链路需要验证；Deferred 的 TAA Pass 当前为空壳 | `TemporalEffects.cpp`, `DeferredRenderer.cpp` |
| Deferred SSAO / Shadow / Transparent / PostProcess / Tonemap | 渲染函数里有 Pass 标记/占位块；不能视为已完成 | `DeferredRenderer.cpp` |
| Forward Shadow | 有 PIX BeginEvent，但从该代码段看还没有对应绘制/状态/资源流程 | `ForwardRenderer.cpp` |
| Path Tracing / DXR | Roadmap 中规划了 PathTracing，但当前 CMake 显式添加的应用仅 ForwardApp / DeferredApp；仓库代码搜索未发现 DispatchRays / PathTracing 的实现命中 | 需进一步针对分支/未索引文件检验 |
| RenderGraph | 架构文档提及为后续目标；目前所检目录未显示可用的 RenderGraph 实现 | 当前应视作未来架构任务 |
| Dear ImGui | 在默认分支代码搜索未发现命中 | 作为待集成项 |

## 1. 总目标

将项目从“可运行的 DX12 学习型渲染器”演进为：
1. 多条渲染管线共享资源、材质、相机和场景；
2. 每项效果具有可切换参数、Debug View 和验证场景；
3. 渲染 Pass、资源读写、Barrier 与 GPU 同步关系清晰；
4. 可以通过 GPU Profiling 比较算法正确性与性能；
5. 后续可用插件式方式接入 DXR Path Tracing 和神经渲染。

## 2. 路线图（按依赖顺序）

### R0 — 修复与可重复验证（P0）
**目标：建立可靠基线。**
- 修复构建、Shader 路径、资源路径、材质映射和资源生命周期问题。
- 移除硬编码绝对路径，使用统一 AssetRoot/配置文件。
- 固定测试场景：基础几何、Saber、Kyomachiya；固定相机、光照、分辨率。
- 建立构建与启动说明，确保干净构建能编译并运行。
- 打开 D3D12 Debug Layer；开发阶段检查 GPU Validation、资源状态和对象生命周期。

**验收：** 每个 Demo 从干净构建可启动；指定测试场景能稳定显示；Resize/最小化/还原/关闭窗口不崩溃；输出测试日志和截图。

### R1 — 共享核心与三管线生命周期（P0）
**目标：让三条管线通过统一生命周期接入，而不是各自成为孤立 Demo。**
- 明确三条管线的真实名称与状态。已确认实现类包括 ForwardRenderer、DeferredRenderer；Path Tracing 目前按规划中的未来管线处理。
- 完善 RendererRegistry：创建、初始化、Resize、切换、Shutdown 的调用路径。
- 统一 Scene、Camera、ModelInstance、Material、Light 与 RenderFrameDesc。
- 确认所有 Renderer 的 GetFeatures() 返回值与实际功能相符；当前 ForwardRenderer 的 Feature mask 返回 Deferred，需检查并修正。
- 修正 RendererRegistry API 的 const/name 使用、重复注册和失败反馈等边界。
- 资源释放前等待相关 GPU Fence；切换管线期间正确销毁和重建管线专属资源。

**验收：** Forward/Deferred 可通过统一入口切换；新增 Renderer 不需复制应用主循环；每条管线的能力清单与实际功能一致。

### R2 — Dear ImGui 实时工具层（P0/P1）
**目标：先提供观测和调参工具，避免盲目堆特效。**
- 使用稳定的 Dear ImGui DX12 + Win32 backend（版本固定并纳入依赖管理）。
- 面板分区：Renderer、Scene/Model、Camera、Material、Lighting、Debug、Performance。
- 提供管线选择、模型选择、渲染分辨率、VSync、FPS/帧时间、CPU/GPU 时间。
- 实时编辑 BaseColor、Metallic、Roughness、Normal/AO 贴图开关、曝光与灯光强度。
- Debug View：BaseColor、Normal、Depth、Metallic、Roughness、AO、Velocity、GBuffer 各通道。
- 为未实现效果显示 Disabled/Not Implemented；避免 UI 看起来能操作但实际不生效。
- 调整 UI 与 SceneColor/backbuffer/Present 顺序，确保 ImGui 不被后续复制覆盖；处理 Resize、字体纹理及 Descriptor 生命周期。

**验收：** 操作每个控件都有可观察结果；打开/关闭 UI 不破坏画面；FPS 计算正确；关闭程序时 DX12 资源正确释放。

### R3 — 资源/材质与渲染正确性（P1）
**目标：让输入数据可靠。**
- 核对 glTF 材质语义：BaseColor、Normal、Metallic-Roughness、Occlusion、Emissive、AlphaMode、双面标记。
- 逐项验证纹理槽位、色彩空间（sRGB/Linear）、法线重映射、Tangent/Bitangent 与坐标约定。
- 统一上传/默认纹理和资源路径管理；补充缺图、错误格式、空材质的 fallback。
- 验证模型包围盒、法线、索引与多 primitive/material。
- 为 Resize、Asset Load/Unload、频繁模型切换设置 GPU 同步与描述符复用规则。

**验收：** 至少两个不同拓扑/材质复杂度模型在 Forward 与 Deferred 中正确显示；Debug View 可定位数据问题。

### R4 — 完成 Deferred PBR 闭环（P1）
按顺序完成：
1. 验证 GBuffer 格式、写入与 Lighting 解码一致；
2. 确保直接光 BRDF（当前 Cook–Torrance + diffuse）正确，处理 roughness 下限、能量守恒与 Fresnel；
3. 验证 HDR 环境加载与 Equirectangular→Cubemap；
4. 完成 Diffuse Irradiance 卷积；
5. 完成 GGX Prefilter（正确的 mip/roughness 对应关系）；
6. 完成 BRDF LUT 积分；
7. 正确显示 Skybox、验证不同环境光照与曝光；
8. 加入 HDR SceneColor、Tonemap、sRGB 输出流程；
9. 统一环境贴图资产相对路径，避免绝对路径；
10. 用金属球/粗糙度球阵列、Saber 和室内场景做验证。

**验收：** 不同 roughness/metallic 值有合理变化；环境光与直接光分工正确；HDR 高光不被提前截断；不同环境图能产生可解释的外观差异。

### R5 — 完成 Deferred 必需 Pass（P1/P2）
**顺序不应按占位注释并行开工，建议逐项单独落地：**
1. Shadow Map：灯光视图/投影、Depth-only PSO、偏移与 PCF；
2. SSAO：半球采样、深度/法线输入、Blur、强度/半径；
3. Transparent Forward：透明物排序、深度只读、Blend PSO，与 Deferred 场景合成；
4. TAA：Projection jitter、Velocity、历史帧、重投影、Neighborhood clamp、History invalidation（Resize/切管线/相机跳变）；
5. PostProcess 基础：曝光、Tonemap；再考虑 Bloom、Color Grading；
6. 后续再加 SSR、DOF、Fog、Vignette 等可选效果。

**验收：** 每个 Pass 可单独启用/关闭；输入/输出、资源状态和结果可检查；TAA 在静止与运动场景均无明显历史拖影或闪烁。

### R6 — 完善 Forward 管线（P1/P2）
- 先完善直接光 PBR 和同一 Material 模型下的材质一致性。
- 加 Shadow Map、环境光照（如果定义为支持）、天空盒和 HDR/Tonemap。
- 加透明对象排序与 Blend State，验证 Depth Prepass 是否真的带来收益。
- 对照 Deferred 逐项比较相同场景输出；对不支持的特性明确报告。
- 核查 ForwardRenderer::GetFeatures()，当前返回值看起来与类名/实现不一致。

**验收：** Forward 不是仅能绘制，而是形成职责明确的独立管线；相同材质参数语义与 Deferred 一致。

### R7 — GPU 性能和诊断工具（P2）
- 加 D3D12 timestamp query，分别记录 GBuffer、Lighting、Shadow、SSAO、TAA、Tonemap 等 Pass 的 GPU 时间。
- CPU 侧记录 Update/command recording/Present 时间；报告平均帧时间、P50/P95。
- 建立 RenderDoc/PIX 捕获流程与性能场景。
- 先测量，再决定 MSAA/TAA、Depth Prepass、批次排序和资源格式取舍。
- 以后再实现 GPU Instancing、Indirect Draw、Frustum/occlusion culling、LOD、纹理 Mipmap/streaming。

**验收：** 每一项优化都有相同场景、相同分辨率下的基线及对比记录。

### R8 — RenderGraph 与自动资源调度（P2）
依赖：R0、R1、R3 之后推进，避免在资源语义仍频繁变化时大规模重构。
- 定义 RenderPass 输入/输出资源、读写类型和执行回调。
- Compile 阶段构建依赖 DAG、检测循环/未定义读、推导 Resource State 与 Barrier。
- 管理 transient resources 和生命周期别名（aliasing）时先保证正确性。
- 先把 Deferred 的 GBuffer→Lighting→Sky→Tonemap 串成 Graph，再接入 Forward。
- 将 DXR 的加速结构和 UAV/AS 状态纳入后续扩展，而不是一开始就设计过度抽象。

**验收：** Pass 顺序由依赖决定；状态转换可调试；新加 Pass 不必手写全局调度流程。

### R9 — DXR Path Tracing 最小闭环（P2）
**目前视为新管线开发，不视为既有功能补齐。**
1. 检查 D3D12 DXR tier/device capability。
2. 建立 BLAS 与 TLAS，验证实例 transform、更新和生命周期。
3. 建立 Raytracing Pipeline State Object（RTPSO）。
4. 编写 RayGen、Miss、ClosestHit shader。
5. 建立 Shader Binding Table（SBT）。
6. 调用 DispatchRays，将结果写入 UAV；
7. 最小结果：法线/命中距离可视化 → 遮挡或硬阴影 → 纯漫反射 Path Tracing → 多重反弹与重要性采样；
8. 增加采样累积、相机变化重置历史、降噪和性能面板。
9. 后续评估 DXR 与 raster 的混合路径。

**验收：** 有真实 DispatchRays 的 GPU 帧捕获和输出图；可切换 Raster/Path Tracing；记录每像素采样数、GPU 时间与降噪开销。

### R10 — GPU Driven Rendering（P3）
仅在 CPU 提交或 draw-call 开销成为可测量瓶颈后推进：
- Frustum culling、实例化和批次合并；
- ExecuteIndirect/Indirect Draw；
- GPU visibility buffer / Hi-Z occlusion；
- Bindless（先建立描述符索引规则和资源生命周期，再扩展）。

### R11 — Neural Rendering（P3 / 研究分支）
不要先把神经渲染塞进核心主线。先以独立 Demo/插件验证一种表示：
- 3D Gaussian Splatting：数据加载、排序/投影、Gaussian rasterization；
- 或 NeRF：采样/体渲染与推理接口；
- 研究 CUDA 与 D3D12 的数据共享/同步、模型推理、场景合成。
- 将 Neural Pass 作为独立渲染路径接到共享 Camera、Scene、Output 接口。

**验收：** 独立样例可运行；不影响 Forward/Deferred；能解释数据格式、算法代价和互操作开销。

## 3. 执行约束
- 每个阶段先建立单一可复现目标，完成后才开下一阶段。
- “代码有类/接口”不算完成；要求可构建、可运行、可视化验证、资源状态正确。
- 避免一次让 AI 改动多个子系统；每次改动都先给计划、改最小范围、构建、运行测试、审查差异。
- 任何新功能都应包括：配置项、资源生命周期、失败路径、Debug View/日志、验收方式。
- 不在缺少 GPU 基准时盲目追求多线程、Bindless、RenderGraph 或神经渲染。

## 4. 当前最优先的具体工作包

**工作包 A（先做）：**
1. 修正并验证 HDR 资源的相对路径与加载失败提示；
2. 验证 IBL 四个 Compute Shader 的实际输出，不要只看 Pass 调用了 Dispatch；
3. 验证 Forward/Deferred 的 resize 和资源重建；
4. 检查 RendererFeature 声明与真实功能一致；
5. 为 Deferred 尚为空的 SSAO/Shadow/Transparent/PostProcess/TAA/Tonemap 块建立实现状态表；
6. 建立一组固定测试场景和 Debug View。

**工作包 B（随后做）：**
1. 集成 Dear ImGui；
2. 先做管线选择、模型选择、帧时间、错误/功能状态显示；
3. 再做材质/光照滑块与 GBuffer 调试；
4. 确保 UI 绘制与 SceneColor/backbuffer/Present 顺序正确。

**工作包 C（再之后）：**
1. 完成 IBL / HDR / Tonemap 闭环；
2. 完成 Shadow Map 和 SSAO；
3. 处理透明与 TAA；
4. 建立 GPU timestamp 与帧捕获基准；
5. 再决定 RenderGraph 的改造边界。

## 5. 目前不建议提前做
- 暂不先做完整 RenderGraph，直到资源读写和 Pass 职责已稳定；
- 暂不大规模 GPU Driven/Bindless，除非 Profile 指出瓶颈；
- 暂不把 Path Tracing 标记为完成，必须完成 BLAS/TLAS、RTPSO、SBT、DispatchRays；
- 暂不将 Neural Rendering 与主渲染器深度耦合，先用独立 Demo 验证。

## 6. 参考代码和文档
- README: https://github.com/wow425/ToiyumeRender/blob/master/README.md
- Roadmap: https://github.com/wow425/ToiyumeRender/blob/master/docs/Roadmap.md
- Showcase: https://github.com/wow425/ToiyumeRender/blob/master/docs/Showcase.md
- Engine architecture: https://github.com/wow425/ToiyumeRender/blob/master/Engine/Engine.md
- Base renderer: https://github.com/wow425/ToiyumeRender/blob/master/Engine/04_Renderer/Renderer/Base/BaseRenderer.h
- Deferred renderer: https://github.com/wow425/ToiyumeRender/blob/master/Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp
- Forward renderer: https://github.com/wow425/ToiyumeRender/blob/master/Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp
- Environment lighting: https://github.com/wow425/ToiyumeRender/blob/master/Engine/04_Renderer/EnvironmentLightingManager.cpp
- Temporal effects: https://github.com/wow425/ToiyumeRender/blob/master/Engine/04_Renderer/TemporalEffects.cpp
