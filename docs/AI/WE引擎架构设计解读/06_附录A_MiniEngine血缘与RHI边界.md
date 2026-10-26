# 06 附录 A：MiniEngine 血缘与 RHI 边界

> 本附录回应约束文档对「MiniEngine 作为现有 DX12 底层实现与资源管理的辅助参考」的要求。
> 作者：Lead。结论分「实测」与「未确认」两类，逐条标注。

---

## A.1 为什么需要这个附录

约束文档第 9、11 行把 MiniEngine 指定为 RHI 辅助参考：

- `docs/AI/ToiyumeRender 架构演进专项约束/ToiyumeRender 架构演进专项约束.md:9` —— 给出上游地址 `https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine`。
- `ToiyumeRender 架构演进专项约束.md:11` —— 「Wicked Engine 是主要的高层渲染架构参考；MiniEngine 是现有 DX12 底层实现与资源管理的辅助参考。」

**实测，本地不存在 MiniEngine 检出**：

- `find D:/CS-Self-Study -maxdepth 5 -iname '*MiniEngine*'` → 无结果。
- `D:\CS-Self-Study\Computer_Graphics\DX12\工程实现参考\` 下仅有 `WickedEngine-master`。

因此无法直接对照其源码。但存在一条更强的证据链：**TooiyumeRender 的 `Engine/` 本身就是 MiniEngine 的血缘后代**，这一事实使「该参考哪部分、该改哪部分」从主观选型变成可核对的差异分析。

---

## A.2 血缘证据（实测）

### A.2.1 仓库自述

`Engine/Engine.md:1` 标题为：`# 📦 MiniEngine Core 模块结构指南`，全文 91 行，明确以 MiniEngine Core 为对象描述本 Engine 的模块划分（Command System / Resource System / Descriptor System / Synchronization System / Memory Management System / Resource State Tracking(未实现)），并给出「Engine 职责 = Graphics API Layer + GPU Infrastructure + 部分 RHI」。

`Engine/Engine.md` 同时给出该项目的推荐依赖方向（原文）：

> Application → Renderer → RenderFeatures → RenderPasses → RenderGraph → ResourceSystem → GPUInfrastructure → RHI → Core
> Math 与 Utility 为横向基础模块。

并列出「严禁出现的反向依赖」：RenderPass 依赖 Renderer、ResourceSystem 依赖 Scene、RHI 依赖 RenderGraph、Utility 依赖 Renderer、Math 依赖 GPU 模块。

**注意**：这是**期望**，不是现状。现状是否满足该单向依赖，由 `02_架构现状审计_audit-tooiyume.md` 独立验证；本附录只把它作为「目标意图」的历史记录。

### A.2.2 文件级来源对照（实测）

上游 MiniEngine 的代码分布在三个子目录：`MiniEngine/Core/`（144 项）、`MiniEngine/Model/`、`MiniEngine/ModelConverter/`。TooiyumeRender 把它们重组进 `Engine/00_Core … 05_Scene` 六层。对照如下（左=上游相对路径，右=本仓库路径）：

