# 01A_WE_应用层与渲染路径生命周期

> 作者：we-lifecycle（队友，共享任务 task-1）
> 对应原则：①应用层与渲染路径分离 · ⑤资源生命周期与 GPU 执行同步 · ⑥为未来管线保留扩展
> WE 源码基线：`D:\CS-Self-Study\Computer_Graphics\DX12\工程实现参考\WickedEngine-master\`（本地快照，非上游最新）
> TooiyumeRender 引用基线：工作树（HEAD=`51c09d09917a507e33ff7e468e824e34963339c5`，2026-10-16 19:38）；引用的两个差异文件已逐条比对，见 §11.1
> 证据规则：每条结论附 `文件路径:行号`；`[实测]`=本次调查在命令输出中直接看到的行；`[推断]`=由命名/调用点推测，且注明确认需要读哪个文件

---

## 0. 摘要：本报告可支撑的结论

1. **WE 的「应用层 / 渲染路径」分离是真实存在的，而且比约束文档设想的更薄**：`wi::Application`（≙ 文档中的 MainComponent，见 §1.2）只持有 `RenderPath*` 裸指针，二者之间只有 3 个接触面：`init(canvas)`、`colorspace` 赋值、12 个生命周期虚函数。`[实测]` `wiApplication.h:27`、`wiRenderPath.h:17-40`。
2. **切换点的真相与常见误解不同**：路径替换发生在 fade **全遮罩那一帧**，但触发位置是 `Update` 帧阶段（`wiApplication.cpp:287` → `wiFadeManager.cpp:38-45` 调 `onFade()`），**不是** Render/Compose 阶段；遮罩本身由应用层绘制（`wiApplication.cpp:450-469`）。`[实测]`
3. **旧 path 不会被 Application 销毁**：`ActivatePath` 只调 `activePath->Stop()`（`wiApplication.cpp:74-77`），delete 责任完全在外层持有者（Demo 里是按值成员 `Example_ImGui.h:15-23`，Editor 里是另一种持有方式）。`[实测]`
4. **全帧只有一次 GPU 提交**：`wiApplication.cpp:388`；RenderPath 内部**没有任何** `SubmitCommandLists`、没有 `WaitForGPU`，只有 `device->WaitCommandList(cmd, other)` 与 `device->Barrier(...)` 表达依赖。真正的 GPU 同步点在设备内部（帧末 fence 互等 `wiGraphicsDevice_DX12.cpp:5510-5525`、帧首三缓冲 fence 阻塞 `:5533-5546`）。`[实测]`
5. **resize 的完整链路是「窗口消息 → 重建/ResizeBuffers 交换链 → canvas 尺寸变化 → RenderPath2D::Update 的尺寸比较 → ResizeBuffers 重建全部 RT」**，其中 GPU 停顿出现在交换链侧（`wiGraphicsDevice_DX12.cpp:3108` `WaitForGPU()`），而 RT 侧只做「置空 + 重建」（`wiRenderPath3D.cpp:96-341`，无显式 fence，靠帧首三缓冲 fence 兜底）。`[实测]`
6. **HDR→LDR 的分界是一行**：`wiRenderPath3D.cpp:2473` 的 `renderer::Postprocess_Tonemap(...)`；其前是 HDR 链（`:2348-2445`），其后是 LDR 链（`:2496-2563`），ping-pong 用 `rt_read/rt_write` 两个指针表达（`:2341-2343`、`:2544`）。`[实测]`
7. **`RenderPath3D` 不是「一条管线」，而是一个「栅格管线全家桶基类」**：它同时是资源容器（约 40 个 RT）、参数集合（约 60 个开关）、公共功能调用者（约 25 个 `renderer::*Resources`）。PathTracing 只能「跳过基类资源 + 自写后处理链」来复用它（`wiRenderPath3D_PathTracing.cpp:142`、`:583-704`）。`[实测]`
8. **对 TooiyumeRender 的总体判断**：原则①的**接口形态**（薄基类 + 可切换路径 + 应用层持有点）必需；WE 的**实现规模**（3326 行基类、40 个 RT、~60 开关、aliasing allocator、~25 个 Resources 结构体）**规模不匹配**。建议只取「RenderPath 基类 + Application 持有 activePath + Start/Stop 绑定资源生命周期 + 单次提交」四点，其余留待真有多条路径时再长。`[推断]（依据为 §2-§10 实测 + §11.2 现状体积）`

---

## 1. 术语与两个前置事实

### 1.1 本报告使用的术语（与 00_总览 §6 一致）

| 术语 | 本报告的判定标准 |
| --- | --- |
| 应用层 | 持有窗口/设备/交换链/输入/主循环，并且**自己录制绘制命令**的层 |
| 渲染路径 | 一帧内「用哪些 RT、按什么顺序、调用哪些公共功能、产出最终图」的编排者 |
| 公共渲染功能 | 与「谁来编排」无关的单效果/单阶段能力（画场景、画阴影、画天空、单个后处理） |

### 1.2 MainComponent = Application（实测）

约束文档 §2.1 写「MainComponent 与 RenderPath 生命周期」；在本地 WE 中：

```cpp
// WickedEngine/WickedEngine.h:137
using MainComponent = wi::Application;
```

`[实测]` grep 全仓 `MainComponent` 仅此一处命中，它**只是类型别名，不是独立类**。因此文档这一条的真实对象就是 `wi::Application`。

### 1.3 本地 WE 版本的路径数量事实（实测）

- `wiRenderPath.h`（51 行，**没有 .cpp**，全部内联）、`wiRenderPath2D.h/.cpp`、`wiRenderPath3D.h/.cpp`、`wiRenderPath3D_PathTracing.h/.cpp` 是全部 `RenderPath` 家族文件；**不存在** `RenderPath3D_Deferred/_Forward/_Tiled`（与 00_总览 §3.3 的结论一致）。`[实测：目录列举 + 全仓 grep]`
- 全仓 `ActivatePath` 使用点 18 处（Editor 1、Samples 4、wiLoadingScreen 1、wiLua 1、BindLua 5、文档 6）。`[实测]`

---

## 2. 【问题 1】Application 与 RenderPath 的所有权与生命周期

### 2.1 所有权：单向、且 Application 不拥有路径

```cpp
// WickedEngine/wiApplication.h:24-27
std::unique_ptr<wi::graphics::GraphicsDevice> graphicsDevice;   // :24 独占
RenderPath* activePath = nullptr;                              // :27 裸指针，不拥有
```

`[实测]` 对照：
- **设备**：`unique_ptr` 独占，`Application::SetWindow` 创建（`wiApplication.cpp:728-818`），并把裸指针写进全局设备槽 `wi::graphics::GetDevice() = graphicsDevice.get();`（`wiApplication.cpp:819`）。
- **路径**：裸指针 + `GetActivePath()`（`wiApplication.h:68`），Application 的析构路径上**没有** `delete activePath`（`wiApplication.h` 无析构声明，`wiApplication.cpp` 无 `~Application`）。`[实测]`

这块的责任分配在官方文档里被明说（`Content/Documentation/WickedEngine-Documentation.md:293`）：

> "The `Application` has many uses that the user is not necessarily interested in. The most important part is that it manages the `RenderPath`s. There can be one active `RenderPath` at a time... because a RenderPath is a highly customizable class, there is no limitation what can be done within the RenderPath, for example supporting multiple concurrent RenderPaths if required."

→ **谁 new 谁 delete**：Application 只 `new` 自己的 device/swapchain/canvas/RT；路径对象由**应用层实例**（Demo 里是 `Application` 子类的按值成员，见 §9.1）持有。`[实测]`

### 2.2 ActivatePath：替换 + fade 的真实实现

```cpp
// WickedEngine/wiApplication.cpp:64-87（摘要）
void Application::ActivatePath(RenderPath* component, float fadeSeconds, wi::Color fadeColor, FadeManager::FadeType fadetype)
{
    component->init(canvas);                                   // :68 把应用层画布交给路径
    fadeManager.Start(fadeSeconds, fadeColor, [this, component]() {
        if (activePath) activePath->Stop();                    // :74-77 旧路径停止（释放 RT，见 §5.1）
        component->Start();                                    // :79-82 新路径启动（重建 RT，见 §5.1）
        activePath = component;                                // :83 指针替换
    }, fadetype);
    fadeManager.Update(0);   // :86 "If user calls ActivatePath without fadeout, it will be instant"
}
```

`[实测]` 三条要点：
1. `component->init(canvas)` 在 fade **之前**立即执行（尺寸/DPI 先就位，避免切换瞬间拿到 0 尺寸）。
2. 真正的替换被封装成一个 `std::function` 存进 `FadeManager::onFade`（`wiFadeManager.h:30`），由帧循环推进。
3. `fadeManager.Update(0)`（`:86`）使 `fadeSeconds=0` 的情形**当场完成**——这是 `ActivatePath(&renderer)`（`Example_ImGui.cpp:157`）瞬时生效的原因，不需要等下一帧。`[实测：wiFadeManager.cpp:18-24]`

### 2.3 淡入淡出在帧循环的哪一段完成切换

```cpp
// WickedEngine/wiFadeManager.cpp:26-45（摘要）
float t = timer / targetFadeTimeInSeconds;                     // :26
timer += Clamp(dt, 0, 0.033);                                  // :27
switch (state) {
  case FADE_IN:  ... opacity 0→1 ...                           // :31-36
  case FADE_MID:
      state = FADE_OUT; opacity = 1;                            // :38-41
      onFade();                                                 // :43 ← 这里完成 Stop/Start/替换
      timer = 0; fadeEventTriggeredThisFrame = true;            // :44-45
  ...
}
```

调用点 `Application::Run()`：

```cpp
// WickedEngine/wiApplication.cpp:287
fadeManager.Update(deltaTime);   // ← Update 帧阶段，介于 input::Update 与 activePath->PreUpdate 之间
```

`[实测]` 因此 **切换发生在 Update 阶段的中途**：同一帧内先 `fadeManager.Update`（旧路径 Stop → 新路径 Start → 指针替换），随后 `:289-295` 才 `activePath->PreUpdate()`、`:294` 之前还有 `activePath->init(canvas)`，所以**新路径在同帧就会被 PreUpdate/Update/Render/Compose**，旧路径同帧不再被调用。遮罩绘制在 `Application::Compose`（`:450-469`）里用应用层的命令列表画全屏方块/上一帧背景纹理，与路径无关。

`[推断，已核对注释]` 设计动机：把「谁替换谁」从渲染阶段挪到更新阶段，可以保证**同一帧内不发生「渲染中途换路径」**——渲染开始时 `activePath` 已稳定。确认这一意图可读 `wiFadeManager.h:41-65` 的注释与 `wiApplication.cpp:285-295` 的相邻顺序（本报告未读到显式文字说明，标注为推断）。

### 2.4 旧路径何时被销毁

- **释放 GPU 资源：立即**。`RenderPath3D::Stop()` 整个函数体就是 `DeleteGPUResources()`（`wiRenderPath3D.cpp:1816-1819`）。`[实测]`
- **销毁对象：由外部所有者决定，WE 不规定**。Demo 的 `renderer` 是 `Example_ImGui` 的值成员（`Example_ImGui.h:15-23`），随 Application 子类一起析构；Editor 是另一种持有方式（`Editor/Editor.cpp:360` 调用点，持有细节未读，**未确认**，需读 `Editor/Editor.cpp` 中 `renderComponent` 的声明）。
- 由此产生一个**风险点（对 TooiyumeRender 直接相关）**：`Stop()` 会 `DeleteGPUResources()` 把全部 RT 置空，若此时 GPU 仍在用（上一帧命令已提交但未完成），释放是否安全取决于设备侧的延迟释放队列——WE 依赖 `GetDevice()->FlushDeallocations()`（`wiRenderPath2D.cpp:21`）与帧首 fence（`wiGraphicsDevice_DX12.cpp:5533-5546`）。`[推断]` 确认需要读 `wiGraphicsDevice_DX12.cpp` 的 `DestroyResource`/deferred deallocation 路径（本报告未读，见 §12）。

---

## 3. 【问题 2】四个核心对象的归属 + Run() 一帧的真实调用顺序

### 3.1 归属表（全部实测）

| 对象 | 持有者 | 声明/创建点 | 备注 |
| --- | --- | --- | --- |
| `graphicsDevice` | `Application`（`unique_ptr`） | 声明 `wiApplication.h:24`；创建 `wiApplication.cpp:728-818` | 同时把裸指针写进全局 `wi::graphics::GetDevice()`（`:819`），**渲染路径不通过 Application 取设备** |
| `swapChain` | `Application`（public 值成员） | 声明 `wiApplication.h:57`；创建/Resize `wiApplication.cpp:832-850` | 只有 Application 会 `RenderPassBegin(&swapChain)`（`:370`、`:378`），这是「谁 Present」的判据（§3.4） |
| `canvas` | `Application`（public 值成员） | 声明 `wiApplication.h:58`；`init` `wiApplication.cpp:821-830` | 可被复制进 RenderPath（RenderPath 继承 Canvas，`wiRenderPath.h:8`），每帧 `:293 activePath->init(canvas)` 同步 |
| `window` | `Application`（public 值成员） | 声明 `wiApplication.h:59`；`SetWindow` 入口 `wiApplication.cpp:723-725` | 窗口消息→`SetWindow`→重建/Resize 交换链（§7） |
| `fadeManager` / `infoDisplay` / `backlog`(全局) | `Application` | `wiApplication.h:34`、`:107-141` | fade 与调试 HUD 都是应用层职责 |
| `activePath` 指向的对象 | **外部**（Demo 的按值成员等） | `wiApplication.h:27` | Application 不 delete |

`[实测]` 关键结构判断：**这是「设备/交换链/窗口在应用层，RT/管线在路径层」的两段式**。路径层拿设备靠全局函数 `GetDevice()`，拿尺寸靠继承的 `Canvas`。这让路径层与 Application **零头文件依赖**（§4.3）。

### 3.2 Run() 一帧的真实顺序（逐条行号，全部实测）

| # | 行号 | 动作 | 帧阶段 |
| --- | --- | --- | --- |
| 1 | `wiApplication.cpp:91-96` | 懒初始化 `Initialize()` | 前置 |
| 2 | `:98` | `font::UpdateAtlas(canvas.GetDPIScaling())` | 前置 |
| 3 | `:100-134` | HDR10 时按需建 `rendertargetPreHDR10`（R11G11B10_FLOAT）否则置空 | 前置（资源） |
| 4 | `:136-220` | **初始化未完成分支**：只画启动屏并 `:218 SubmitCommandLists` + `:219 return` | 早退分支 |
| 5 | `:222` | `splash_screen = {}` | — |
| 6 | `:224-256` | 首次执行 startup 脚本（lua/luab/rewriteable） | 一次性 |
| 7 | `:258-266` | 窗口失活且非 alwaysactive → `Sleep(10)` + `input::Update` + return | 早退分支 |
| 8 | `:268` | `profiler::BeginFrame()` | 前置 |
| 9 | `:270` | `deltaTime = timer.record_elapsed_seconds()` | 时间 |
| 10 | `:272-280` | framerate_lock 时 QuickSleep；`clamp(dt, 0, 0.5)` | 时间 |
| 11 | `:282` | `wi::input::Update(window, canvas)` | 输入 |
| 12 | `:285` | `eventhandler::FireEvent(EVENT_THREAD_SAFE_POINT, 0)` | 事件 |
| 13 | `:287` | **`fadeManager.Update(deltaTime)`** ← 路径切换可能在此发生 | 应用层更新 |
| 14 | `:289-295` | `activePath->colorspace = GetSwapChainColorSpace()`；`activePath->init(canvas)`；**`activePath->PreUpdate()`** | 路径更新 |
| 15 | `:297-321` | FixedUpdate 循环（frameskip：`while (deltaTimeAccumulator >= 1/targetFrameRate) FixedUpdate()`）→ 内部 `:418-421 activePath->FixedUpdate()` | 固定步长 |
| 16 | `:324` | `Update(deltaTime)` → 内部 `:398-403` lua/backlog/资源流送；`:405-409` **`activePath->Update(dt); activePath->PostUpdate();`** | 变量步长 |
| 17 | `:326` | `Render()` → 内部 `:428` lua；`:430-435` **`activePath->PreRender(); activePath->Render(); activePath->PostRender();`** | 渲染（录制） |
| 18 | `:329` | `cmd = graphicsDevice->BeginCommandList()`（最终合成命令列表） | 合成 |
| 19 | `:332-355` | CrossFade 时保存当前 backbuffer 到 `crossFadeTexture`（PushBarrier+FlushBarriers+CopyResource） | 合成 |
| 20 | `:357-362` | `image::SetCanvas`/`font::SetCanvas`；按 swapChain 尺寸 `BindViewports` | 合成 |
| 21 | `:364-371` | `RenderPassBegin(rendertargetPreHDR10 或 swapChain, cmd)` | 合成 |
| 22 | `:372` | **`Compose(cmd)`** → 内部 `:445-448` `activePath->Compose(cmd)`；`:450-469` fade 遮罩；`:472-709` infoDisplay HUD；`:711-713` profiler/backlog | 合成 |
| 23 | `:373` | `RenderPassEnd(cmd)` | 合成 |
| 24 | `:375-384` | HDR10 时第二次 pass：`activePath` 的输出映射进 swapChain | 合成 |
| 25 | `:386-387` | `input::ClearForNextFrame()`；`profiler::EndFrame(cmd)` | 收尾 |
| 26 | `:388` | **`graphicsDevice->SubmitCommandLists()`** ← 全帧唯一提交（含 Present，见 §3.4） | 提交 |
| 27 | `:389` | `renderer::UpdateGPUSuballocator()` | 收尾 |

`[实测]` 结论：
- **Application 的帧顺序内没有任何 fence/wait**（`grep WaitForGPU` 全仓调用者只有 Editor/内部辅助，见 §6.4）。
- **PostRender 在 Render 之后、Compose 之前**（`:430-435` 三步连调），Compose 是「把路径产出的图像映射到应用层 RT/交换链」的钩子。
- 「Present」不是 Application 的一行代码，而是 `:370/:378 RenderPassBegin(&swapChain)` 的副作用：它把 swapchain 登记到该命令列表，`:5473 commandlist.swapchains` 驱动设备在提交后 Present。`[实测]`

### 3.3 Update / FixedUpdate / Render / Compose 的钩子内容（摘要）

- `Application::Update`：`lua::SetDeltaTime`、`lua::Update`、`backlog::Update`、`resourcemanager::UpdateStreamingResources`，然后 `activePath->Update/PostUpdate`（`:398-409`）。
- `Application::FixedUpdate`：`lua::FixedUpdate` + `activePath->FixedUpdate`（`:416-421`）。
- `Application::Render`：`lua::Render` + `PreRender/Render/PostRender`（`:428-435`）。
- `Application::Compose`：`activePath->Compose` + fade + HUD + profiler + backlog（`:443-713`）。

→ **应用层自己的绘制有 3 个位置**：启动屏（`:136-220`）、fade/HUD（`:450-709`）、以及子类覆写 `Compose` 追加（`Example_ImGui.cpp:160-310` 用 `device->AllocateGPU` 动态缓冲直接录 ImGui）。`[实测]`

### 3.4 提交与 Present 的真实位置（实测）

```cpp
// WickedEngine/wiGraphicsDevice_DX12.cpp:5388 SubmitCommandLists()
:5396-5407  cmd_last = cmd_count; cmd_count = 0; 逐命令列表 Close()
:5458-5468  frame_fence_values[GetBufferIndex()]++; 每队列 submit() 并 Signal(frame_fence)
:5470-5507  逐命令列表，对 commandlist.swapchains 里每个交换链 Present(...)   // :5495
:5510-5525  每个队列 Wait 其它队列的 frame_fence（注释：disables overlapping queues into the next frame）
:5531-5546  FRAMECOUNT++；对下一个 bufferindex 的 frame_fence 做 CPU 阻塞等待（三缓冲）
```

`[实测]` 因此「Present 在 SubmitCommandLists 里，与命令列表的 swapchain 标记绑定」；「GPU 同步在设备内部，应用层与路径层都看不见」。

---

## 4. 【问题 3】RenderPath 的虚接口清单、耦合面、反向依赖

### 4.1 接口清单（`wiRenderPath.h`，全部**非纯虚、默认空实现**）`[实测]`

| 行号 | 签名 | 语义 |
| --- | --- | --- |
| `:8` | `class RenderPath : public wi::Canvas` | **继承 Canvas**：路径自带尺寸/DPI/scaling，无需 Application 传入尺寸 |
| `:11` | `uint32_t layerMask = 0xFFFFFFFF`（private） | 可见性层掩码（`RenderPath3D.cpp:385` 传给 `renderer::UpdateVisibility`） |
| `:17` | `virtual void Load()` | 资源装载模态（Editor 用，见 `wiLoadingScreen`） |
| `:19` | `virtual void Start()` | 激活时（fade 回调里）调用 |
| `:21` | `virtual void Stop()` | 失活时调用 |
| `:23` | `virtual void PreUpdate()` | 每帧最早 |
| `:25` | `virtual void FixedUpdate()` | 固定步长 |
| `:28` | `virtual void Update(float dt)` | 变量步长 |
| `:30` | `virtual void PostUpdate()` | 更新收尾 |
| `:32` | `virtual void PreRender()` | 渲染前资源准备 |
| `:35` | `virtual void Render() const` | 录制渲染命令 |
| `:37` | `virtual void PostRender()` | 渲染收尾 |
| `:40` | `virtual void Compose(wi::graphics::CommandList cmd) const` | 合成到应用层目标 |
| `:42-43` | `getLayerMask/setlayerMask` | — |
| `:45` | `wi::graphics::ColorSpace colorspace = SRGB` | 由 Application 每帧写（`wiApplication.cpp:292`） |
| `:48-49` | `static constexpr script_check_identifier = relative_path_storage(__FILE__); virtual const char* GetScriptBindingID() const` | Lua 绑定类型标识 |

**没有纯虚函数**（`=` 0 一个也没有）。`[实测：通读 51 行]`

### 4.2 Application 依赖 RenderPath 的成员（耦合面清单，实测）

`grep` 全部 `application_refs` + 通读 `wiApplication.cpp` 后，Application→RenderPath 的接触面**只有**：

1. `activePath` 指针本身（`wiApplication.h:27`）与 `GetActivePath()`（`:68`）。
2. `init(canvas)`（`wiApplication.cpp:68`、`:293`）——**来自 Canvas 基类，不是 RenderPath 自己的接口**。
3. `colorspace` 成员赋值（`:292`）。
4. 12 个虚函数调用：`Stop/Start`（`:74/:79`）、`PreUpdate`（`:294`）、`FixedUpdate`（`:419`）、`Update/PostUpdate`（`:406/:407`）、`PreRender/Render/PostRender`（`:431-433`）、`Compose`（`:446`）。
5. Lua 绑定侧：`GetActivePath()` + `GetScriptBindingID()`（`wiApplication_BindLua.cpp:53-96`）。

→ **不需要**：RenderPath 的 RT、参数、scene、device、任何 getter。Application **完全不知道**路径内部有什么。`[实测]`

### 4.3 是否存在 RenderPath→Application 的反向依赖

**主链路：不存在。** `[实测]` grep `#include "wiApplication.h"` 全仓只有 5 处：`WickedEngine.h:13`（聚合头）、`wiLua.h:5`、`wiLoadingScreen.cpp:2`、`wiApplication_BindLua.h:4`、`wiApplication.cpp:1`。**三个 `wiRenderPath*.h` 都不 include 它**，也不含 `Application*` 成员。

