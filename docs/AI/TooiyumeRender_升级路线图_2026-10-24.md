# TooiyumeRender 渲染器升级路线图

> 依据：《TooiyumeRender 审计总报告（第一阶段）》及五份专项审计，审计日期 2026-10-24。  
> 当前阶段：只读审计已完成；报告称未修改源码。此路线图是实施规划，不代表以下修复已经完成。

## 1. 总体判断

TooiyumeRender 不需要推倒重写。现有 DX12 RHI、Deferred GBuffer/Lighting 主链、PBR BRDF、glTF 解析与 `.ty` 模型烘焙都有可复用的实现。升级应围绕现有架构补齐正确性、生命周期、交互调试和 DXR，而不是先引入 ECS、RenderGraph 或大规模抽象层。

当前最重要的约束是：**审计报告确认了干净构建成功，但没有启动三个 Demo，也没有验证运行画面**。`Assets/Model` 缺失，因此当前不能据代码或构建结果宣称画面正确。第一阶段必须建立可重复的运行验证基线。

目标路线：

**可重复构建与资产基线 → 修正渲染数据错误 → 稳定资源/窗口生命周期 → 统一入口与 Dear ImGui → 完成可验证的光栅 PBR → DXR 最小闭环 → 阴影、SSAO、透明、TAA 等扩展。**

## 2. 阶段总览

| 阶段 | 优先级 | 目标 | 主要交付 | 阶段退出条件 |
|---|---|---|---|---|
| R0 基线与可复现性 | P0，先做 | 新环境能配置、构建、加载测试资产并启动 | 资产策略、路径解析、shader 编译链、构建脚本 | 干净构建成功，至少一个标准模型可见，日志可定位资产/设备失败 |
| R1 核心渲染正确性 | P0 | 修复材质槽、默认贴图、深度几何、TBN、分辨率等确定性错误 | 修复清单、GBuffer 对照截图 | 无贴图与全贴图材质均合理，多 draw 模型通过验证 |
| R2 生命周期与窗口稳定性 | P0 | Resize、最小化、相机宽高比、资源重建和 sorter 生命周期可靠 | 完整 Resize 链、回归测试 | 多分辨率拖动、最小化/还原和重复加载不崩溃、不拉伸 |
| R3 统一入口与交互工作台 | P1 | 模型、渲染器、调试视图和参数可实时切换 | `RendererDemoApp`、RendererRegistry 接通、ImGui、RenderParams | UI 交互真实改变画面；切换 renderer/model 不黑屏、不累积旧 draw |
| R4 光栅 PBR 闭环 | P1 | 材质、光照、IBL、色彩管理和 Forward/Deferred 行为清晰 | PBR 参数链、IBL 缓存、AlphaTest、色彩链路 | 标准测试场景通过材质/光照/IBL/色彩检查，关键算法可从 UI 调试 |
| R5 DXR 最小闭环 | P1 | 从设备能力检查到 `DispatchRays` 真正执行 | RTPSO、BLAS/TLAS、SBT、RayTracingRenderer | DXR 模式输出可验证的命中/未命中结果；不支持 DXR 时可回退光栅 |
| R6 扩展功能与工程加固 | P2 | 逐项补齐高级渲染功能和非阻塞工程问题 | Shadow、SSAO、透明、TAA、热重载、文档等 | 每项有单独验收，不以占位 Pass 或宣传文案计为完成 |

## 3. R0：建立可重复构建与运行基线

### 3.1 必须完成

1. **确定测试资产的交付方式。** 给仓库提供可合法再分发的小型 glTF/GLB 测试模型，或提供带校验值、许可证说明的下载脚本；同时建议实现内置立方体/球体作为无外部资产时的启动烟雾测试。仅有 Skybox 不能验证 Mesh、材质、深度和 PBR。
2. **建立统一 `AssetsRoot`。** 移除源码和 Demo 中的硬编码盘符路径。明确资源路径以可执行文件目录、项目根目录或命令行配置中的哪一个为基准，并把模型、环境图和缓存路径纳入同一规则。
3. **恢复可移植的 CMake 配置。** toolchain 使用 `VCPKG_ROOT` 等环境配置，而不是开发机绝对路径。修复或移除非法的 `CMakeSettings.json`。对 PIX DLL 采取明确策略：由项目分发、包管理或安装脚本提供；不要只在本机存在。
4. **统一 Shader 编译产物来源。** 把 Forward 实际使用的 `01_Default` HLSL 纳入 CMake 编译，并让 Forward 引用构建目录生成的 ShaderCache，而不是仓库内旧 `.h` 字节码。补充 `.hlsli` include 依赖，使公共 Shader 修改会触发重编译。
5. **固定构建验证流程。** 提供 Debug/Release 的配置、构建、运行脚本；遇到 MSVC 工具集变更时清理或重新生成失效 PCH。修复 Release 配置下 `RELEASE` 宏缺失的问题。
6. **关闭不必要的无条件模型重建。** 默认使用已有 `.ty` 缓存，仅在显式请求时强制重新烘焙，并记录导入失败原因。