| 上游 MiniEngine | 本仓库 | 说明 |
| --- | --- | --- |
| `Core/GraphicsCore.h/.cpp` | `Engine/02_RHI/GraphicsCore.h/.cpp` | 名字空间 `Graphics` 未改 |
| `Core/CommandAllocatorPool.*` | `Engine/02_RHI/Command/CommandAllocatorPool.*` | 重组进 Command 子目录 |
| `Core/CommandContext.*` | `Engine/02_RHI/Command/CommandContext.*` | 同上 |
| `Core/CommandListManager.*` | `Engine/02_RHI/Command/CommandListManager.*` | 同上 |
| `Core/CommandSignature.*` | `Engine/02_RHI/Command/CommandSignature.*` | 同上 |
| `Core/DescriptorHeap.*` | `Engine/02_RHI/Descriptor/DescriptorHeap.*` | 重组进 Descriptor |
| `Core/DynamicDescriptorHeap.*` | `Engine/02_RHI/Descriptor/DynamicDescriptorHeap.*` | 同上 |
| `Core/LinearAllocator.*` | `Engine/02_RHI/Memory/LinearAllocator.*` | 重组进 Memory |
| `Core/GraphicsCommon.*` | `Engine/02_RHI/Pipeline/GraphicsCommon.*` | 重组进 Pipeline |
| `Core/PipelineState.*` | `Engine/02_RHI/Pipeline/PipelineState.*` | 同上 |
| `Core/RootSignature.*` | `Engine/02_RHI/Pipeline/RootSignature.*` | 同上 |
| `Core/SamplerManager.*` | `Engine/02_RHI/Pipeline/SamplerManager.*` | 同上 |
| `Core/GpuResource.h` | `Engine/02_RHI/Resource/GpuResource.h` | 重组进 Resource |
| `Core/GpuBuffer.*` | `Engine/02_RHI/Resource/GpuBuffer.*` | 同上 |
| `Core/ColorBuffer.*` | `Engine/02_RHI/Resource/ColorBuffer.*` | 同上 |
| `Core/DepthBuffer.*` | `Engine/02_RHI/Resource/DepthBuffer.*` | 同上 |
| `Core/PixelBuffer.*` | `Engine/02_RHI/Resource/PixelBuffer.*` | 同上 |
| `Core/ShadowBuffer.*` | `Engine/02_RHI/Resource/ShadowBuffer.*` | 同上 |
| `Core/Texture.*` | `Engine/02_RHI/Resource/Texture.*` | 同上 |
| `Core/UploadBuffer.*` | `Engine/02_RHI/Resource/Heap/UploadBuffer.*` | 多出一层 Heap |
| `Core/ReadbackBuffer.*` | `Engine/02_RHI/Resource/Heap/ReadbackBuffer.*` | 同上 |
| `Core/d3dx12.h`、`Core/dds.h` | `Engine/02_RHI/d3dx12.h`、`Engine/03_AssetSystem/Importers/Texture/dds.h` | dds.h 被移入资产层 |
| `Core/DDSTextureLoader.*` | `Engine/03_AssetSystem/Importers/Texture/DDSTextureLoader.*` | 移入资产层 |
| `Core/TextureManager.*` | `Engine/03_AssetSystem/Importers/Texture/TextureManager.*` | 移入资产层 |
| `Model/TextureConvert.*` | `Engine/03_AssetSystem/Importers/Texture/TextureConvert.*` | 上游在 Model/ 而非 Core/ |
| `Model/MeshConvert.*` | `Engine/03_AssetSystem/Importers/Mesh/MeshConvert.*` | 同上 |
| `Model/IndexOptimizePostTransform.*` | `Engine/03_AssetSystem/Importers/Mesh/IndexOptimizePostTransform.*` | 同上 |
| `Model/ModelConvert.*` | `Engine/03_AssetSystem/Importers/Model/ModelConvert.*` | 同上 |
| `Model/ModelLoader.*`、`Model/Model.*` | `Engine/05_Scene/Model/ModelLoader.*`、`Engine/05_Scene/Model/Model.*` | 移入场景层 |
| `Model/glTF.*`、`Model/json.hpp` | `Engine/03_AssetSystem/Importers/glTF.*`、`.../json.hpp` | 同上 |
| `Model/ConstantBuffers.h` | `Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h` | **上游在 `Model/` 而非 `Core/`**（此前易误判） |
| `Model/LightManager.*` | `Engine/05_Scene/Lighting/LightingSystem.*` | 已重命名 |
| `Core/BufferManager.*` | `Engine/04_Renderer/BufferManager.*` | 移入渲染器层 |
| `Core/Camera.*`、`Core/CameraController.*`、`Core/ShadowCamera.*` | `Engine/05_Scene/Camera/` | 移入场景层 |
| `Core/Display.*` | `Engine/01_Application/Display.*` | 移入应用层 |
| `Core/GameCore.*` | `Engine/01_Application/GameCore.*` | 同上 |
| `Core/GameInput.*` | `Engine/01_Application/GameInput.*` | 同上 |
| `Core/SystemTime.*` | `Engine/00_Core/SystemTime` | 同上 |
| `Core/FileUtility.*`、`Core/Hash.h`、`Core/Utility.*`、`Core/Color.*` | `Engine/00_Core/Utility/` | 重组 |
| `Core/Math/`、`Core/VectorMath.h` | `Engine/00_Core/Math/` | 重组（并自行扩充 Matrix3/4、Transform 等） |
| `Core/TemporalEffects.*` | `Engine/04_Renderer/TemporalEffects.*` | **上游在 Core/，本项目移入渲染器层**（TAA） |
| `Core/Shaders/` | `Engine/Shader/` | 移出 02_RHI，独立顶层目录 |