**但有两个例外（重要，对目标的适用性判断有影响）**：

| 例外 | 证据 | 性质 |
| --- | --- | --- |
| `wi::LoadingScreen`（一个 2D 路径）反过来持有 `Application*` | `wiLoadingScreen.h:37` `addLoadingComponent(RenderPath* component, Application* main, ...)`；`wiLoadingScreen.cpp:39`；`:45` 内调 `main->ActivatePath(component,...)` | **「加载屏」是一种应用层设施伪装成路径**：它是唯一需要反向指针的路径 |
| Lua 桥 `wiLua::EnableEditorFunctionality(Application*, RenderPath*)` | `wiLua.h:206`、`wiLua.cpp:57`、`wiLua.cpp:794` | 编辑器脚本需要同时拿到两者，属于工具层 |

`[实测]` 结论：`RenderPath` 基类本身零反向依赖；**反向依赖只出现在「特殊的路径（LoadingScreen）」与「工具桥（Lua/Editor）」**。这正是「允许专用实现、不强迫共用不合理接口」（原则②）的体现：WE 没有为了 LoadingScreen 往基类塞 `Application*`。

### 4.4 路径层如何拿设备与画布（替代了反向依赖）

- 设备：全局函数 `wi::graphics::GetDevice()`，由 `wiApplication.cpp:819` 赋值。`[实测]`
- 画布：继承 `Canvas`（`wiRenderPath.h:8`），每帧由 Application 复制（`wiApplication.cpp:293`）。
- 场景/相机：全局单例 `wi::scene::GetScene()/GetCamera()`（`wiScene.h:650-664`，注释明确写 "You don't need to use it, but it's an option for simplicity"），在 `wiRenderPath3D.h:197-204` 取地址缓存。