### 3.2 验收

- 干净目录可配置、可构建全部目标；至少验证 Debug 与 Release。
- 三个旧 Demo 能启动，至少一个受版本控制或可重复下载的测试模型可见。
- 资源缺失时有清晰错误，不允许仅输出一行日志后留下难以区分的黑屏。
- 修改公共 `.hlsli` 后，相关 HLSL 会重新编译。
- 关闭 `forceRebuild` 后，第二次启动不重复执行完整资产解析/烘焙。

## 4. R1：修复确定性渲染错误

按下列顺序处理。建议每项独立提交，并保留修复前后截图或日志。

| ID | 修复项 | 实施要点 | 验收方法 |
|---|---|---|---|
| R1.1 | 统一材质纹理槽语义 | 统一 `glTF`、`ConstantBuffers` 和 GPU 绑定使用的枚举/映射；避免不同 enum 被当成同一索引；增加数量与索引的静态检查 | 分别使用 BaseColor、Normal、Emissive、Occlusion、MetallicRoughness 独立贴图的测试材质核验绑定 |
| R1.2 | 修正默认纹理表 | 无 Normal 时用中性法线；无 Emissive 时用黑色；无 AO 时 AO=1；无 Metallic-Roughness 时采用符合材质默认值的纹理 | 无法线、无 AO、无 MR 贴图的模型不应出现全局错误法线、AO 全黑或意外全金属 |
| R1.3 | 修复深度顶点缓冲偏移 | 为每个 draw 正确累计 `curDepthVBOffset` | 构造同一 Mesh 含至少两个 draw 的模型，确认各 draw 的深度/AlphaTest 路径正确 |
| R1.4 | 修正 Tangent 解码 | 使用已经从 UNORM 解码还原的 Tangent 构造 TBN，不要混用编码值与还原值 | 法线贴图模型在旋转、掠射角和不同 UV 下检查高光方向 |
| R1.5 | 移除渲染分辨率硬编码 | 光照 Shader 从常量缓冲或资源尺寸获取实际分辨率 | 以 1280×720、1920×1080、窗口化尺寸分别检查世界坐标重建和光照 |
| R1.6 | 修正 Deferred sorter 生命周期 | 在合适的帧结束/数据更新边界 Reset sorter，确保旧模型、旧 draw 不会累积 | 反复加载、卸载或切换模型，确认 draw 数量与当前场景一致 |
| R1.7 | 固定 C++/HLSL 常量缓冲契约 | 对齐 `GlobalConstants` 等字段顺序、类型和打包规则；新增字段前先核对所有 Shader 声明 | 用静态检查/断言和 Shader 编译验证，不只依赖画面目测 |

**验收门槛：** R1 未通过前，不开发以 GBuffer 数据为依据的 Debug View，也不把材质显示异常归因于 BRDF 数学。审计判断活跃版本 `DeferredCommon.hlsli` 的 GGX/Smith/Schlick 核心实现总体正确，应先保护这部分实现，优先修复输入数据与资源映射。

## 5. R2：补齐 Resize 与 GPU 资源生命周期

Resize 不是 UI 小功能，而是渲染器正确性和稳定性的基础设施，必须在 renderer 切换与 ImGui 前完成。

### 5.1 实施内容

1. 让 `Display` 的 Resize 事件可靠传递到当前 App/Renderer；移除当前 `OnResize` 零调用的断链。
2. Resize 前等待必要的 GPU 工作完成；按一致顺序释放并重建 SwapChain backbuffer 相关资源、`SceneColorBuffer`、GBuffer、Depth、Forward render targets、视口和 scissor。
3. 窗口最小化时忽略 0×0 尺寸；恢复时再执行合法的资源重建。
4. 更新 Camera aspect ratio 与投影矩阵，避免 Buffer 已重建但画面仍按旧比例拉伸。
5. 使用 SwapChain 当前 backbuffer index，而非手工假设索引递增。
6. 修正资源 `Destroy` 后描述符句柄状态；Resize 循环不能持续消耗 RTV/Descriptor Heap。资源替换至少保证 GPU 不再引用旧资源后才销毁。
7. 明确 `Present` 的资源状态转换和调用顺序；启用 D3D12 Debug Layer 检查资源尺寸与状态错误。