说明：本表基于上游仓库文件清单（2026-10-24 抓取）与本仓库实际文件名比对；同名同义即判定继承，未逐文件做内容 diff（见 A.7 未确认项）。

### A.2.3 内容级抽查（实测）

`Core/GraphicsCore.h`（上游 54 行）内容摘要：

`namespace Graphics { void Initialize(bool RequireDXRSupport=false); void Shutdown(); bool IsDeviceNvidia(ID3D12Device*); bool IsDeviceAMD(...); bool IsDeviceIntel(...); extern ID3D12Device* g_Device; extern CommandListManager g_CommandManager; extern ContextManager g_ContextManager; #ifndef RELEASE extern const GUID WKPDID_D3DDebugObjectName; #endif }`

`Engine/02_RHI/GraphicsCore.h`（本仓库 42 行）：同为 `namespace Graphics`，第 26 行 `extern ID3D12Device* g_Device; // 设备`，第 28 行 `extern CommandListManager g_CommandManager; // 命令管理器`。

差异（可核对）：
1. 本仓库**没有** `ContextManager g_ContextManager`（上游有）。
2. 上游用 `#ifndef RELEASE` 保护 `WKPDID_D3DDebugObjectName`；旧审计报告把「RELEASE 宏缺失」列为 P0，本仓库该保护的存在性需在 `02` 中复核。
3. 上游保留 WRL（`Microsoft::WRL`）与设备厂商探测（IsDeviceNvidia/AMD/Intel）；本仓库是否保留见 `02`。

其他仓库内自证：`Engine/03_AssetSystem/Importers/glTF.h:74` 注释直接提到 MiniEngine 的作者（「MiniEngine 的作者…直接把规范里的这俩字段注释掉了」），说明该文件确实直接承袭 MiniEngine 的 `Model/glTF.h`。

---

## A.3 相对 MiniEngine，本项目删掉了什么（实测：按上游清单逐一比对）

### A.3.1 整个后处理与屏幕空间效果族被删掉

上游 `MiniEngine/Core/` 中存在、而本仓库 `Engine/` 中**完全没有同名文件**的效果模块：

| 上游文件 | 职责 | 本仓库 |
| --- | --- | --- |
| `Core/PostEffects.h/.cpp` | 后处理编排 + 后缓冲拷贝 | 无 |
| `Core/FXAA.h/.cpp` | 快速抗锯齿 | 无 |
| `Core/SSAO.h/.cpp` | 屏幕空间环境光遮蔽（含 Z 线性化） | 无（有自带环境光？？需 `02` 确认） |
| `Core/DepthOfField.h/.cpp` | 景深 | 无 |
| `Core/MotionBlur.h/.cpp` | 运动模糊 | 无 |
| `Core/ImageScaling.h/.cpp` | 图像缩放（Upscale） | 无 |
| `Core/TemporalEffects.h/.cpp` | TAA（抖动、历史、解析） | **有**，但被移到 `Engine/04_Renderer/TemporalEffects.*` |

**架构含义**：MiniEngine 的 Core 里**同时**装着 RHI 与「公共渲染功能」（后处理族）。本项目照抄了 RHI 部分，却没有照抄与之配套的公共渲染功能层——于是出现了「有引擎底盘、没有可复用的渲染功能层」的结构性空缺，这正是约束原则②要解决的问题，也是本项目自行发明 `Engine/04_Renderer/` 层级的原因。

### A.3.2 上游的渲染管线本体没有被采纳

上游 `MiniEngine/Model/` 中存在 `Renderer.h/.cpp` 与 `SponzaRenderer.h/.cpp`——这是 MiniEngine 自己的渲染色序编排。

本仓库 `Engine/` 中**没有** `Renderer.*` / `SponzaRenderer.*`；取而代之的是自研的 `BaseRenderer / DeferredRenderer / ForwardRenderer / RendererRegistry`（位于 `Engine/04_Renderer/Renderer/`）。

**架构含义**：本项目的渲染路径层不是继承来的，是自研的。因此它没有现成参考可对齐，其边界问题（与公共渲染功能混淆、编排逻辑散落）必须在 `03`/`04` 中作为设计问题处理，而不能靠「照 MiniEngine 补文件」解决。

### A.3.3 上游的 GPU 计时与调优基础设施没有被采纳