→ **代价**：路径层「天生可用全局设备与全局场景」，所以它才能在**没有 Application 参数**的前提下工作；代价是**隐式全局状态**（原则④明确要避免的那类）。`[推断，依据上述实测行]`

---

## 5. 【问题 4】RenderPath2D 的职责

### 5.1 它是什么（实测）

`wiRenderPath2D.h:14` `class RenderPath2D : public RenderPath`。它把「渲染路径」从抽象钩子落地成**一条具体的 2D 合成链**，职责有四块：

**(a) 2D 合成层 + RT 生命周期**

```cpp
// wiRenderPath2D.h:17-33
Texture rtStencilExtracted, stencilScaled, rtFinal, rtFinal_MSAA;
wi::gui::GUI GUI;
XMUINT2 current_buffersize; float current_layoutscale;
float hdr_scaling = 9.0f;  uint32_t msaaSampleCount = 1, msaaSampleCount2D = 1;
wi::vector<wi::video::VideoInstance*> video_decodes;
// :37-41 新增三个资源生命周期虚函数
virtual void DeleteGPUResources(); virtual void ResizeBuffers(); virtual void ResizeLayout();
```

- `ResizeBuffers()`（`wiRenderPath2D.cpp:24-82`）：建 `rtFinal`（`R8G8B8A8_UNORM`，RT|SRV；MSAA 时另建瞬态 `rtFinal_MSAA`，`:59-81`）；若 `resolutionScale != 1` 或采样数不匹配，另建 stencil 提取/缩放两张（`:34-52`，注释说明 `D24 was erroring on Vulkan AMD`）。
- `DeleteGPUResources()`（`:11-22`）：尺寸缓存清零（`current_buffersize={}`、`current_layoutscale=0`）+ 四张 RT 置空 + `GetDevice()->FlushDeallocations()`（`:21`，「延迟释放」）。
- `ResizeLayout()`（`:83-86`）：只更新 DPI 缩放。

**(b) GUI 与 2D 图层**

- `wi::gui::GUI GUI`（`h:20`）、`GetGUI()`（`h:122-123`）。
- 图层模型：`struct RenderLayer2D { vector<RenderItem2D> items; string name; int order; }`（`h:95-99`），`wi::vector<RenderLayer2D> layers{1}`（`h:99`）；`AddLayer/SetLayerOrder/SetSpriteOrder/SetFontOrder/SortLayers/CleanLayers/DeleteLayers`（`h:102-120`），排序是冒泡（`cpp:484-522`）。
- `Render()` 里：`image::SetCanvas(*this)`（`cpp:184-185`）→ 组 RenderPass → 遍历 layers 画 sprite/font（`:257-282`，stencil 缩放只在第一个需要 stencil 的 sprite 前做一次 `renderer::ScaleStencilMask`）→ `GetGUI().Render(*this, cmd)`（`:284`）。

**(c) 视频解码调度**

- 视频实例收集到 `video_decodes`（`cpp:102-124`），`Render()` 里用独立 `QUEUE_VIDEO_DECODE` 命令列表解码（`:170-180`），再 `device->WaitCommandList(cmd, video_cmd)`（`:189`）与主列表汇合。`[实测]`

**(d) 3D 结果的接入点（向上暴露 + 向下索取）**

- 向下：`GetDepthStencil()`（`h:63`，默认 `nullptr`）、`GetGUIBlurredBackground()`（`h:66`，默认 `nullptr`）——**基类把「没有 3D」表达为返回空指针**，而不是让 2D 路径也持有深度图。
- 向上：`GetRenderResult2D()`（`h:60`，非虚，返回 `rtFinal`）。

### 5.2 3D 结果 + GUI 如何合成（实测）

```cpp
// wiRenderPath2D.cpp:182-290 Render() 摘要
cmd = device->BeginCommandList();                        // :182
image::SetCanvas(*this); font::SetCanvas(*this);         // :184-185
device->WaitCommandList(cmd, video_cmd);                 // :189
renderer::ProcessDeferredTextureRequests(cmd);           // :198
if (GetGUIBlurredBackground() != nullptr)                // :200  ← 3D 路径交出模糊背景
    image::SetBackground(*GetGUIBlurredBackground());    // :201
dsv = GetDepthStencil();                                 // :205  ← 3D 路径交出深度
if (rtStencilExtracted.IsValid()) renderer::ExtractStencil(...)  // :207-210
// :212-241 RenderPassImage：rtFinal_MSAA + Resolve(&rtFinal) 或直接 rtFinal；stencilScaled 或原生 dsv
// :257-282 画 2D 图层
GetGUI().Render(*this, cmd);                             // :284
RenderPassEnd(cmd);                                      // :286
```

```cpp
// wiRenderPath2D.cpp:292-310 Compose() 摘要
fx.enableFullScreen(); fx.blendFlag = BLENDMODE_PREMULTIPLIED;   // :297-299
if (colorspace != SRGB) fx.enableLinearOutputMapping(hdr_scaling);  // :300-304
wi::image::Draw(&GetRenderResult2D(), fx, cmd);                   // :305  把 rtFinal 画到应用层目标
```

→ **合成关系**：`RenderPath3D::Compose` 先把 3D 最终图（HDR/LDR 后处理产物）画到应用层 RT，然后调 `RenderPath2D::Compose` 把 `rtFinal`（2D 图层 + GUI，pre-multiplied）叠上去（`wiRenderPath3D.cpp:1788-1814`）。**3D→2D 是「先 Compose 3D 结果，再叠加 2D 层」的两段式，不是把 2D 塞进 3D 的 RenderPass**。`[实测]`

### 5.3 「向上暴露渲染结果」的虚函数设计意图（实测 + 推断）

三个函数构成一套**最小结果契约**：

| 函数 | 返回 | 时机 | 意图（推断，依据为调用点） |
| --- | --- | --- | --- |
| `GetRenderResult2D()`（非虚） | `const Texture&`（`rtFinal`） | `ResizeBuffers` 之后始终有效 | 2D 结果的**固定契约**：2D 是所有路径的最小公共输出 |
| `GetRenderResult3D()`（`wiRenderPath3D.h:235`） | `const Texture&`（`*GetLastPostprocessRT()`） | `RenderPostprocessChain` 之后（`:2544`） | 3D 结果的**可变契约**：因为后处理 ping-pong，最终 RT 会换，所以返回指针解引用而非固定成员 |
| `GetDepthStencil()`（`h:63`/`wiRenderPath3D.h:238`） | `const Texture*`，默认 nullptr | `PreRender` 后 | 让 2D 层做 stencil 裁剪；纯 2D 路径回答「没有」 |
| `GetGUIBlurredBackground()`（`h:66`/`wiRenderPath3D.h:241`） | `const Texture*`，默认 nullptr | 后处理链末尾写入（`:2546-2556`） | 让 GUI 背景模糊；纯 2D 路径回答「没有」 |

`[推断]` 「默认返回 nullptr 而不是纯虚」是**刻意的规模控制**：2D 路径不必实现 3D 概念，3D 路径也不必关心 2D 是否需要深度。这比「在基类声明纯虚并让 2D 路径返回空实现」更省事，也让新增路径可以只实现真正相关的部分。确认这一意图可直接读 `wiRenderPath2D.h:60-66` 的默认实现（已完成）。