### 5.2 验收

- 在不同长宽比窗口连续拖动 30 秒，不崩溃、不拉伸、不出现资源尺寸不匹配。
- 最小化、还原、重复调整分辨率均可恢复。
- 连续执行至少 100 次 Resize 或模型/Renderer 切换，无持续增长的描述符分配、无旧资源画面残留。
- Debug Layer 无新的资源状态、RTV 维度或生命周期错误。

## 6. R3：统一入口、Renderer 切换与 Dear ImGui

### 6.1 先整合，不先删旧 Demo

新建统一的 `RendererDemoApp`，复用现有 `RendererRegistry`、`BaseRenderer` 和三个现有 Demo 的资产/相机初始化逻辑。先让新入口通过完整验证，再决定是否保留旧 Demo 作为回归测试入口。

- 修正 `ForwardRenderer` 注册名尾随空格等注册问题，并让 `RendererRegistry::CreateRenderer()` 成为真实创建路径。
- 建立清晰的 Renderer 切换顺序：等待旧 Renderer 的 GPU 工作完成 → Shutdown → 断开旧输出资源 → 创建并初始化新 Renderer → 重新绑定输出资源与相机尺寸 → 恢复渲染。
- 将模型清单与路径解析集中管理，UI 模型列表必须来源于实际可加载资产。

### 6.2 ImGui 接入

- 通过项目现有 vcpkg/CMake 体系引入 Dear ImGui 的 Win32 与 DX12 backend。
- ImGui 使用独立的 shader-visible Descriptor Heap；不要挤占引擎已有材质 Descriptor Heap。
- 在 WndProc 转发鼠标、键盘输入；当 ImGui 捕获输入时，相机控制不应同时响应。
- 增加 `RenderParams` 作为参数的唯一 CPU 侧真源，明确参数从 UI 到 Constant Buffer 再到 Shader 的链路。
- 面板建议：Performance、Scene/Model、Renderer、Material/Lighting、Debug View、DevTools。
- 初期只放真实接通的控件；未实现的功能显示禁用状态或 `Not Implemented`，不能让 UI 数值变化但画面不变。

**需要特别修正审计报告中的一个时序风险：** 报告建议在 `Present` 前调用 `RenderUI`，但现有 `Display::Present` 会先把 `SceneColorBuffer` Copy 到 SwapChain backbuffer。如果 ImGui backend 直接把 UI 画到 backbuffer，随后再执行该 Copy，UI 会被覆盖。正确实现应当二选一：**将 ImGui 画到 SceneColor 后再复制，或在 SceneColor 复制到 backbuffer 之后、SwapChain Present 之前绘制 ImGui，并正确处理 backbuffer 的 RENDER_TARGET/PRESENT 状态转换。** 若采用第二种方式，就不能仅把 `RenderUI` 简单插在当前 `Display::Present` 调用之前。

### 6.3 验收

- 在同一入口中切换 Forward、Deferred、DXR（DXR 尚不可用时明确显示原因/禁用）模式。
- 切换模型后没有旧模型残留、崩溃或持续性资源泄漏。
- FPS/帧时间、模型切换、Renderer 切换和 Debug View 可用；Performance 面板区分 CPU 帧时间与真实 GPU 时间，不把 CPU 测量冒充 GPU 时间。
- 拖动一个已接通的参数（如光照强度或曝光）时，画面发生可预测变化。

## 7. R4：完成可验证的光栅 PBR 闭环

不要把“Deferred Lighting 能执行”直接等同于“完整 PBR”。按材质数据、直接光、IBL、色彩输出和资产性能依次收尾。

### 7.1 材质数据和贴图

- 修复 sRGB 标志映射：BaseColor/Emissive 等颜色贴图走正确色彩空间，Normal、Occlusion、Metallic-Roughness 等数据贴图不走 sRGB 解码。
- 将纹理压缩 flags 从材质转换流程完整传递到 TextureConvert；按格式、色彩空间和压缩参数设计 DDS 缓存键，避免不同需求复用同一错误缓存。
- 修复 `normalTexture.scale` 的 glTF 读取路径；在数据链支持后再将其暴露为实时参数。
- AlphaMode 先实现 `MASK`/AlphaTest（正确传递 `alphaCutoff` 并在 Shader 中 `clip`）；`BLEND` 透明混合独立排期，不能把它与 AlphaTest 混为一谈。
- 核验 AO、MR 通道和 UV 选择；未支持的 glTF 属性应在文档中明确标注，不要静默伪装为完整支持。

### 7.2 光照与 IBL