| 上游文件 | 职责 | 本仓库 |
| --- | --- | --- |
| `Core/GpuTimeManager.h/.cpp` | GPU 时间戳统计 | 无 |
| `Core/EngineProfiling.h/.cpp` | 引擎级 profiling | 无（有 PIX 事件） |
| `Core/EngineTuning.h/.cpp` | 运行期参数调节（BoolVar/NumVar） | 无 |

**架构含义**：约束原则⑤要求「资源生命周期与 GPU 执行同步可验证」。上游提供 `GpuTimeManager` 作为 GPU 侧时间证据；本项目缺失，意味着「可验证」目前只能靠 PIX 捕获与 CPU 侧断言，这在 `05` 的验收设计里需要显式补偿。

### A.3.4 其他未采纳项（不影响架构判断，仅记录）

`Core/BitonicSort`、`Core/BuddyAllocator`、`Core/EsramAllocator`、`Core/GraphRenderer`、`Core/ParticleEffect*`、`Core/ParticleShaderStructs.h`、`Core/TextRenderer`、`Core/Cube`、`Core/Input.*`、`Core/Core.vcxproj`、`Core/Fonts/`；`Model/Animation.*`、`Model/BuildH3D.cpp`、`Model/ModelH3D.*`、`Model/ParticleEffects.*`、`ModelConverter/`（Assimp 转换工具链）。

---

## A.4 MiniEngine 的架构特征与局限（对本次设计的启示）

1. **它不是一个分层架构**。上游 `Core/` 里同目录放着 `GraphicsCore.h`（RHI）与 `PostEffects.h`（渲染功能）、`GameCore.h`（应用框架）、`TextureManager.h`（资产）。MiniEngine 靠「命名空间 + 文件命名约定」组织，不靠目录分层。
   → **启示**：约束文档禁止「只复制目录/类名」，这一点在 MiniEngine 上尤其成立。本项目当前的六层目录（`00_Core…05_Scene`）是**自己新加的**，不能因为目录存在就认为分层已经成立；必须用依赖关系证明（见 `02`）。
2. **它的「公共渲染功能」用极轻的约定实现**：每个效果是一个命名空间 + 3 个自由函数 `Initialize()` / `Shutdown()` / `Render(Context...)`，输入通过参数显式传入。
   → **启示**：这是约束原则④「明确输入输出约定」的一个低成本范本，比 WE 的「RenderPath 持有全部 RT + 虚函数覆写」更贴近本项目规模。
3. **它没有 RenderPath 概念**：不存在「可切换的渲染路径」抽象；编排逻辑在 `Model/Renderer.cpp` 里以过程式写法完成。
   → **启示**：应用层/渲染路径分离（原则①）在 MiniEngine 里**没有先例可抄**，必须以 WE 为主要参考，并按本项目规模裁剪。
4. **它把 GPU 资源与 D3D12 细节混在同一层**：`GpuResource.h` 直接以 `ID3D12Resource*` 为核心，上层可自由拿到裸接口。
   → **启示**：本项目当前 RHI 泄漏点（见 `02`）**是继承来的**，不是新引入的 bug。原则③要求收紧，但收紧的代价与收益必须逐点评估（见 A.6），不能整体推倒重写。

---

## A.5 MiniEngine 的渲染功能契约（原样摘录，作为原则④的参考模型）

上游各效果的公开签名（实测抓取，2026-10-24）：

- `Core/PostEffects.h`（47 行）：`void Initialize(void); void Shutdown(void); void Render(void); void CopyBackPostBuffer(ComputeContext& Context);`
- `Core/FXAA.h`（34 行）：`void Render(ComputeContext& Context, bool bUsePreComputedLuma);`
- `Core/TemporalEffects.h`（47 行）：`void Initialize(); void Shutdown(); void Update(uint64_t FrameIndex); uint32_t GetFrameIndexMod2(); void GetJitterOffset(float& JitterX, float& JitterY); void ClearHistory(CommandContext&); void ResolveImage(CommandContext&);`
- `Core/SSAO.h`（35 行）：`void Render(GraphicsContext& Context, const float* ProjMat, float NearClipDist, float FarClipDist);` 与 `void Render(GraphicsContext& Context, const Math::Camera& camera);` 与 `void LinearizeZ(ComputeContext& Context, const Math::Camera& camera, uint32_t FrameIndex);`
- `Core/DepthOfField.h`（28 行）：`void Render(CommandContext& BaseContext, float NearClipDist, float FarClipDist);`