---

## 6. 【问题 5】RenderPath3D::Render 全流程骨架 + 命令列表来源 + 提交/同步

### 6.1 命令列表从哪来（实测）

**全部来自同一个设备方法**：`graphicsDevice->BeginCommandList()`（即 `wi::graphics::GetDevice()->BeginCommandList()`），可选传入队列类型（`QUEUE_COMPUTE`/`QUEUE_COPY`/`QUEUE_VIDEO_DECODE`）。`RenderPath3D::Render` 内创建的列表（实测行号）：

| 局部变量 | 行号 | 队列 | 用途 |
| --- | --- | --- | --- |
| `cmd_prepareframe` | `:860-880` | GRAPHICS | 每帧准备（BindCameraCB、UpdateRenderData、barriers） |
| `cmd_prepareframe_async` | `:883-927` | COMPUTE | 异步准备（UpdateRenderDataAsync/湿地图/加速结构/SurfelGI/DDGI） |
| `cmd_maincamera_prepass` | `:939-1022` | GRAPHICS | 深度预通道 |
| `cmd_maincamera_compute_effects` | `:1027-1134` | COMPUTE | AO/SSR/SSGI/RTShadow/VRS 分类 |
| `cmd_occlusionculling` | `:1137-1179` | GRAPHICS | 遮挡剔除查询 |
| `cmd`（平面反射深度） | `:1182-1247` | GRAPHICS | 反射深度 |
| `cmd_ocean` | `:1249-1265` | COMPUTE+COPY | 海洋 |
| `cmd_shadowmap` | `:1268-1277` | GRAPHICS | 阴影图 |
| `cmd_vxgi` | `:1279-1285` | COMPUTE | VXGI 体素化 |
| `cmd_updatetextures` | `:1288-1310` | COPY | 光照图/环境探针/贴花 |
| `cmd`（反射颜色） | `:1313-1425` | GRAPHICS | 反射通道 |
| `cmd`（不透明颜色） | `:1427-1602` | GRAPHICS | 主通道 |
| `cmd`（天气异步） | `:1604-1650` | COMPUTE | 体积云/空气透视 |
| `cmd_postprocess` | `:1756-1773` | GRAPHICS | 后处理链 |
| `cmd`（流送回读） | `:1775-1779` | COPY | 纹理流送回读 |
| `video_cmd` | `:796-813`（`wiRenderPath3D.h:171` 成员） | VIDEO_DECODE | 视频 |

→ **一条帧内有 15+ 个命令列表，跨 4 种队列**。这是 WE 的「多队列利用率」策略，也是它体积大的直接原因之一。`[实测]`

### 6.2 Render 阶段骨架（按顺序，实测行号区间）

```
:834-842   const 方法与 mutable 状态：!prerender_happened → assert + const_cast 调 PreRender；prerender_happened=false
:845       wi::jobsystem::context ctx（每帧一个 CPU job 上下文）
:847-857   地形虚拟纹理 copypages（QUEUE_COPY，jobsystem::Execute）
:860-880   cmd_prepareframe：ProcessDeferredTextureRequests（注释：主线程第一件事）→ jobsystem::Execute{ BindCameraCB; UpdateRenderData; Barrier(debugUAV→UAV)+Barrier(Aliasing(&rtPostprocess,&rtPrimitiveID)) }
:883-927   cmd_prepareframe_async（COMPUTE）：WaitCommandList(cmd, cmd_prepareframe) + UpdateRenderDataAsync / 湿地图 / 加速结构 / SurfelGI / DDGI
:929-936   drawscene_flags = OPAQUE|IMPOSTOR|HAIRPARTICLE|TESSELLATION|OCCLUSIONCULLING|MAINCAMERA
:939-1022  主相机深度预通道：RenderPassImage{depthBuffer_Main, rtPrimitiveID} + DrawScene ×2
:1027-1134 主相机 compute 效果：可见性准备 / Tiled 光剔除 / AO / VRS 分类 / SSR / SSGI / RTShadow / MeshBlend 边缘
:1137-1179 遮挡剔除（RenderPassBegin(&scene->queryHeap)）
:1182-1247 平面反射深度预通道
:1249-1265 海洋（COMPUTE→COPY 回读）
:1268-1277 阴影贴图（DrawShadowmaps）
:1279-1285 VXGI 体素化
:1288-1310 纹理更新（光照图/环境探针/贴花）
:1313-1425 平面反射颜色通道
:1427-1602 主相机不透明颜色通道：RT 反射/RT 漫反射/VXGI 解析/Visibility_Shade → outline source → RenderPassBegin(ALLOW_UAV_WRITES) → DrawScene/DrawSky/RenderOutline → Barrier(Aliasing(&rtAO,&rtParticleDistortion))
:1604-1650 天气异步 compute
:1652-1706 不透明 part2（合成天气 或 Resolve MSAA）
:1708-1727 地形虚拟纹理 tile 分配
:1729-1752 透明段：RenderLightShafts / RenderVolumetrics / RenderTransparents
:1754      RenderCameraComponents(ctx)
:1756-1773 cmd_postprocess：WaitCommandList(cmd, video_cmd) → RenderPostprocessChain(cmd) → 视频 Resolve → Barrier(debugUAV)
:1775-1779 COPY 队列：TextureStreamingReadbackCopy
:1781      RenderPath2D::Render()          ← 2D 图层 + GUI 合成到 rtFinal
:1783      wi::jobsystem::Wait(ctx)         ← 唯一同步点（CPU job，不是 GPU）
:1785      first_frame = false
```

`[实测]` **关键否定结论**：`Render()` 内**没有** `SubmitCommandLists()`、**没有** `WaitForGPU()`、**没有** fence 创建。

### 6.3 GPU 同步的真实位置与形式（实测）

| 形式 | 位置 | 作用 |
| --- | --- | --- |
| `device->WaitCommandList(cmd, other_cmd)` | `:884`、`:1006(:1027 附近)`、`:1269`、`:1385(:1313 段)`、`:1429`、`:1730`、`:1757`、`:1776` 等 | **队列内/跨队列依赖**：把「前一个列表」作为该列表的前置。设备提交时会把带依赖的列表前后各提交一次以提高利用率（`wiGraphicsDevice_DX12.cpp:5411-5445` 注释） |
| `device->Barrier(...)` | `:1596-1599`、后处理链 `:2346`、`:1756-1773` 段等 | 资源状态转换 + **显存别名（Aliasing）** |
| 帧末队列互等 | `wiGraphicsDevice_DX12.cpp:5510-5525` | 每队列 Signal `frame_fence`，再等其它队列（注释：这会使队列无法跨帧重叠） |
| 帧首三缓冲 fence 阻塞 | `wiGraphicsDevice_DX12.cpp:5531-5546` | `FRAMECOUNT++` 后对下一个 buffer 的 frame_fence 做 CPU 等待 |
| `WaitForGPU()` | `wiGraphicsDevice_DX12.cpp:3108`（交换链重建时） | 全仓调用者：Editor 1 处、设备内部 2 处、helper 2 处；**Application 帧循环从不调用** |
| `jobsystem::Wait(ctx)` | `wiRenderPath3D.cpp:1783` | **CPU job 线程同步，不是 GPU** |

→ 对原则⑤的启示：**WE 把「资源生命周期 vs GPU 执行」的同步责任全部收进设备层**（frame_fence + 延迟释放队列 + Barrier/WaitCommandList），路径层只负责「声明依赖」。`[实测 + 推断]`

### 6.4 Render 的可覆写子阶段（`wiRenderPath3D.h:184-192`，实测）

`RenderAO`（cpp:1826）、`RenderSSR`（:1881）、`RenderSSGI`（:1894）、`RenderOutline`（:1909）、`RenderLightShafts`（:1922）、`RenderVolumetrics`（:2030）、`RenderSceneMIPChain`（:2055）、`RenderTransparents`（:2084）、`RenderPostprocessChain`（:2334）——**全部 virtual 非纯虚，带默认实现**。这九个是「管线之间的真正差异点接口」，也是 PathTracing 唯一能选择性复用/覆写的地方（§9.2）。

---

## 7. 【问题 6】窗口 resize → 各 RT 重建的完整调用链

### 7.1 链路（实测）

```
[平台] WM_SIZE
  └─(Samples/Template_Windows/main.cpp:15-19) application.SetWindow(hWnd)
       └─ Application::SetWindow(wiApplication.cpp:723-866)
            :725  this->window = window
            :728-818  device 仅首次创建（已存在则跳过）
            :819  GetDevice() = graphicsDevice.get()
            :821-830  canvas.init(renderWidth/H) 或 canvas.init(window)   ← 画布尺寸刷新（wiCanvas.h:38-47 读窗口属性）
            :832-845  swapChain.desc：首次填 buffer_count=3 与格式；尺寸来自 canvas
            :846-850  graphicsDevice->CreateSwapChain(&desc, window, &swapChain)   ← 无条件调用
            :852  rendertargetPreHDR10 = {}
            :859-864 订阅 EVENT_SET_VSYNC（后续 vsync 变化也走 CreateSwapChain）
                 └─ GraphicsDevice_DX12::CreateSwapChain(wiGraphicsDevice_DX12.cpp:3097-…)
                      :3099-3102 复用内部状态、写回 desc
                      :3105-3114 若已有后台缓冲：WaitForGPU() 后强制 Reset 旧纹理   ← resize 的 GPU 停顿点
                      :3178-3232 swapChain==nullptr → CreateSwapChainForHwnd（首建）
                      :3233-3244 否则 → swapChain->ResizeBuffers(buffer_count, width, height, format, flags)
                      :3246-3294 颜色空间（HDR/SDR）设置
                      :3296-…    重建每个后台缓冲的 RTV/SRV
[下一帧] Application::Run()
  :287  fadeManager.Update
  :289-295  activePath->colorspace / activePath->init(canvas)（Canvas 尺寸随之更新）/ activePath->PreUpdate()
  :324  Update → activePath->Update(dt)
       └─ RenderPath3D::Update(wiRenderPath3D.cpp:349-373)
            :353  RenderPath2D::Update(dt)
                 └─ wiRenderPath2D.cpp:90-94
                      if (GetInternalResolution() != current_buffersize) ResizeBuffers();   ← resize 检测点
                      :95-98 DPI 变化 → ResizeLayout()
       └─ RenderPath3D::ResizeBuffers(wiRenderPath3D.cpp:96-341)      ← 全部 3D RT 重建
  :326  Render → activePath->PreRender()
       └─ wiRenderPath3D.cpp:379-382  二次保险：rtMain_render 采样数与 msaaSampleCount 不符 → 再 ResizeBuffers()
       └─ wiRenderPath2D.cpp:149-153  二次保险：尺寸检查 → ResizeBuffers()
```