- 将光源从硬编码常量改为可配置数据，至少支持方向、强度和颜色；通过 `RenderParams`/常量缓冲真正传入 Shader。
- 修复 Prefilter LOD：按实际 mip 数计算最大可用 mip，不能继续使用与资源不一致的硬编码 8.0。
- 修正 Skybox UV/NDC 映射，保证背景方向与相机一致。
- 将 IBL 烘焙移出每帧 Update，只在首次初始化或环境图改变时执行；实现可验证的 DDS 缓存并正确更新状态字段。
- 检查直接光与 IBL 的能量衔接、粗糙度响应、法线和 AO 影响。

### 7.3 色彩管理与 Renderer 能力边界

- 明确 HDR 场景颜色、Tonemap/曝光、输出色彩空间和最终 backbuffer 格式之间的关系，避免 ACES 只作用于 Lighting 而遗漏 Skybox，或重复/遗漏 Gamma 转换。
- 为 Forward Renderer 接通真实光照，修正其 Z/AlphaTest 顶点流和根签名/PSO 契约。若短期不实现与 Deferred 同等的 PBR 功能，UI 和文档必须明确其实际能力，不能宣称两个 Renderer 功能等价。
- 增加 6 档 Debug View：Final、BaseColor、Normal、Metallic、Roughness、Depth。每档应直接输出对应数据源，且不能在 R1 修复之前使用。

### 7.4 验收

- 使用固定测试场景和材质球，验证 BaseColor、Normal、Metallic、Roughness、AO、Emissive、AlphaTest、IBL 与 Tonemap。
- 在至少三个窗口分辨率和不同曝光/光照参数下检查结果。
- IBL 首次烘焙后能复用缓存；更换环境图后重新烘焙；不更换时不在每帧阻塞。
- 记录关键测试场景的截图与配置，作为之后升级的视觉回归基准。

## 8. R5：DXR 最小可运行闭环

DXR 要作为新增 Renderer 接入，而不是让现有光栅路径依赖光追。光栅路径始终应可独立构建、运行和回退。

### D0：设备能力与请求链

- 修复 `RequiresRaytracingSupport()` 的 `const override` 签名问题，消除请求被基类静默吞掉的情况。
- 明确导出并管理 DXR Device/feature tier；启动日志记录能力检查结果。
- 无 DXR 支持时禁用相应 UI 项并继续运行光栅模式。

**验收：** override 被编译器验证；日志显示真实查询到的 Raytracing Tier；启动行为与硬件能力一致。

### D1：Shader 与加速结构

- 扩展 Shader 编译链，支持与项目 DXC/设备能力匹配的 DXIL library profile；为 DXR state object 建立独立创建/缓存路径，不复用仅面向 Graphics PSO 的缓存。
- 从 Mesh VB/IB 构建 BLAS；从 ModelInstance 的变换构建 TLAS；明确 AS 输入/输出 buffer 的状态、UAV barrier 和生命周期。
- 对静态几何尽量复用 BLAS；只在实例变换变化时更新/重建 TLAS，避免每帧无条件重复构建。

**验收：** PIX 或调试日志能确认 `BuildRaytracingAccelerationStructure` 被执行，BLAS/TLAS 输入和状态正确。

### D2：Ray Dispatch 最小图像

- 实现 Ray Generation、Miss、Closest Hit/Hit Group、Shader Binding Table。
- 先输出清晰、易验证的诊断结果：命中返回法线颜色，未命中返回黑色/背景色；结果写入 Renderer 输出并经过统一 Present 路径。
- 注册 `RayTracingRenderer`，让统一入口可以切换光栅与 DXR。

**验收：** DXR 模式中画面确实来自 `DispatchRays`，PIX 中可追踪 Dispatch；关闭 DXR 后 Deferred 仍可正常运行。

### D3：从“DXR 可执行”到“有用途的光追算法”

按独立里程碑推进：先做硬阴影射线，再做反射或其他明确的光追效果。每个算法都应有可比较的光栅/光追场景、明确采样与偏差策略、性能统计和开关。未完成这些之前，只能称为 DXR 最小闭环，不应宣传为完整路径追踪器。

## 9. R6：后续功能与工程加固

建议顺序如下，避免并行开太多空壳：