**可直接借鉴的三点**：
1. 每个效果只有「初始化 / 关闭 / 执行」三个入口，状态藏在 .cpp 内部 —— 效果之间不互相调用，由上层编排。
2. 执行入口的输入是**显式参数**（命令上下文、投影矩阵、远近裁剪面、帧索引），不是「全局渲染器状态」。这让输入输出约定可读、可测。
3. 需要跨帧的状态（TAA 历史、帧索引奇偶）由效果自己持有并提供 `ClearHistory` / `GetFrameIndexMod2` 这类显式管理入口，而不是让上层直接摸内部 RT。

**不适用之处**：MiniEngine 的效果模块内部直接使用**全局单例**资源（`BufferManager` 里的全局后缓冲），输入并非完全显式；本项目若照抄需把「全局后缓冲」替换为显式传入的目标纹理，才能真正满足原则④的「明确输入输出约定」。

---

## A.6 结论：RHI 的最小可用抽象边界（建议）

由于 RHI 是继承来的，本附录的建议是**收敛而非重写**：

### A.6.1 必须保留（物理事实，收不掉）

- `ID3D12Device*`、命令队列、fence/信号量：D3D12 的硬性模型。
- 描述符堆的种类与容量（CBV_SRV_UAV / RTV / DSV / Sampler）与其可见性（shader-visible vs CPU-only）。
- 资源状态（`D3D12_RESOURCE_STATE_*`）与 barrier：**这是必须对上层可见的物理事实**。约束原则⑤明确「禁止用抽象隐藏同步关系」，因此不应把 barrier 藏进一个「自动 Graph」里（除非确实实现了资源状态跟踪——`Engine/Engine.md` 自述 `Resource State Tracking（未实现）`）。
- 显式堆类型选择（DEFAULT / UPLOAD / READBACK）与 residency。

### A.6.2 应当收紧（当前泄漏点，具体清单见 `02`）

- 上层不应直接调用 `ID3D12Device::CreateCommittedResource`；应经 `GpuResource`/`Texture`/`GpuBuffer` 的创建 API。
- 上层不应直接 `GetD3D12Resource()` 后自己写状态转换；应通过明确的 `Transition(resource, before, after, cmd)` 一类具名 API。
- 上层不应自建 `D3D12_ROOT_SIGNATURE_DESC`；应经 `RootSignature`/`PSOBuilder`。
- 资源创建参数的结构体（`TextureDesc` 一类）应当是与后端无关的 POD，而非直接暴露 `CD3DX12_RESOURCE_DESC`。

### A.6.3 应当砍掉（规模不匹配）

- 上游的多适配器/多厂商探测（`IsDeviceNvidia/AMD/Intel`）——本项目单机单卡教学用途，保留会增加分支而无收益（**但需先确认本仓库是否已保留**，见 `02`）。
- `EsramAllocator`（Xbox ESRAM）——平台特定，本项目不适用；上游有而本项目未采纳，方向正确。
- 任何为多后端（Vulkan/Metal）预留的抽象层——本项目单 D3D12 后端；WE 的 `wiGraphicsDevice` 多后端抽象属于「规模不匹配」，不要引入（详见 `04`）。

---

## A.7 未确认项

1. **未确认**：本仓库与上游 MiniEngine 是否存在**逐文件内容**继承（未做逐行 diff）。判定继承的依据是文件名/命名空间/仓库自述，属强证据但非逐行验证。确认方式：对 `Engine/02_RHI/Command/CommandContext.*` 与上游 `Core/CommandContext.*` 做 diff。
2. **未确认**：`Engine/02_RHI/GraphicsCore.h` 是否保留了上游的 `WKPDID_D3DDebugObjectName` 与 `RELEASE` 宏保护。确认方式：读该文件全文（42 行）。
3. **未确认**：`Core/MotionBlur.h`、`Core/ImageScaling.h` 的公开签名（抓取超时未取到）。
4. **未确认**：上游 `Core/Synchronization` 相关（MiniEngine 用 fence 值 + 事件，见 `CommandListManager`）与本仓库的差异。
5. **未确认**：`Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h` 是否逐字承袭上游 `Model/ConstantBuffers.h`；旧审计提到其纹理槽 enum 顺序有错配问题（`ConstantBuffers.h:18-26`），需在 `02` 中定位是继承缺陷还是改造引入。