`[实测]` 两个「双保险」是必要的，因为 `:290-294` 的 `init(canvas)` 与 `Update` 同帧发生，而 `PreRender` 可能在别的调用序列中被单独触发（`Render` 里 `:834-841` 甚至用 `const_cast` 兜底调 `PreRender`）。

### 7.2 RenderPath3D::ResizeBuffers 的新旧尺寸对应关系（实测）

```cpp
// wiRenderPath3D.cpp:96-341（摘要，行号为实测）
:98   first_frame = true;
:99   DeleteGPUResources();                     // 先全释放
:103-105 internalResolution；写回 camera->width/height
:108  device->CreateAliasingAllocator();        // 别名分配器（每次 resize 重建）
:111-161 定义别名纹理 desc（尺寸以 internalResolution 为基准）
        rtPostprocess      = format_rendertarget_main, 全分辨率, RT|SRV|UAV
        rtPrimitiveID      = format_idbuffer, 全分辨率, RT|SRV(+UAV if MSAA), layout=SHADER_RESOURCE_COMPUTE
        rtSceneCopy        = format_rendertarget_main, 1/4 分辨率, mip=min(8, log2(max))
        rtParticleDistortion = R16G16_FLOAT, 全分辨率, sample_count=1
        rtWaterRipple      = R16G16_FLOAT, 1/8 分辨率
        rtAO               = R8_UNORM, 全分辨率
:164-167 aliasing_allocator.Add(两组) + Commit + SetName("renderpath3D.aliasingAllocation")
:171-217 用 aliasingAllocation + offset 建 rtPostprocess / rtPrimitiveID(-_render) / rtSceneCopy_tmp / rtParticleDistortion(-_render)
        :189 rtPrimitiveID_render = rtPrimitiveID
        :215 rtParticleDistortion_render = rtParticleDistortion      ← MSAA=1 时自引用同一 RT
:193-200 rtSceneCopy 单独 CreateTextureZeroed（不别名，因为要跑 mipgen）+ rtSceneCopy_tmp 别名 + CreateMipgenSubresources
:219-245 rtMain(format_rendertarget_main, RT|SRV|UAV)；MSAA 时 rtMain_render
        :239 驱动降级 → 回写 msaaSampleCount
        :243 否则 rtMain_render = rtMain                          ← 非 MSAA 时自引用
:246-262 rtGUIBlurredBackground[0] = R10G10B10A2_UNORM 1/4 分辨率；[1][2] = 再 1/4（即 1/16），UAV|SRV
:263-277 VRS Tier2 时 rtShadingRate = R8_UINT（tile 网格），layout=UNORDERED_ACCESS
:279-304 深度：
        depthBuffer_Main  sample_count = msaa, layout=DEPTHSTENCIL, format_depthbuffer_main, bind=DEPTH_STENCIL
        depthBuffer_Copy / _Copy1  R32_FLOAT, sample_count=1, mip_levels=5, SRV|UAV, layout=SHADER_RESOURCE_COMPUTE
                                   + CreateMipgenSubresources
:306-321 debugUAV = R8G8B8A8_UNORM, SRV|UAV
:322-323 renderer::CreateTiledLightResources / CreateScreenSpaceShadowResources
:325-338 注释「These can trigger resource creations if needed」→ 逐个重放
        setAO / setSSREnabled / setSSGIEnabled / setRaytracedReflectionsEnabled / setRaytracedDiffuseEnabled /
        setFSREnabled / setFSR2Enabled / setEyeAdaptionEnabled / setReflectionsEnabled / setBloomEnabled /
        setVolumeLightsEnabled / setLightShaftsEnabled / setOutlineEnabled
:340  RenderPath2D::ResizeBuffers()              ← 最后再建 2D 的 rtFinal
```

**中间资源与新尺寸的对应关系（实测）**：

| 资源 | 分辨率 | 格式 | 依赖的新尺寸 |
| --- | --- | --- | --- |
| `rtMain` / `rtPostprocess` / `rtPrimitiveID` / `rtAO` / `rtParticleDistortion` | 全分辨率（`internalResolution`） | 见上 | `camera->width/height`（`:103-105` 写回） |
| `rtSceneCopy` / `rtSceneCopy_tmp` | 1/4 | `format_rendertarget_main` | 全分辨率 / 4，mip 数 `min(8, log2(max))` |
| `rtWaterRipple` | 1/8 | `R16G16_FLOAT` | 全分辨率 / 8 |
| `rtGUIBlurredBackground[0]` / `[1][2]` | 1/4 / 1/16 | `R10G10B10A2_UNORM` | 全分辨率 / 4 再 / 4 |
| `depthBuffer_Main` | 全分辨率，采样数 = msaa | `format_depthbuffer_main` | 采样数被驱动降级时回写（`:239`） |
| `depthBuffer_Copy/_Copy1` | 全分辨率，采样数 = 1，5 mip | `R32_FLOAT` | 供 mipgen + 前一帧深度（`:715-716` 交换） |
| `rtFinal` / `rtFinal_MSAA` | `GetInternalResolution()` = 物理尺寸 × `resolutionScale`（`wiRenderPath2D.h:125-132`） | `R8G8B8A8_UNORM` | 2D 层自己的缩放 |

`[实测 + 推断]` **注意**：`RenderPath3D::ResizeBuffers` 内**没有**显式 fence 或 `WaitForGPU`（对照交换链侧 `wiGraphicsDevice_DX12.cpp:3108` 有）。安全性来自：RT 是新建资源、旧的进入设备延迟释放队列，而帧首三缓冲 fence（`:5533-5546`）保证「上一帧的命令已完成的资源」才被真正释放。`[推断]` 确认需读设备的资源销毁路径（见 §12）。

### 7.3 resize 的「NW 尺寸 vs buffer 尺寸」结论（实测）

- **交换链**：由 `SetWindow` 无条件 `CreateSwapChain` → `ResizeBuffers` 分支处理（`wiGraphicsDevice_DX12.cpp:3233-3244`）。
- **路径 RT**：由 `RenderPath2D` 的 `GetInternalResolution() != current_buffersize` 惰性检测（`wiRenderPath2D.cpp:90-94`）。
- **相机尺寸**：由 `RenderPath3D::ResizeBuffers` 写回（`wiRenderPath3D.cpp:103-105`）。
- **因此 resize 的语义是「数据流被动跟随」**：没有任何 `OnResize` 广播、没有事件、没有观察者，全靠「每帧比一次尺寸」。

**对照 TooiyumeRender（实测，见 §11.1）**：`Display::Resize`（`Display.cpp:140-168`）只处理交换链与 RTV，**不通知任何 Renderer**；`BaseRenderer::OnResize` 声明了（`BaseRenderer.h:181`）、两边都实现了（`DeferredRenderer.cpp:139`、`ForwardRenderer.cpp:95`），但**全仓无任何调用点**（grep：Engine 内 3 处声明/定义 + Demos 0 处）。→ 这是原则⑤在本项目里最直接的缺口。

---

## 8. 【问题 7】渲染结果如何暴露给上层

### 8.1 三个出口（实测）

| 出口 | 声明 | 返回类型 | 有效时机 | 实际指向 |
| --- | --- | --- | --- | --- |
| `GetLastPostprocessRT()` | `wiRenderPath3D.h:175-180` | `const Texture*` | 后处理链结束后 | `&rtPostprocess` 初始（`h:176`）；`:2544 lastPostprocessRT = rt_read`；FSR 时 `:2558-2562` 改指 `&rtFSR[0]` |
| `GetRenderResult3D()` | `wiRenderPath3D.h:235` | `const Texture&` | 同上 | `return *GetLastPostprocessRT();`（就是它的引用化包装） |
| `GetGUIBlurredBackground()` | `wiRenderPath3D.h:241` | `const Texture*` | 后处理链末尾 | `return &rtGUIBlurredBackground[2];` |
| `GetRenderResult2D()` | `wiRenderPath2D.h:60` | `const Texture&` | `ResizeBuffers` 后 | `rtFinal` |

`[实测]` 成员是可变的：`mutable const Texture* lastPostprocessRT = &rtPostprocess;`（`h:175`），注释写明理由：

> "Post-processes are ping-ponged, this function helps to obtain the last postprocess render target that was written"（`wiRenderPath3D.h:176-177`）

### 8.2 消费点（实测）

- `Compose`：`wiRenderPath3D.cpp:1793-1798` `wi::image::Draw(&GetRenderResult3D(), fx, cmd)`（`fx.blendFlag=BLENDMODE_OPAQUE, quality=LINEAR, enableFullScreen`）→ 画到应用层 RT（非 HDR10 时就是交换链）。
- 2D 叠加：`:1813 RenderPath2D::Compose(cmd)` 再用 `BLENDMODE_PREMULTIPLIED` 把 `rtFinal` 叠上（`wiRenderPath2D.cpp:297-305`）。
- GUI 背景：`wiRenderPath2D.cpp:200-201` `image::SetBackground(*GetGUIBlurredBackground())`——**这是 GUI 模糊背景的唯一消费点**。
- 深度：`wiRenderPath2D.cpp:205-210` 用 `GetDepthStencil()` 决定是否做 stencil 提取。
- 路径内部自用：PathTracing 的 `Compose` **不用 getter**，直接读 `lastPostprocessRT` 成员（`wiRenderPath3D_PathTracing.cpp:722-726`）——因为它和基类在同一个 protected 作用域里。

### 8.3 HDR 中间结果与 LDR 最终呈现的分界（实测）

```cpp
// wiRenderPath3D.cpp:2334-2564 RenderPostprocessChain
:2341-2343  const Texture* rt_first = nullptr; rt_read = &rtMain; rt_write = &rtPostprocess;   // ping-pong 双 RT
:2346        Barrier(GPUBarrier::Aliasing(&rtPrimitiveID, &rtPostprocess))
── 阶段一：HDR 后处理 :2348-2445 ──
             FSR2 或 TemporalAA / 水下效果 / 自定义 BeforeTonemap / DepthOfField / MotionBlur
             每步：rt_first=nullptr; std::swap(rt_read, rt_write);
── 阶段二：Tonemapping（HDR→LDR）:2447-2494 ──
:2451-2460   ComputeLuminance（眼适应）
:2461-2471   ComputeBloom
:2473-2490   renderer::Postprocess_Tonemap(rt_first?:rt_read → *rt_write, cmd, ...)   ★ 分界线
:2492-2493   std::swap(rt_read, rt_write)
── 阶段三：LDR 后处理 :2496-2563 ──
             自定义 AfterTonemap / Sharpen / FXAA / ChromaticAberration / CRT
:2544        lastPostprocessRT = rt_read        ← 结果出口
:2546-2556   GUI 背景模糊（Downsample4x → Downsample4x → Blur_Gaussian）写入 rtGUIBlurredBackground[0/1/2]
:2558-2562   FSR 时 Postprocess_FSR，lastPostprocessRT = &rtFSR[0]
```