1. **Shadow Map**：复用现有 ShadowCamera/ShadowBuffer 骨架，接通 shadow pass 与采样；与 DXR 阴影作为不同算法选项比较。
2. **透明混合**：在 AlphaTest 已正确工作的基础上，明确排序、混合状态、深度写入和材质限制。
3. **SSAO**：使用正确的深度/法线数据，单独提供开关和调试输出。
4. **Tonemap/HDR 输出扩展**：若 R4 的 SDR 色彩链尚未满足目标，再进行 HDR SwapChain 输出等扩展；基础色彩链不能继续留空。
5. **TAA**：先实现真实 Velocity Buffer 写入、历史帧资源、重投影与拒绝机制，再启用 TAA Shader。当前仅有孤立 Shader 或空函数不算完成。
6. **Shader 热重载与 ShaderLibrary**：先修复生成头的 include guard/链接符号问题，再实现依赖追踪、编译失败回退和状态对象更新。
7. **RHI 非阻塞问题**：按真实需求修复 Fence 完成值回写、UAV Barrier 批处理边界、描述符分配线程安全、Sampler 去重、重复创建视图等问题。多线程命令录制未启用前，不必为了架构完整性提前重写调度系统。
8. **文档与工程清理**：修 README、Showcase 和 Roadmap，更新真实目录和功能状态；删除、实现或标注空文件/死目录；每个 Pass 的文档状态必须区分“已执行、部分接通、仅有 Shader、空壳”。

当前不建议优先引入完整 RenderGraph、ECS、Reverse-Z 或多线程命令录制。这些不是现阶段验证正确 PBR、接入 UI 或运行 DXR 最短路径上的瓶颈。

## 10. AI 编程执行规则

为避免 AI 一次改动范围过大，把每个里程碑拆成独立可审查的小批次：

1. 开始前先读相关源码与专项报告，不凭文件名猜实现；列出将修改的文件、依赖和潜在回归。
2. 一次只处理一个可验证目标；禁止同时大规模重命名、重构目录和改渲染算法。
3. 完成后执行 Debug 构建；碰到构建、Shader 编译或链接错误，先修当前批次，不继续堆下一批。
4. 运行对应 Demo，使用固定测试资产；记录截图、日志、GPU Debug Layer/PIX 证据。构建通过只证明能编译，不证明画面正确。
5. 对每个修复给出“问题原因、修改文件、关键差异、验证命令、运行结果、未验证项”。没有实际运行就标记为未验证。
6. 每个阶段通过后再进入下一阶段；出现新错误时回到最近一个可复现基线。

## 11. 第一轮具体执行清单

建议按这个顺序给 AI 编程工具下任务，不要一开始就要求“全部完成”：

- **批次 A（R0）**：资产/资源根与干净启动基线；CMake 可移植化、PIX 依赖策略、Forward Shader 编译来源和 `.hlsli` 依赖修复。
- **批次 B（R1）**：统一纹理槽映射与默认纹理；修复深度 VB offset、Tangent 解码、动态分辨率、Deferred sorter Reset、CB 布局契约。
- **批次 C（R2）**：完整 Resize/最小化/相机宽高比/Backbuffer 索引/描述符重建。
- **批次 D（R3）**：统一入口接通 RendererRegistry，先验证 Forward/Deferred 切换，再接 ImGui；确认 UI 绘制与 Copy/Present 的正确时序。
- **批次 E（R4）**：RenderParams→CB→Shader 数据链、Debug View、灯光控制、IBL 修复与缓存、材质/色彩链路。
- **批次 F（R5）**：DXR D0 → D1 → D2，每个子阶段独立编译和验证。
- **批次 G（R6）**：按优先级逐项完成 Shadow、透明、SSAO、TAA、热重载与工程清理。

## 12. 最终验收矩阵

| 验收维度 | 必须证明的内容 | 证据形式 |
|---|---|---|
| 构建 | 干净 Debug/Release 配置与构建成功；shader 依赖变更触发重编 | 构建命令与日志 |
| 运行 | 可重复启动、正确加载测试模型、失败路径清晰 | 启动日志、测试资产清单 |
| 图像正确性 | 材质槽、法线、深度、IBL、色彩输出正确 | 固定场景截图、Debug View 对照 |
| 生命周期 | Resize、最小化/还原、模型切换、Renderer 切换稳定 | 回归步骤、Debug Layer 日志 |
| UI 数据链 | 参数更改实际影响目标 Shader/画面 | UI 操作前后截图及参数日志 |
| DXR | 真正执行 AS 构建和 `DispatchRays`，而非只检测设备能力 | PIX Capture、DXR 诊断画面 |
| 性能 | 对比关键路径 CPU/GPU 时间、启动烘焙成本、纹理显存 | 时间戳/Profiler 数据及测试条件 |
| 文档 | 声明的已完成功能与实测状态一致 | README、Roadmap、功能状态表 |

**路线图完成原则：** 不以代码量、面板数量或 Pass 名称判断进度；以可重复运行、可观察证据、明确的失败边界和独立验收结果判断进度。