**分界点：`wiRenderPath3D.cpp:2473`**。`[实测]` 注意设计细节：注释明确写着 Bloom 与眼适应**不属于**后处理链，而在 tonemap 里消费（`:2446` 附近注释原文 "Bloom and eye adaption is not part of post process "chain", because they will be applied to the screen in tonemap"）——即 `ComputeBloom` 产生的是**供 tonemap 采样的辅助纹理**，不参与 ping-pong。`[实测]`

---

## 9. 【问题 8】多路径共存与切换；新增路径的最小清单

### 9.1 共存与切换的三种真实用法（实测）

| 用法 | 证据 | 形态 |
| --- | --- | --- |
| 单路径、按值成员 | `Example_ImGui.h:15-23`（`Example_ImGui` 继承 Application，含 `Example_ImGuiRenderer renderer;`）；`Samples/Tests/Tests.cpp:56 ActivatePath(&renderer)` | 最常见：路径是应用层子类的成员，生命周期跟随应用 |
| 两路径 CrossFade | `Editor/Editor.cpp:360 ActivatePath(&renderComponent, 0.5f, wi::Color::Black(), wi::FadeManager::FadeType::CrossFade)` | 编辑器里在「视口渲染路径」与别的路径间淡入淡出（Editor 的 `renderComponent` 声明方式**未确认**，需读 `Editor/Editor.cpp`） |
| 加载屏（第三种路径） | `wiLoadingScreen.cpp:39-45`：`addLoadingComponent(component, main, fadeSeconds, ...)` 内部 `main->ActivatePath(component, ...)` | 路径作为「应用层设施的载体」，是唯一持 `Application*` 的路径 |
| 无路径 | `Samples/Template_Windows/main.cpp`（100 行，无任何 RenderPath） | 路径是**可选**的：Application 在没有 activePath 时也能跑（`wiApplication.cpp:405-409` 都判空） |

`[实测]` 结论：**「同时只激活一条，但可切换」是 WE 的实际语义**；文档 `:293` 说的 "supporting multiple concurrent RenderPaths if required" 指的是「路径对象可以同时存在」，而非「同帧同时激活」。

### 9.2 PathTracing 路径的 override / 复用清单（逐条，实测）

`wiRenderPath3D_PathTracing.h:8` `class RenderPath3D_PathTracing : public RenderPath3D`。

**override（自己实现的）：**

| 行号 | 成员 | 为何必须覆写 |
| --- | --- | --- |
| `h:26` → `cpp:50-143` | `void ResizeBuffers() override` | 光追路径不需要基类那套栅格 RT（rtSceneCopy/rtAO/rtVelocity/...）。它 `:52` 先 `DeleteGPUResources()` 复用基类释放，再只建自用资源，最后 `:142` `RenderPath2D::ResizeBuffers(); // we don't need to use any buffers from RenderPath3D, so skip those` ← **注释即设计依据** |
| `h:32` → `cpp:145-300` | `void Update(float dt) override` | 累积采样（`sam++` `:181`）、只在第一采样更新加速结构/场景（`:188-189`）、末尾 `:191 RenderPath3D::Update(dt)` 复用基类 |
| `h:33` → `cpp:302-712` | `void Render() const override` | 用 `renderer::RayTraceScene(...)`（`:391-402`）替代栅格 DrawScene；并自写后处理链（`:583-704`） |
| `h:34` → `cpp:714-731` | `void Compose(CommandList) const override` | 只用 `rtMain→rtPostprocess` 的结果路径（`:722-726` 直接用 `lastPostprocessRT` 成员），然后 `:730 RenderPath2D::Compose(cmd)` |
| `h:30` → `cpp:45-48` | `~RenderPath3D_PathTracing()` | 等去噪 job 结束（`wi::jobsystem::Wait(denoiserContext)`） |
| `h:44` | `resetProgress()`（非虚） | 采样重置 |

**复用（不覆写）：**

| 复用项 | 证据 |
| --- | --- |
| 相机/可见性/帧常量资源（`frameCB`、`UpdateRenderData`、`BindCameraCB`、`UpdateRenderDataAsync`） | `cpp:322-337` 直接调基类用的那些 `renderer::` 函数 |
| 场景更新（`RenderPath3D::Update` 内 `scene->Update`） | `cpp:191` |
| 加速结构更新 | `cpp:330-337` `UpdateRaytracingAccelerationStructures` |
| `RenderVolumetrics` / `RenderLightShafts` 子阶段 | `cpp:486`、`:488` —— **直接调用基类的 virtual 子阶段** |
| 天空/空气透视/体积云后处理 | `cpp:469-484` |
| 调试绘制/精灵字体/`DrawSpritesAndFonts` | `cpp:492-580` |
| 光照/后处理资源结构体（`CreateLuminanceResources`/`CreateBloomResources`） | `cpp:134-135` |
| 参数与 setter（`setLightShaftsEnabled` 等） | `cpp:137-138` |
| `DeleteGPUResources`（基类的逐一置空） | `cpp:52` |
| `PreRender`（**不覆写**） | 声明表里没有 —— 因此沿用基类；但基类 `PreRender` 会为栅格管线创建 `rtSSR/rtVelocity/rtShadow/visibilityResources/...`（`wiRenderPath3D.cpp:450-726`），**对光追路径存在冗余分配**。源码中未见显式豁免 → `[推断]`，确认需核对该路径是否通过「开关全关」间接跳过（`wiRenderPath3D.cpp:450-473` 的「按开关释放」分支可能是缓解手段，但 `:587-617 rtVelocity`、`:650-674 visibilityResources` 的开条件是「一大串开关」，需要逐个核） |

`[实测 + 部分推断]` **这是本报告对「原则②」最重要的一条观察**：PathTracing 复用的不是「基类的编排」，而是**基类里那些恰好与管线无关的片段**（资源结构体、子阶段、场景更新）。它同时暴露了基类的根本问题——**`RenderPath3D` = 编排 + 资源容器 + 参数中心，三者没有分离**，所以「复用」只能靠「跳过 + 重写」。

### 9.3 新增一条前向路径的最小必需实现清单（`[推断]`，依据为本节全部实测行）

**必须实现（否则跑不出画面）：**

1. `class XxxPath : public RenderPath2D`（或继承 `RenderPath3D` 复用它）——若走 2D 起步，`wiRenderPath.h:8` 已给你画布与钩子。
2. `ResizeBuffers()`（`wiRenderPath2D.h:37`）：创建自己的 RT，`GetInternalResolution()` 取尺寸（`wiRenderPath2D.h:125-132`）；末尾**不要**调 `RenderPath3D::ResizeBuffers()`（PathTracing 的注释就是先例，`wiRenderPath3D_PathTracing.cpp:142`）。
3. `DeleteGPUResources()`（`wiRenderPath2D.h:37`）：置空 + 尺寸缓存清零（照 `wiRenderPath2D.cpp:11-22`）。
4. `Start()` / `Stop()`：`Stop() ⇒ DeleteGPUResources()`、`Start() ⇒ ResizeBuffers()`（照 `wiRenderPath3D.cpp:1816-1824`）——这是`ActivatePath` 的 fade 回调会自动调的两点，**接上资源生命周期是「路径可切换」的必要条件**。
5. `Render() const`：自己 `BeginCommandList()`、记录 pass、调用公共绘制能力（`renderer::DrawScene/DrawSky/...`）、复用 `RenderPath2D::Render()` 收尾（`wiRenderPath3D_PathTracing.cpp:709` 是先例）。
6. `Compose(cmd) const`：`image::Draw(自己的最终 RT)` + `RenderPath2D::Compose(cmd)`（照 `wiRenderPath3D_PathTracing.cpp:714-731`）。
7. `Update(dt)`（可选但实际必需，否则尺寸/DPI 不刷新）：至少调 `RenderPath2D::Update(dt)`（`wiRenderPath2D.cpp:88-127` 内已含 resize 检测与 DPI 处理）。

**必须复用（否则与项目基础设施脱节）：**

- 设备的全局取用（WE 是 `GetDevice()`，TooiyumeRender 需自己定契约，见 §11.3）。
- 场景/相机的取用处与可见性更新时机（WE：`RenderPath3D::Update` 里 `scene->Update`；TooiyumeRender：Demo 自己调 `CameraController->Update`）。
- 公共绘制能力与资源结构体（WE：`renderer::*`；TooiyumeRender：`BaseRenderer` 的 `BindRenderState/BindMaterial/GetPSO`）。

**可以忽略：**

- `Load()`（编辑器装载钩子，普通路径不用；WE 的 `RenderPath3D_PathTracing` 也没覆写）。
- `FixedUpdate()`、`PostUpdate()`、`PostRender()`、`PreUpdate()`（基类空实现；只有需要固定步长或时机特殊的路径才覆写）。
- `GetScriptBindingID()`（只有要暴露给 Lua 才需要）。
- `GetGUIBlurredBackground()/GetDepthStencil()`（不需要 2D 层做模糊/裁剪时返回 nullptr 即可）。

### 9.4 若要做「神经渲染（3DGS）路径」的额外判断（`[推断]`）

- WE 官方文档 `:293` 明说 「supporting multiple concurrent RenderPaths if required」；3DGS 与光追同属「不走栅格 DrawScene」的路径，PathTracing 已经示范了模板（`:142` 跳过基类资源、`:302-712` 自写 Render、`:583-704` 自写后处理）。
- **但 PathTracing 复用了大量基类成员（`wiRenderPath3D.h:101-169` 的 40 个 RT + 25 个 Resources 结构体）**，3DGS 若继承 `RenderPath3D` 会被动继承这些声明与 `PreRender` 冗余分配；若继承 `RenderPath2D` 则要自己写相机/场景更新前段。
- → `[推断]` 对「Neural 初期独立路径」（约束原则⑥）而言，**继承 `RenderPath2D` + 只依赖公共能力是更干净的选择**；这一条需要 TooiyumeRender 侧自己定义「公共能力的最小边界」（本报告不给实现方案）。

---

## 10. 【问题 9】这套组织的代价与前提

### 10.1 WE 依赖的项目级基础设施（实测）

| 基础设施 | 证据 | 渲染路径对它的依赖方式 | 缺失会怎样 |
| --- | --- | --- | --- |
| JobSystem | `wiJobSystem.h:17-75`（Initialize/ShutDown/Execute/Dispatch/Wait）；`wiRenderPath3D.cpp:845` `jobsystem::context ctx`、`:1783 Wait(ctx)` | 每帧一个 context，把 BindCameraCB/UpdateRenderData/各 compute 效果并行化 | `Render()` 的并行结构必须重写（WE 的 15 个命令列表里有多个是 job 并行录制的） |
| 全局 Scene/Camera 单例 | `wiScene.h:650-664`（Meyer's singleton，注释「You don't need to use it, but it's an option for simplicity」）；`wiRenderPath3D.h:197-204` | 路径**直接** `&GetScene()/&GetCamera()`，并在 `Update` 里 `scene->Update(...)`（`wiRenderPath3D.cpp:358-371`） | 路径不知道「场景从哪来」。要解耦必须改成显式传参 |
| 事件系统 | `wiEventHandler.h`；`wiApplication.cpp:285` `FireEvent(EVENT_THREAD_SAFE_POINT)`、`:859-864` 订阅 `EVENT_SET_VSYNC` | 应用层与设备之间（vsync 变更 → 重建交换链） | 交换链重建的路由会硬编码 |
| 参数系统 | `wiApplication.cpp:731-760` `wi::arguments::HasArgument(...)`（--debugdevice/--gpuvalidation/--igpu/...） | 应用层选设备/验证级别 | 设备选择需要自己写 |
| Lua 绑定 | `wiApplication.cpp:60-61` push_global；`wiApplication_BindLua.cpp:53-96` | `GetActivePath() + GetScriptBindingID()` 把路径暴露成脚本对象（`wiApplication_BindLua.cpp:137/144/151/158/163` 有 3D/Loading/2D/任意/清空 5 个入口） | 不影响 C++ 侧（可整块忽略） |
| 资源流送 | `wiApplication.cpp:403 resourcemanager::UpdateStreamingResources(dt)`；`wiRenderPath3D.cpp:1775-1779` 回读 | 应用层每帧驱动，路径层负责回读命令 | 大资产项目必需；教学项目非必需 |
| 显存别名分配器 | `wiRenderPath3D.cpp:108-217`（`CreateAliasingAllocator`+`offset_*`+`Add/Commit`） | 路径自己管理一组别名纹理 | 省显存，但使「资源生命周期」复杂度上升一个量级 |

### 10.2 逐条适用性判断（对 TooiyumeRender：约 1-3 万行、单 D3D12 后端、教学为主、现有 Deferred 管线）

| WE 设计 | 判断 | 理由（含替代方案与代价） |
| --- | --- | --- |
| 薄 `RenderPath` 基类（12 个虚拟钩子，全默认空实现，继承 Canvas） | **必需** | 这是原则①的最小实现。现状（§11.1）：`IGameApp` 只有 5 个纯虚钩子但应用层**自己录命令**（`DeferredApp.cpp:96-131` 每帧两个 `GraphicsContext::Begin/Finish`），渲染流程裸露在 Demo 里。替代方案「把渲染写进 Application 子类」正是约束文档点名要避免的。代价：多一层间接调用（可忽略） |
| `ActivatePath` + `activePath` 单活动路径 | **可选（建议先做接口、不做 fade）** | fade/CrossFade 需要额外一张 `crossFadeTexture` 与「保存上一帧后台缓冲」逻辑（`wiApplication.cpp:332-355`、`wiFadeManager.*` 139 行）——对教学项目收益低。可以先实现 `ActivatePath(path)` 瞬时切换（WE 的 `fadeSeconds=0` 分支 `wiFadeManager.cpp:18-24`），保留 fade 接口 |
| `Start() ⇒ ResizeBuffers()` / `Stop() ⇒ DeleteGPUResources()` 的绑定 | **必需** | 这是「路径可切换 + 资源生命周期可验证」（原则⑤）的结合点，且实现只有 4 行（`wiRenderPath3D.cpp:1816-1824`）。替代方案「外部手动调 OnResize/Shutdown」在本项目已经是失败案例（§11.1：`OnResize` 无人调用） |
| 12 个钩子中的 5 个（`Load/FixedUpdate/PostUpdate/PostRender/PreUpdate`） | **可选** | 教学项目用不到编辑器装载/FixedUpdate 双时间步。代价：留空实现即可，无成本 |
| `RenderPath2D` 的 2D 图层 + GUI + 视频解码 | **规模不匹配（除 GUI 外）** | WE 的 `RenderPath2D` 550 行里，图层冒泡排序（`cpp:484-543`）、视频解码队列（`cpp:170-189`）、DPI/stencil 缩放（`cpp:34-52`）对教学项目全部超纲。**但它证明了一件事**：2D 合成层可以作为「所有路径的共同出口」（`rtFinal` + `GetRenderResult2D`），这个形态可选保留 |
| `GetDepthStencil()/GetGUIBlurredBackground()` 默认 nullptr 的「最小结果契约」 | **必需（形态）** | 它是「路径向上暴露结果」的最小方案，替代方案「应用层知道路径内部 RT 名字」会让应用层与路径强耦合（当前 TooiyumeRender 的 `Display::SceneColorBuffer = DeferredRenderer.GetDeferredBuffer().SceneColorBuffer;[0]`（`DeferredApp.cpp:76`、`Tooiyume.cpp:74`）正是这种耦合）。代价：每次新增输出要加虚函数 |
| `RenderPath3D` 作为「RT 容器 + 约 60 开关 + 25 个 Resources 结构体」 | **规模不匹配** | 3326 行基类 + 40 个 RT + aliasing allocator，对 1-3 万行教学项目是无法维护的（改一个效果要理解整个 `ResizeBuffers` 的 246 行）。**且它直接导致 PathTracing 只能「跳过 + 重写」**（`wiRenderPath3D_PathTracing.cpp:142`）。替代方案：**把「公共渲染功能」独立成模块（`renderer::` 那层）而把 RT 所有权下沉到具体路径**——这正是原则②要的形态 |
| 15+ 命令列表 / 4 队列并行 | **规模不匹配** | 单 D3D12 后端的教学项目用 1-2 个命令列表即可。代价：WE 的多队列结构使 `Render()` 长达 955 行，且要求读者理解 `WaitCommandList` 的语义 |
| 显存别名（Aliasing allocator + offset） | **规模不匹配** | 教学项目显存压力不构成问题；引入后每一处 `offset_*` 都要与 `Barrier(Aliasing(...))` 配对，是原则⑤风险的最大来源之一（`wiRenderPath3D.cpp:111-217` vs `:1596-1599`、`:2346`） |
| 全局 `GetDevice()` + 全局 Scene/Camera | **可选（但需明确选择并记录）** | WE 用全局换来「路径零 Application 依赖」（§4.3）。TooiyumeRender 已经有一个全局设备概念（`GraphicsCore`），继续用成本最低；但**全局 Scene 是原则③的雷区**（场景应独立于路径，路径应显式接收 scene 引用）。代价：显式传参会让每帧调用多两个参数 |
| JobSystem 并行录制 | **可选** | 教学项目单线程足够；但若保留 job 接口，需注意 `wiRenderPath3D.cpp:1783` 那种「CPU job 同步」与 GPU 同步是两件事 |
| Lua 绑定 / 事件系统 / 参数系统 | **规模不匹配（可选）** | 对教学项目是纯负担。唯一值得借鉴的是「vsync 变更→重建交换链」的事件式解耦（`wiApplication.cpp:859-864`），可用一个回调替代 |
| `GetScriptBindingID` / `script_check_identifier` | **规模不匹配** | 纯 Lua 基础设施 |

### 10.3 这套组织的真正代价（三条，实测支撑）

1. **「复用」的代价是「跳过」**：PathTracing 为了复用基类的少量能力，必须继承一个携带 40 RT 的基类，然后用 `ResizeBuffers` 的注释（`wiRenderPath3D_PathTracing.cpp:142`）宣告「不要基类那些」。`[实测]`
2. **同步被藏进设备层**：路径层写不出「这里在等 GPU」——`WaitForGPU` 全仓只有 5 个调用者，Application 一个都没有。对原则⑤而言这**既是优点（不泄漏）也是风险（不可见）**。`[实测]`
3. **全局状态使「路径 = 可复用组件」不成立**：`RenderPath3D` 依赖全局 Scene/Camera（`wiRenderPath3D.h:197-204`）与全局 Device（`wiApplication.cpp:819`），因此路径无法脱离「整个引擎运行时」被测试/复用。`[实测 + 推断]`

---

## 11. 与 TooiyumeRender 现状的对照（本报告的适用性依据）

### 11.1 现状实测（工作树，HEAD 差异已核对）

| 事实 | 证据 | 与 WE 的对照 |
| --- | --- | --- |
| 应用层接口是 `GameCore::IGameApp`（`Startup/Cleanup/IsDone/Update/RenderScene/RenderUI/RequiresRaytracingSupport`） | `GameCore.h:12-27` | 对应 WE `Application` 的可覆写钩子，**但没有 `PreRender/Compose` 的分离**：`RenderScene()` 一个函数承担全部渲染 |
| 主循环：`RunApplication`（`GameCore.cpp:97-155`），每帧 `UpdateApplication`（`:58-77`）= `GameInput::Update` → `game.Update(dt)` → `game.RenderScene()` → `Display::Present()` | `GameCore.cpp:58-77`、`:136-150` | 对应 WE `Run()`，但**没有 fade 阶段、没有路径切换、没有 PreUpdate/PostUpdate 分层** |
| 渲染路径无抽象：Demo 直接持有具体 Renderer 全局对象并调用 `Update/Render/EndFrame` | `DeferredApp.cpp:55`（全局 `DeferredRenderer`）、`:107`、`:126-128`；`Tooiyume.cpp:53`、`:135-137` | **这是原则①的主要缺口**：具体管线类型出现在应用层代码中（`DeferredApp.cpp:11` include 具体头） |
| 已有抽象基类 `Renderer::BaseRenderer`（`Initialize/Shutdown/OnResize/BeginFrame/Update/Render/EndFrame/GetFeatures/GetPSO/BindRenderState/BindMaterial`） | `BaseRenderer.h:172-215` | **这就是「RenderPath 的雏形」，比 WE 更克制**（10 个虚函数、无 RT 容器、无参数中心）；但 `Update` 与 `Render` 的职责划分与 WE 的 `Update/Render/Compose` 不同（`Render(context, frame, pass, batchType)` 是「按 pass 画一批」而不是「编排一帧」） |
| 已有注册表 `RendererRegistry`（名字→creator）+ `RendererAutoRegister` | `RendererRegistry.h:29-59` | WE 没有等价物（WE 靠 `ActivatePath` 传指针 + Lua 名字）；**这是比 WE 更好的起点**，但目前**无任何使用点**（Demos 里 grep 到 0） |
| 交换链/窗口在 `Display`（`Display.h:16-25`、`Display.cpp:134-247`）；``SceneColorBuffer`` 是全局指针，由 Demo 赋值 | `Display.h:18`、`DeferredApp.cpp:76` | 与 WE 的「交换链在 Application」一致；但 WE 用「路径 Compose 到自己画到 backbuffer」，本项目用「全局 SceneColorBuffer 指到渲染器的 RT」 |
| Present 在 `Display::Present`：`PreparePresentSDR` 做一次 Copy → `Present(0,0)` | `Display.cpp:229-247`、`:251-263` | 对应 WE `SubmitCommandLists` 内的 Present；本项目每个 GraphicsContext 自己 `Finish()`（即提交），**没有「全帧单次提交」**（`DeferredApp.cpp:109`、`:130` 两次 Finish） |
| resize：`WM_SIZE` → `Display::Resize` 只做 `IdleGPU` + `ResizeBuffers` + 重建 RTV | `GameCore.cpp:162-164`、`Display.cpp:140-168` | **缺口**：不通知 Renderer；`BaseRenderer::OnResize` 声明（`BaseRenderer.h:181`）+ 两处实现（`DeferredRenderer.cpp:139`、`ForwardRenderer.cpp:95`）**全仓无调用者**（grep 实测） |
| HDR：场景颜色 RT 为 `R8G8B8A8_UNORM`（SDR 直接输出） | `RendererConfig.h:17`（`SceneColorBufferFormat`）、`Display.cpp:29`（`SwapChainFormat`） | **缺口**：没有 HDR 中间结果，因此也没有 HDR/LDR 分界（对照 WE `wiRenderPath3D.cpp:2473`） |
| 场景/相机：`Scene::Camera::Camera` 是 Demo 的成员，由 `CameraController` 更新；无全局 Scene 单例 | `DeferredApp.cpp:38-39`、`:105` | **比 WE 干净**：本项目没有全局 Scene 单例的包袱，路径显式接收 `RenderFrameDesc{Camera, Models, deltaT}`（`BaseRenderer.h:72-78`） |
| 提交/同步：`GraphicsContext::Finish()` 提交；`CommandManager::IdleGPU()` 在 resize/关机时调用 | `DeferredApp.cpp:109/130`；`Display.cpp:143/165`；`GameCore.cpp:81` | **比 WE 更粗**：没有帧 fence 的三缓冲等待（WE `wiGraphicsDevice_DX12.cpp:5531-5546`）；确认需读 `CommandListManager.h:98` 的 `IdleGPU` 实现与 `GraphicsContext::Finish`（本报告未读 → §12） |

`[实测]` 注：`GameCore.cpp`、`Display.cpp` 与 HEAD 的差异只有 asset-root 解析与 `Utility::Printf` 日志（`git diff --stat`：19 行 / 2 行新增），**本报告引用的行号与结论在 HEAD 上同样成立**（差异定位见 `git diff -U2`）。

### 11.2 体积对照（实测 wc -l）

| 层 | WE | TooiyumeRender |
| --- | --- | --- |
| 应用层 | `wiApplication.cpp` 938 行 | `GameCore.cpp` 179 + `Display.cpp` 267 + `GameInput.cpp` 628 = 1074 行 |
| 路径基类 | `wiRenderPath.h` 51 行（header-only） | `BaseRenderer.h` 218 + `BaseRenderer.cpp` 115 = 333 行 |
| 路径实现 | `wiRenderPath2D` 550 + `wiRenderPath3D` 3326 + PathTracing 733 = 4609 行 | `DeferredRenderer` 655 + `ForwardRenderer` 419 = 1074 行 |
| RHI | 未统计（`wiGraphicsDevice_DX12.cpp` 单文件即 5800+ 行） | `02_RHI` 10481 行（含 `d3dx12.h` 4052） |

→ `[推断]` **WE 的路径层是本项目的 4 倍以上**，且复杂度集中在一个基类里。任何「照搬 `RenderPath3D`」的迁移都会让项目体积翻倍而收益不明。

### 11.3 对 TooiyumeRender 的四条最小行动建议（`[推断]`，供 Lead 与后续工作包取舍）

1. **保留并强化 `BaseRenderer` 作为 RenderPath 基类**，补 `Start()/Stop()`（或复用 `Initialize/Shutdown`）与尺寸变更钩子，让「路径生命周期」显式化——参照 `wiRenderPath3D.cpp:1816-1824`。
2. **把 `Display::SceneColorBuffer` 这个全局赋值改成「路径通过接口交出结果」**（`GetRenderResult()` 形态，参照 `wiRenderPath3D.h:235`），避免应用层知道 `GetDeferredBuffer().SceneColorBuffer`（`DeferredApp.cpp:76`）。
3. **把「OnResize 无人调用」补上**：`WM_SIZE → Display::Resize → 当前路径 OnResize` —— 这是 WE 用「每帧尺寸比较」替代的那条链（`wiRenderPath2D.cpp:90-94`），而本项目已有 `OnResize` 接口可直接用。
4. **不要引入**：aliasing allocator、多队列命令列表、fade/CrossFade、Lua 绑定、全局 Scene 单例。

---

## 12. 未确认清单（每条写明确认需要读哪个文件）

1. `RenderPath3D_PathTracing` 的 `PreRender` 冗余分配到底如何被规避：需读 `wiRenderPath3D.cpp:375-830` 的每个资源创建条件的开关表达式，并核对该路径默认开关值（`wiRenderPath3D.h:32-93`）。
2. GPU 资源「延迟释放」与 `Stop()` 立即置空的安全性：需读 `wiGraphicsDevice_DX12.cpp` 的 `DestroyResource`/deferred deallocation/`FlushDeallocations` 实现。
3. `wi::LoadingScreen` 作为路径持有 `Application*` 的完整用法：需读 `wiLoadingScreen.h/.cpp` 全文（本报告只读了 `:37/:39/:45`）。
4. Editor 多路径共存的持有与切换细节（`Editor/Editor.cpp:360` 附近 `renderComponent` 的声明与生命周期）：需读 `Editor/Editor.cpp`。
5. `GraphicsContext::Finish` / `CommandManager::IdleGPU` 的同步语义（本项目侧）：需读 `Engine/02_RHI/Command/CommandContext.cpp` 与 `CommandListManager.h:98` 附近实现。
6. 本项目 `DeferredRenderer.cpp` / `ForwardRenderer.cpp` 的 `OnResize` 实现是否真的能重建 GBuffer（内容未读，仅确认了签名与行号）：需读 `DeferredRenderer.cpp:139` 与 `ForwardRenderer.cpp:95` 的函数体。
7. WE `Application::Update` 中 `resourcemanager::UpdateStreamingResources` 与路径的交互（截断读）；需读 `wiResourceManager.cpp` 相关函数。

---

## 附录 A：本报告证据索引（按 WE 文件）

| 文件 | 行号 | 事实 |
| --- | --- | --- |
| `WickedEngine/WickedEngine.h` | 137 | `using MainComponent = wi::Application;`（唯一命中） |
| `WickedEngine/wiApplication.h` | 24,27,34,45,53-60,63,67-68,81-94,97,101,107-141,143-151,153-154 | 成员与接口（见 §2.1、§3.1、§4.2） |
| `WickedEngine/wiApplication.cpp` | 46-62,64-87,89-390,392-412,414-422,424-438,440-716,723-866,868-886,888-896,910-937 | 生命周期、帧序、Compose、SetWindow（见 §2、§3、§7） |
| `WickedEngine/wiRenderPath.h` | 8,11,17-49 | 基类接口表（见 §4.1） |
| `WickedEngine/wiFadeManager.h` | 12-34,41-65,67-70 | 状态机与 Start/IsFaded（见 §2.3、§2.4） |
| `WickedEngine/wiFadeManager.cpp` | 12-61 | Update 的四个状态（见 §2.3） |
| `WickedEngine/wiCanvas.h` | 9-47,62-66 | Canvas 尺寸来源（见 §3.1、§7.1） |
| `WickedEngine/wiRenderPath2D.h/.cpp` | h:14,17-33,37-41,43-47,60,63,66,86-99,102-120,122-123,125-139 / cpp:11-22,24-82,83-86,88-127,147-156,157-291,292-310 | RenderPath2D 职责（见 §5） |
| `WickedEngine/wiRenderPath3D.h` | 12,15-31,32-93,95-96,98,101-169,171,173,175-180,184-192,194-195,197-204,206,215-232,235,238,241,243-368,370-382,384-395 | 接口与资源全集（见 §6、§8、§9） |
| `WickedEngine/wiRenderPath3D.cpp` | 17-94,96-341,343-347,349-373,375-830,832-1786,1788-1814,1816-1824,2334-2564,2566+ | 全部（见 §6、§7、§8） |
| `WickedEngine/wiRenderPath3D_PathTracing.h/.cpp` | h:8,11-24,26,30,32-34,36-42,44 / cpp:45-48,50-143,145-300,302-712,714-731 | override 清单（见 §9.2） |
| `WickedEngine/wiScene.h` | 650-664 | 全局 Scene/Camera（见 §4.4、§10.1） |
| `WickedEngine/wiJobSystem.h` | 17-75 | job API（见 §10.1） |
| `WickedEngine/wiLua.h/.cpp`、`wiLua.h:206` | 5,57,206,794 | 工具桥反向依赖（见 §4.3） |
| `WickedEngine/wiLoadingScreen.h/.cpp` | h:37 / cpp:39,45 | 路径→Application 的例外（见 §4.3、§9.1） |
| `WickedEngine/wiApplication_BindLua.cpp` | 14,44,53-96,137/144/151/158/163 | Lua 暴露面（见 §10.1） |
| `WickedEngine/wiGraphicsDevice_DX12.cpp` | 3097-3244,5388-5559 | 交换链创建/Resize、提交、Present、同步（见 §3.4、§6.3、§7.1） |
| `Content/Documentation/WickedEngine-Documentation.md` | 293,361 | 官方对 RenderPath 的定位（见 §2.1、§9.1） |

## 附录 B：TooiyumeRender 侧引用索引

| 文件 | 行号 | 事实 |
| --- | --- | --- |
| `Engine/01_Application/GameCore.h` | 12-27,32-40 | `IGameApp` 接口（见 §11.1） |
| `Engine/01_Application/GameCore.cpp` | 28-56,58-77,97-155,158-175 | 初始化与主循环（见 §11.1） |
| `Engine/01_Application/Display.h/.cpp` | h:16-25 / cpp:131-138,140-168,171-216,229-263 | 交换链/Resize/Present（见 §7.3、§11.1） |
| `Engine/04_Renderer/Renderer/Base/BaseRenderer.h` | 66-78,113-170,172-215 | Renderer 基类与 RenderFrameDesc（见 §11.1） |
| `Engine/04_Renderer/Renderer/Base/RendererRegistry.h` | 29-59 | 注册表（见 §11.1） |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.h/.cpp` | h:52-149 / cpp:139 | GBuffer 语义与 OnResize（见 §11.1） |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.h/.cpp` | h:46-102 / cpp:95 | ForwardBuffer 与 OnResize（见 §11.1） |
| `Engine/04_Renderer/Renderer/RendererConfig.h` | 8-19,25-33 | GBuffer/SceneColor 格式（见 §11.1） |
| `Demos/DeferredApp/DeferredApp.cpp` | 55,69-91,94-110,113-131,135-143 | 应用层直接用具体 Renderer（见 §11.1） |
| `Demos/ForwardApp/Tooiyume.cpp` | 53,64-93,96-123,126-143 | 同上（见 §11.1） |
| `docs/AI/ToiyumeRender 架构演进专项约束/00_总览与阅读指南.md` | 60,69,103-107,127-133 | 交付物索引与术语（见 §1.1、§1.3） |
