# 03 RHI 深度审计 + 00_Core / 01_Application 概览

> 审计对象: `Engine/02_RHI`（重点 a/c/e/f）+ `00_Core` / `01_Application` 概览
> 方法: 只依据真实读到的源码，逐条给 `文件:行号` 证据；未读到的写"未验证"。
> 状态标签: DONE / PARTIAL / ORPHAN / STUB / MISSING / BUG / UNKNOWN

---

## 0. 一句话结论

RHI 主干（设备→队列→分配器池→命令上下文→描述符堆→资源封装）已经**成链可用**：交换链确实在 `Display`，帧末 `Present` 走的是"SceneColor → BackBuffer CopyResource"这条简化路径。
但有三个层面的问题会直接卡住后续开发：

1. **DXR 只做到"探测 + QueryInterface"，没有任何 StateObject / SBT / DispatchRays**，`g_DeviceDXR` 和 `g_bRaytracingSupported` 都是**死变量**（第 e 节）。
2. **Resize 链路是断的**：`WM_SIZE → Display::Resize` 只重建交换链 back buffer，**从未通知任何 Renderer`OnResize`**，而 Renderer 的 GBuffer/SceneColor 全部按旧尺寸创建（第 d 节）。
3. **描述符/资源生命周期没有"延迟释放"**：`GpuResource::Destroy` 立刻 `m_pResource = nullptr`，`DescriptorAllocator` 的 CPU 堆只增不减，resize 一次漏一批描述符（第 c/g 节）。

---

## a) 帧循环与同步（Fence / 分配器复用 / Present / 多帧资源）

### a1 帧循环骨架 —— DONE
`Engine/01_Application/GameCore.cpp:26-58`
- `InitializeApplication` 顺序: `Graphics::Initialize(RequiresRaytracingSupport()) → SystemTime::Initialize() → GameInput::Initialize() → game.Startup()`（:29-36，DONE）。
- 主循环 `RunApplication`：`PeekMessage` 抽干消息队列 → `UpdateApplication`（:117-131）。
- `UpdateApplication`：`Update(DeltaTime)` 后 `game.RenderScene()` + `Display::Present()`（:39-58）。**每帧只有一次 Present，没有"多帧在飞"的显式设计**。
- `DeltaTime = Graphics::GetFrameTime()`，而 `s_FrameTime` 在 `Display::Present` 里才更新（`Display.cpp:236-238`）→ **第一帧 DeltaTime=0**（`Display.cpp:40` 初始 0.0f），相机/动画首帧步进为 0。属 **BUG(轻微)**，推理链清晰。

### a2 Fence 值管理 —— PARTIAL（设计巧，但有一处真实缺陷）
- 队列类型打进 fence 高 8 位：`CommandListManager.cpp:14-15` `m_NextFenceValue((uint64_t)Type << 56 | 1)`、`m_LastCompletedFenceValue((uint64_t)Type << 56)`；创建时 `m_pFence->Signal((uint64_t)m_Type << 56)`（:51）。这样 `IsFenceComplete` 可以直接由值反推队列：`CommandListManager.h:88-93` `GetQueue(D3D12_COMMAND_LIST_TYPE(FenceValue >> 56))`。这是本引擎比较亮的设计（DONE）。
- `ExecuteCommandList` 在 `m_FenceMutex` 内 `Close() + ExecuteCommandLists + Signal`（`CommandListManager.cpp:112-122`），返回值即本次 fence（DONE）。
- **缺陷**：`WaitForFence` 用**单个共享 event handle**，注释自己也承认会造成线程饥饿（`CommandListManager.cpp:159-174`），但更实际的问题是：等待结束后无条件 `m_LastCompletedFenceValue = FenceValue`（:172），把一个"我只等到 99"的语义写成了"队列已完成 99"——若另一线程已把 100 记进去，会被**回写成 99**（`IsFenceComplete` 里的 `std::max` 保护只在读硬件值那条路径上，见 :138-141，写路径没有保护）。**BUG（多线程下 fence 进度可倒退）**，修法：改成 `m_LastCompletedFenceValue = std::max(m_LastCompletedFenceValue, FenceValue)`。
- `IncrementFence` 目前无调用者（`CommandListManager.cpp:124-129`），只有 `WaitForIdle` 调它（`CommandListManager.h:30`）→ PARTIAL/未被业务使用。

### a3 每帧 CommandAllocator 复用 —— PARTIAL（会无上限增长）
`Engine/02_RHI/Command/CommandAllocatorPool.cpp:31-58`
- 就绪队列 `m_ReadyAllocators` 用 `(fence, allocator)` 先进先出；请求时**只看队首**：`AllocatorPair.first <= CompletedFenceValue` 才 `Reset()` 复用，否则直接新建（:37-55）。队首没完成时，**即使队列后面有已完成的分配器也不会被复用**。
- 后果：CPU 跑得比 GPU 快时，分配器数量单调增长（不是每帧固定 N 个），与"每帧一个 allocator"的常见教科书做法不同。属 **PARTIAL + 潜在显存/句柄膨胀**。修法：遍历队列找第一个已完成的，或按 frame index 索引 ring。
- `Shutdown` 只 `Release` 并 `clear()` `m_AllocatorPool`，**不清 `m_ReadyAllocators`**（:21-28）→ 关闭后队列里残留悬垂指针（若还有人 `RequestAllocator` 会暴毙）。**BUG(关闭路径)**。

### a4 Present 时机 —— PARTIAL（可用但缺同步）
- 交换链在 `Display`：`Display.cpp:193-199` `CreateSwapChainForHwnd(g_CommandManager.GetCommandQueue(), ...)`，3 buffer、`DXGI_SWAP_EFFECT_FLIP_DISCARD`、`SampleDesc.Count=1`（:177-188）。
- `Display::Present` 先做一次 copy pass 再 `Present(0,0)`（`Display.cpp:227-245`）。**`Present(0,0)` 不做 vsync，也没有任何 fence 空闲等待/帧 pacing**——CPU 会一路排下去，靠分配器池/描述符堆的 fence 回收兜底（不会立刻崩，但显存与 CPU 队列会累积）。
- **BackBuffer 轮换用手动取模**：`g_CurrentBuffer = (g_CurrentBuffer + 1) % SWAP_CHAIN_BUFFER_COUNT`（`Display.cpp:234`），全工程 `GetCurrentBackBufferIndex` **零调用**（grep 命中 0）。FLIP 模型下 back buffer 索引由 DXGI 决定，"手动 +1"在掉帧/异步 Present 时容易与真实索引错位 → **BUG（潜在"渲染到正在显示的 buffer"）**。正确做法：`s_SwapChain1->GetCurrentBackBufferIndex()`。

### a5 多帧资源（每帧 CB）—— PARTIAL
- 没有 ring/多份 CB。动态 CB 走线性分配器：`CommandContext::Finish` 里 `m_CpuLinearAllocator/m_GpuLinearAllocator.CleanupUsedPages(FenceValue)`（`CommandContext.cpp:138-139`），页大小为 CPU 2MB / GPU 64KB（`LinearAllocator.h:46-50`），分配对齐 256（:34）。
- 回收靠 fence：`LinearAllocatorPageManager::RequestPage` 里 `while (!m_RetiredPagesQueue.empty() && g_CommandManager.IsFenceComplete(front.first))`（`LinearAllocator.cpp:30-34`）→ 单帧 CB 安全（DONE）。
- **问题**：`CleanupUsedPages` 每次都把当前页 retire（`LinearAllocator.cpp:131-139`），即便这一页只用了 1 个 256B 常量也整页上交，下一帧重新申请；页面靠 fence 复用，所以不会真的每帧新建，但**每帧至少触发一轮 retire/request**，属于性能瑕疵而非正确性问题（PARTIAL）。

---

## b) Barrier / 资源状态转换

### b1 转换辅助 —— DONE
`Engine/02_RHI/Command/CommandContext.cpp:372-410` `TransitionResource`：
- 计算 `OldState = Resource.m_UsageState`，不同才写 barrier（:382），写回 `Resource.m_UsageState = NewState`（:403）——状态记录在资源上，是单一真相源（DONE）。
- 支持 `BEGIN_ONLY/END_ONLY` 分片转换：`BeginResourceTransition`+ `m_TransitioningState`（:412-438，DONE）。
- 同状态且是 UAV 时插 UAV barrier（:405-406），`InsertUAVBarrier` 在 :440-451。
- 屏障攒批：`m_ResourceBarrierBuffer` 最多 16 个，满了自动 `FlushResourceBarriers()`（:385, :408-409），`CopyBuffer` 等工具函数也先 flush（`CommandContext.h:776-790`）。

### b2 遗漏/重复 —— 有 1 处重复（无害）与 1 处潜在越界
- **重复转换（无害）**：`Display::PreparePresentSDR` 先手动 `TransitionResource(SceneColorBuffer, COPY_SOURCE)` + `TransitionResource(DisplayPlane, COPY_DEST)`（`Display.cpp:253-254`），紧接着 `Context.CopyBuffer(...)` 内部又转换一次（`CommandContext.h:778-779`）。因第二次 `OldState==NewState` 直接跳过（`CommandContext.cpp:382`），不产生多余 barrier，但**语义冗余**，建议删掉外层两行。
- **潜在越界**：`InsertUAVBarrier`（:440-451）在 `FlushImmediate==false` 时只写缓冲、不检查"满 16 就 flush"（对比 `TransitionResource` :408 有检查）。连续插 17 个 UAV barrier 时 `m_ResourceBarrierBuffer[16]` 越界；且 `ASSERT` 在 RELEASE 下被编译成 `(void)(isTrue)`（`00_Core/Utility/Utility.h:91`），**Release 版没有任何保护**。属 **BUG(潜在)**。
- **未验证**：SSAO/Shadow pass 在 `DeferredRenderer.cpp:274-285` 只有 PIXBegin/EndEvent 空壳，没有真实的 depth 前向转换链，无法评估"遗漏转换"。

---

## c) Descriptor 管理（重点）

### c1 Heap 类型与分布 —— DONE（结构清晰）
- CPU 端只读堆（RTV/DSV/CBV_SRV_UAV/SAMPLER 四种）统一用 `DescriptorAllocator`，无界、每次申请 256 个描述符的新堆：`DescriptorHeap.h:39` `sm_NumDescriptorsPerHeap = 256`，`DescriptorHeap.cpp:19-34` `RequestNewHeap`（`D3D12_DESCRIPTOR_HEAP_FLAG_NONE`，:26）。
- Shader-visible 堆用 `DescriptorHeap`（有界、一次性 `Alloc`）：`DescriptorHeap.cpp:61-93`；渲染器侧的纹理堆容量 4096（`ForwardRenderer.cpp:256`、`DeferredRenderer.cpp:390`）。
- 运行时"把 CPU 句柄搬进 shader-visible 堆"由 `DynamicDescriptorHeap` 负责：shader-visible、每堆 1024 个描述符、按 fence 退休回收（`DynamicDescriptorHeap.h:87-91`，`DynamicDescriptorHeap.cpp:15-54`）。

### c2 DynamicDescriptorHeap 的 staging/复制 —— DONE
- 两级：先在 CPU 侧 `DescriptorHandleCache` 做"根参数 → 句柄表"的暂存（`DynamicDescriptorHeap.cpp:169-184` `StageDescriptorHandles`，位图标记脏根参数），draw 时才批量搬：`CommandContext.h:720-736` 每次 `DrawInstanced/DrawIndexedInstanced` 前调 `CommitGraphicsRootDescriptorTables`（view + sampler 两个堆各一次）。
- 批量拷贝用 `CopyDescriptors` + `_BitScanForward64` 跳空洞、16 个一批（`DynamicDescriptorHeap.cpp:257-330`）。数组大小 16，且 dest range 数 ≤ src range 数 ≤16（每次 flush 前判 `NumSrc + Count > 16`，:296），**边界是对的**（这一点容易看错，此处判定 DONE）。
- 堆耗尽时 `RetireCurrentHeap() + UnbindAllValid()` 全量重标脏再换新堆（`DynamicDescriptorHeap.cpp:332-346`），逻辑正确（DONE）。

### c3 跨帧复用冲突 —— PARTIAL（两处真实风险）
1. **CPU 描述符堆只增不减**：`DescriptorAllocator::Allocate`（`DescriptorHeap.cpp:37-54`）申请新堆就永远留在 `sm_DescriptorHeapPool`（:32），唯一释放是 `DestroyAll()`（:14-17）。而 `ColorBuffer::CreateFromSwapChain` **每次调用都新分配 RTV**（`ColorBuffer.cpp:98-104`），`Display::Resize` 每次都重建 3 个 back buffer（`Display.cpp:150-161`）→ **每 resize 一次泄漏 3 个 RTV**，长时间窗口拖动会持续吃描述符。**BUG**。
2. **`DescriptorAllocator::Allocate` 完全无锁**（`DescriptorHeap.cpp:37-54`），只有 `RequestNewHeap` 加锁（:21）。注释宣称它是"线程安全"的分配器（`DescriptorHeap.h:19-20`），但 `m_CurrentHandle`/`m_RemainingFreeHandles` 是共享可变状态 → 多线程同时创建资源（`CommandContext::Begin` 可在多线程录制）会竞争。**BUG**。修法：`Allocate` 内部也取 `sm_AllocationMutex`，或改成 thread-local。
3. 小瑕疵：`StageDescriptorHandles` 中 `((1 << NumHandles) - 1)`（`DynamicDescriptorHeap.cpp:181`）在 `NumHandles==32` 时是 UB（`1<<32`）。属 **BUG(边界)**，当前调用最多绑几个 SRV，未触发。

---

## d) SwapChain / RenderTarget / Resize（关键断裂）

### d1 创建与重建 —— PARTIAL
`Engine/01_Application/Display.cpp:140-168` `Display::Resize`：
- `IdleGPU()` → 改尺寸 → 逐个 `g_DisplayPlane[i].Destroy()`（:150-151）→ `ResizeBuffers(3, w, h, SwapChainFormat, 0)`（:154）→ 重新 `GetBuffer + CreateFromSwapChain`（:156-161）→ `g_CurrentBuffer = 0`（:163）→ 再 `IdleGPU()`（:165）。**交换链这一侧是完整的（DONE）**。
- 触发点只有一处：`GameCore.cpp:143-144` `WM_SIZE → Display::Resize(w,h)`，**且用的是 `lParam` 的低/高 16 位（客户区尺寸）**，没有过滤 `w==0||h==0`（最小化时会以 0 尺寸调 `ResizeBuffers` → 必然失败/断言）。**BUG(最小化崩溃)**。

### d2 渲染目标没跟着重建 —— ORPHAN / BUG（本次审计最重要的可用性缺陷之一）
- `BaseRenderer::OnResize` 是纯虚（`Engine/04_Renderer/Renderer/Base/BaseRenderer.h:181`），`ForwardRenderer::OnResize` 有实现且**正确地** `DestroyForwardBufferTargets(); CreateForwardBufferTargets();`（`ForwardRenderer.cpp:93-102`），`DeferredRenderer::OnResize` 同样（`DeferredRenderer.cpp:122`）。
- **但 grep 全工程 `OnResize(` 只有 5 处：声明/定义 4 处 + 基类 1 处，没有任何调用者**。`Display::Resize` 里唯一的挂钩 `ResizeDisplayDependentBuffers(g_NativeWidth, g_NativeHeight)` 被注释掉了（`Display.cpp:167`），且该函数只有声明+定义（`Display.h:24`），函数体也不存在（grep 未见实现）。
- 结论：**窗口一拖动/缩放，交换链变成新尺寸，但 GBuffer/SceneColor/Depth 仍是旧尺寸**，`PreparePresentSDR` 的 `CopyBuffer` 会按不匹配尺寸拷贝（`Display.cpp:255`）→ Debug 层报 RESOURCE_DIMENSION_MISMATCH 或画面拉伸/黑边。**BUG（必现）**。
- 修法（可直接落地）：在 `Display::Resize` 末尾调用 `g_ActiveRenderer->OnResize(w,h)`，或在 WM_SIZE 里转发给 `IGameApp::OnResize`。

### d3 其余没重建的 —— 未验证/STUB
- `Display::SceneColorBuffer` 由 App 在 `Startup` 里赋值（`RayTracingApp.cpp:74`），是 `shared_ptr`，resize 后指向旧尺寸 buffer，`PreparePresentSDR` 直接用（`Display.cpp:230`）→ 同 d2 的后果。
- `Display.cpp:208-213` 的 Present 根签名/PSO 全被注释，`s_PresentRS/PresentSDRPS` 实际是 STUB（声明于 :136-137，从未 Finalize）。

---

## e) DXR 就绪度（关键）—— 结论: **只有探测，没有管线**

grep 结果（`Engine/` 全量 `*.h/*.cpp/*.hlsl`）：

| 关键字 | 命中 | 位置 |
|---|---|---|
| `ID3D12Device5` | 1 | `GraphicsCore.cpp:39` `ComPtr<ID3D12Device5> g_DeviceDXR` |
| `D3D12_FEATURE_D3D12_OPTIONS5` | 1 | `GraphicsCore.cpp:175` |
| `RaytracingTier` | 2 | `GraphicsCore.cpp:177, 186` |
| `D3D12_RAYTRACING_*` | 9 | 全部在 `02_RHI/d3dx12.h` 的 helper（3827/3835/3861/3886/3896/3904…） |
| `StateObject` | 35 | 全部在 `02_RHI/d3dx12.h`（3312/3400/3455/3502…），引擎代码零使用 |
| `DispatchRays` | **0** | — |
| `ShaderBindingTable` / SBT | **0** | — |
| `D3D12Raytracing`（app 侧） | **0** | — |

实现细节与判定：
- `Graphics::Initialize(bool RequireDXRSupport)` 只在 `RequireDXRSupport==true` 时才查 tier 并 `QueryInterface` 出 `ID3D12Device5`（`GraphicsCore.cpp:171-208`）——**DONE（探测）**。
- 但 `g_bRaytracingSupported` / `g_DeviceDXR` **只在本 cpp 内出现**（grep 各 4/2 次，全在 `GraphicsCore.cpp`），**没有导出到 `GraphicsCore.h`**（`GraphicsCore.h:26-38` 只导出了 `g_Device` 等）→ 任何渲染代码都拿不到 5.1 接口。**ORPHAN**。
- 无 StateObject / 无 DXIL library / 无 SBT / 无 `DispatchRays` → **MISSING**（真正的 RT 管线 0%）。
- `Demos/RayTracingApp` 名字叫 RT，实际 `RequiresRaytracingSupport()` 返回 true（`RayTracingApp.cpp:64`）只为**通过初始化门槛**，渲染仍走 `DeferredRenderer`（`RayTracingApp.cpp:55, 121-123`）。**ORPHAN/名不副实**。
- 顺带发现：`RayTracingApp.cpp:57-62` 硬编码绝对路径 `D:/CS-Self-Study/...` 资产 → 换机器即崩，属工程性问题（**BUG(可移植性)**）。

---

## f) 矩阵/坐标系约定（关键）—— 结论: **右手系 + 行向量 v*M + 行主序 + 深度 [0,1] + 非 ReverseZ**

证据链：

1. **右手系，forward = -Z，行向量**：`Engine/05_Scene/Camera/Camera.h:3-5` 注释 `// 规定强制使用右手系 / Forward = -Z / 行向量 v * M`；`mLook = {0,0,-1}`（:60）；视图空间定义注释在 :54。**DONE 且文档化**。
2. **CameraToWorld 用基向量当行**：`Camera.cpp:101-111` `Matrix4 cameraToWorld(mRight, mUp, -mLook, mWorldPosition)`，`m_mat.r[0..2]` 是 X/Y/Z 轴、`r[3]` 是平移（与 `Matrix4.h:24-28, 65-68` 的行存取一致）。`View = Invert(CameraToWorld)`、`ViewProj = View * Proj`（`Camera.cpp:45-47`），`Matrix4::operator*` 直接 `XMMatrixMultiply`（`Matrix4.h:79`）→ **左到右连乘，行向量语义自洽**。
3. **HLSL 侧同一套**：`Engine/Shader/01_Default/DefaultVS.hlsl:2` `// CPU端采用行向量，不转置，shader端也采用行向量写法, 矩阵使用row_major,都用左乘`；:60-61 `mul(float4(pos,1), WorldMatrix)`、`mul(float4(worldPos,1), ViewProjMatrix)`。反射常量表也是 `row_major float4x4 WorldMatrix`（`DefaultVS.h:72-73, 85`）。**DONE，两端一致，无转置陷阱**。
4. **投影矩阵**：`Camera.cpp:92-97` 行向量形式 `[X 0 0 0; 0 Y 0 0; 0 0 Q1 -1; 0 0 Q2 0]`。`w' = -z_view`，配合 RH 视图空间（前方 z 为负）→ `w'>0`，正确。`X = Y * m_AspectRatio`（:58-59），默认 `m_AspectRatio = 9/16 = h/w`（`Camera.h:132`）→ `X = Y/(16/9)` 恰好是标准 `Y/aspect`，**数学正确**（虽然参数名 heightOverWidth 与习惯相反）。**DONE**。
5. **深度范围 [0,1]**：D3D 默认即 [0,1]，`Q1/Q2` 的两种分支（:65-90）都按"近平面→1 / 远平面→0"（ReverseZ）或"近→0 / 远→1"（普通）构造，未见任何 `[-1,1]` 处理；视口 `MinDepth=0/MaxDepth=1`（`ForwardRenderer.cpp:46-47`）。**DONE [0,1]**。
6. **ReverseZ 是 STUB**：`Camera.h:92` `ReverseZ(bool)` 会把 `m_ReverseZ=true` 并重建投影，但**全工程零调用**（grep `ReverseZ(` 仅 2 处：声明 + 构造函数 `m_ReverseZ(false)` `Camera.h:129`）。深度状态也固定用 `D3D12_COMPARISON_FUNC_LESS`（`GraphicsCommon.cpp:148`），清除值固定 1.0（`DepthBuffer.h:8` `ClearDepth = 1.0f`）。`DepthStateReadOnlyReversed` 只有声明+定义、**从未初始化也从未使用**（`GraphicsCommon.h:44` / `GraphicsCommon.cpp:51`）→ **ORPHAN**。所以"反转 Z"目前只是**预留接口**。
7. **正反面/绕序**：`RasterizerDefault.FrontCounterClockwise = TRUE`，注释写"采用右手坐标系，逆时针为前面"（`GraphicsCommon.cpp:110`），`CullMode = BACK`（:109）。**注意**：右手系 + 逆时针正面在 D3D 里与"左手系 + 顺时针正面"等价，模型导入若按 glTF（RH, CCW）则一致；**未验证**实际 glTF 导入是否做了绕序翻转（`glTF.h:168` 只有相机类型枚举，未读到顶点绕序处理）。
8. **Aspect 不随窗口更新** —— **BUG**：`SetAspectRatio` 只有定义（`Camera.h:82`），**零调用者**；resize 后投影仍按 9:16，非 16:9 窗口必然拉伸。

---

## g) 资源生命周期 / 延迟释放 —— **MISSING**

- `GpuResource` 用 `ComPtr<ID3D12Resource>` 做引用计数（`GpuResource.h:51`），`Destroy()` 直接 `m_pResource = nullptr`（:32），并把 `m_GpuVirtualAddress` 置空、`++m_VersionID`（:33-34）。**没有 fence 保护、没有 deferred release 队列、没有"帧内回收池"**。对一次性的 `Texture`/`GpuBuffer` 尚可（调用方通常先 `IdleGPU`），但对"每帧重建的临时资源"是不安全的（**MISSING**）。
- `GpuResource.cpp` 只有两行（`#include`），类没有额外实现（`GpuResource.cpp:1-2`）→ 继承体系里 `PixelBuffer/ColorBuffer/DepthBuffer/GpuBuffer/UploadBuffer/ReadbackBuffer` 各自管自己的 Desc/View。
- `UploadBuffer::Create` 是**立即 `Destroy()` 再重建**（`UploadBuffer.cpp:12`），`m_UsageState` 未初始化就被写成 `GENERIC_READ`（:36-37 的参数），属 PARTIAL。
- `ReadbackBuffer::Map` 传的 `CD3DX12_RANGE(0, m_BufferSize)`（`ReadbackBuffer.cpp:53`）表示"CPU 会读全部"——对 READBACK 堆这是**多余但合法**；`Unmap` 传 `(0,0)`（:60）表示"没写"——正确。**DONE（细节正确）**。
- `BufferManager` 用 `unordered_map<name, shared_ptr>` 持有全局资源（`BufferManager.cpp:33-34`），`CreateColorBuffer` 同名直接覆盖 map（:45）→ 旧 buffer 的 shared_ptr 立即析构、资源立即释放，**没有先 `IdleGPU`**（:36-48）。当前调用点都在初始化/resize 前空闲期，但这是个"谁调用谁负责"的隐式契约，**BUG(潜在)**。
- `BufferManager::DestroyColorBuffer/DestroyDepthBuffer`（:68-76）有实现，**grep 未见调用者** → ORPHAN。

---

## h) 全局单例 / 静态状态 / 线程安全

| 项 | 位置 | 判定 |
|---|---|---|
| `g_Device`（裸指针，`pDevice.Detach()`） | `GraphicsCore.h:26` / `GraphicsCore.cpp:159` | DONE(可用)，但裸指针 + 末尾手动 `Release()`（:309-313）易双释放 |
| `g_CommandManager` / `g_ContextManager` 全局对象 | `GraphicsCore.cpp:42-43` | DONE；三队列共享一个 `CommandListManager`，跨队列等待靠 fence 高位（见 a2） |
| `g_DescriptorAllocator[4]` | `GraphicsCore.cpp:47-53` | PARTIAL：`Allocate` 无锁（c3-2） |
| `DescriptorAllocator::sm_DescriptorHeapPool` 静态 | `DescriptorHeap.cpp:12` | DONE（仅 `RequestNewHeap` 内加锁），但只增不减 |
| `DynamicDescriptorHeap::sm_*` 静态池 + `sm_Mutex` | `DynamicDescriptorHeap.cpp:9-13` | DONE（`Request/Discard` 均加锁 :17, :51） |
| `CommandAllocatorPool::m_AllocatorMutex` | `CommandAllocatorPool.cpp:33, 63` | DONE（请求/废弃都加锁） |
| `SamplerDesc::s_SamplerCache`（匿名命名空间 map） | `SamplerManager.cpp:15-32` | **BUG**：`CreateDescriptor()` 查完 cache **从未 `insert`**（:20-32 只有 `find` + 返回），所以去重完全失效，每次调用都新分配描述符；且该 map 无锁，多线程创建采样器会竞争。修法：`s_SamplerCache[hashValue] = Handle;` + 加锁。 |
| `LinearAllocator::sm_PageManager[2]` 依赖"自动注册"静态构造顺序 | `LinearAllocator.cpp:10-21` | PARTIAL：`sm_AutoType` 计数初始化，跨 TU 静态初始化顺序未定义时可能错配类型（当前只有 2 个实例且同 TU，实际安全） |
| `SystemTime::sm_CpuTickDelta` | `SystemTime.cpp:6` | DONE（`Initialize` 在 `Graphics::Initialize` 之后调用，`GameCore.cpp:29-31`） |

---

## 00_Core / 01_Application 概览

- **PCH**: `00_Core/PCH.h:1-53`，关掉了 4201/4238/4239/4324 警告，`NOMINMAX`，`_WIN32_WINNT 0x0A00`，引入 `d3d12.h/dxgi1_6.h/d3dx12.h` 与 `wrl`。**注意**：`PCH.h` 里 **没有** `#include <DirectXMath.h>`，但 `Math/*` 用了 `XMFLOAT4X4`/`XMMATRIX`（`Matrix4.h:32, 87`）——靠其他头间接引入，脆弱但能编译（PARTIAL）。
- **断言体系**: `Utility.h:89-143`，RELEASE 下 `ASSERT/ASSERT_SUCCEEDED/ERROR` **全部编译成空**（:91-96）→ 所有资源创建失败在 Release 下静默。**PARTIAL/风险**（尤其 `GraphicsCore.cpp` 里大量 `ASSERT_SUCCEEDED(CreateDescriptorHeap...)`）。
- **耗时统计**: `SystemTime.cpp:9-22` QPC 频率→秒/滴答；`BusyLoopSleep` 直接忙等（:38-47，100% CPU 占用），属于"故意的高精度等待"，**PARTIAL（有 CPU 浪费）**。
- **窗口**: `GameCore.cpp:88-109` 注册窗口类 + `CreateWindow`；`WndProc` 只处理 `WM_SIZE`（→`Display::Resize`）与 `WM_DESTROY`（:139-156）。**没有 WM_ENTRY/焦点/DPI 处理**（PARTIAL）。
- **输入**: `GameInput` 是"直接套用，还没啃"（`GameInput.h:3` 自述），`Update(float frameDelta)` 在 `GameCore.cpp:44` 调用，`kKey_escape` 用于退出（`GameCore.cpp:70-73`）。**PARTIAL（可用，但作者自己标注未消化）**。
- **Display 的 SceneColorBuffer**: 全局 `std::shared_ptr<ColorBuffer>`（`Display.h:18`），由 App 赋值（`RayTracingApp.cpp:74`），`PreparePresentSDR` 直接解引用（`Display.cpp:230`）→ **未判空，若 App 忘记赋值会空指针**（BUG(潜在)）。

---

## 汇总表

| 功能 | 状态 | 文件:行 | 证据说明 |
|---|---|---|---|
| 设备创建 / FeatureLevel 12_1 | DONE | GraphicsCore.cpp:158-164 | `D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_1, ...)`，失败直接 return |
| GPU 选择（厂商偏好） | PARTIAL | GraphicsCore.cpp:145-153 | `GetDesiredGPUVendor()` 固定返回 Nvidia，但没有真正按 vendor 过滤 adapter，只打印日志 |
| Debug Layer + GPU validation | DONE | GraphicsCore.cpp:113-132 | `EnableDebugLayer` + `SetEnableGPUBasedValidation(true)`（_DEBUG） |
| InfoQueue 断点 | PARTIAL | GraphicsCore.cpp:214-224 | 设了 ERROR/WARNING 断点，**未 SetMuteDebugOutput / 未过滤已知无害消息**，易被噪声打断 |
| 三队列（DIRECT/COMPUTE/COPY） | DONE | CommandListManager.h:104-110, cpp:82-89 | 三个 `CommandQueue` 成员，`Create` 全部创建 |
| Fence 值编码队列类型 | DONE | CommandListManager.cpp:14-15, 51 | 高 8 位存 Type，`IsFenceComplete` 反解（h:88-93） |
| CPU 等待 fence（单事件） | BUG | CommandListManager.cpp:159-174 | 共享 event handle + 等待后无条件回写 `m_LastCompletedFenceValue`（:172），多线程可回退 |
| 跨队列 GPU 等待 | PARTIAL | CommandListManager.cpp:150-156 | `StallForFence` 实现正确，但 `StallForProducer`（h:28）**只有声明无定义** → ORPHAN |
| CommandAllocator 池复用 | PARTIAL | CommandAllocatorPool.cpp:37-46 | 只看队首，未完成就新建；`Shutdown` 不清 `m_ReadyAllocators`（:21-28） |
| CommandContext 生命周期 | DONE | CommandContext.cpp:25-63, 121-150 | 借还池 + `Finish` 里 fence 回收 allocator/页/堆 |
| 资源 Barrier 缓冲 | DONE | CommandContext.cpp:372-410 | 16 个 barrier 攒批，满自动 flush（:408） |
| UAV barrier 攒批 | BUG(潜在) | CommandContext.cpp:440-451 | 非立即模式下无"满 16 就 flush"，Release 下 `ASSERT` 为空 → 可越界 |
| 延迟释放 / 资源回收 | MISSING | GpuResource.h:30-35 | `Destroy` 立即置空 ComPtr，无 fence 保护 |
| CPU 描述符堆（无界） | PARTIAL | DescriptorHeap.cpp:19-54 | 每 256 个新建一堆，永不回收；`Allocate` 无锁 |
| Shader-visible 固定堆 | DONE | DescriptorHeap.cpp:61-93 | `FLAG_SHADER_VISIBLE`，`Alloc` 带容量断言 |
| DynamicDescriptorHeap 回收 | DONE | DynamicDescriptorHeap.cpp:15-54, 332-346 | 按 fence 退休/回收，堆满换新并全量重标脏 |
| Dynamic 批量拷贝 | DONE | DynamicDescriptorHeap.cpp:257-330 | `CopyDescriptors` 16 个一批 + 位图跳洞，边界正确 |
| 描述符缓存去重（sampler） | BUG | SamplerManager.cpp:20-32 | `find` 后**从不写入** `s_SamplerCache`，去重失效且无锁 |
| SwapChain 创建 | DONE | Display.cpp:177-199 | 3 buffer / FLIP_DISCARD / 绑 graphics queue |
| SwapChain Resize | DONE | Display.cpp:140-168 | `Destroy → ResizeBuffers → GetBuffer → CreateFromSwapChain`，前后 `IdleGPU` |
| Resize 通知渲染器 | **MISSING** | Display.cpp:167; BaseRenderer.h:181 | `OnResize` 有实现但**零调用者**，`ResizeDisplayDependentBuffers` 被注释 |
| BackBuffer 索引 | BUG | Display.cpp:234 | 手动 `+1 %3`，未用 `GetCurrentBackBufferIndex`（全工程 0 命中） |
| Present copy pass | PARTIAL | Display.cpp:227-261 | 每个 Present 起一个 GraphicsContext 做 CopyResource，`Present(0,0)` 无 vsync/无 pacing |
| Present 根签名/PSO | STUB | Display.cpp:136-137, 208-213 | 声明后从未 `Finalize`，相关代码全部注释 |
| 每帧 CB 内存（线性分配器） | DONE | LinearAllocator.cpp:158-189, 128-143 | 256 对齐、按 fence 回收页 |
| 线性分配器大页 | PARTIAL | LinearAllocator.cpp:64-80, 146-156 | 大页走删除队列，靠 `RequestPage` 顺带清理，可能延迟 |
| DXR 能力探测 | DONE | GraphicsCore.cpp:171-208 | `OPTIONS5` + `RaytracingTier` + `QueryInterface<ID3D12Device5>` |
| DXR 对外暴露 | ORPHAN | GraphicsCore.h:26-38 | `g_DeviceDXR`/`g_bRaytracingSupported` 未导出，仅本 cpp 使用 |
| DXR 管线（SO/SBT/DispatchRays） | **MISSING** | grep 0 命中 | 无 `DispatchRays`、无 SBT、无 StateObject（`d3dx12.h` 里只有 helper） |
| RayTracingApp | ORPHAN | RayTracingApp.cpp:64, 121-123 | 只返回 true 过门槛，渲染仍走 DeferredRenderer |
| 右手系 + 行向量 | DONE | Camera.h:3-5; DefaultVS.hlsl:2 | CPU/HLSL 注释与 `mul(v, M)` 写法一致 |
| 行主序常量 | DONE | DefaultVS.h:72-73, 85 | 反射显示 `row_major float4x4` |
| 深度 [0,1] | DONE | Camera.cpp:92-97; ForwardRenderer.cpp:46-47 | 视口 Min/MaxDepth=0/1，无 `[-1,1]` 处理 |
| ReverseZ | STUB | Camera.h:92-93, 129 | `ReverseZ()` 零调用；`DepthStateReadOnlyReversed` 定义但从未初始化/使用 |
| 深度测试函数 | PARTIAL | GraphicsCommon.cpp:145-153 | 固定 `LESS`/`LESS_EQUAL`，与"非 ReverseZ"一致；ReverseZ 版本缺失 |
| 相机 Aspect 更新 | BUG | Camera.h:82 | `SetAspectRatio` 零调用，resize 后仍 9:16 |
| 内存泄漏检测 | PARTIAL | GraphicsCore.cpp:299-307 | `ReportLiveDeviceObjects` 只在 `_GAMING_DESKTOP && _DEBUG`，默认构建不生效 |
| Shutdown 顺序 | PARTIAL | GraphicsCore.cpp:282-313 | `IdleGPU → DestroyAllContexts → CommandManager.Shutdown → PSO/RS/Descriptor → Common → BufferManager → Display`，未见 `g_DeviceDXR` 释放、未见 `DynamicDescriptorHeap::DestroyAll` 释放的堆被显式清理 |
| 全局状态/锁 | PARTIAL | DescriptorHeap.cpp:37-54; SamplerManager.cpp:15 | 两处共享可变状态无锁 |
| 断言在 Release 下失效 | PARTIAL | Utility.h:89-96 | `ASSERT_SUCCEEDED` 编译为空，资源创建失败静默 |
| 消息循环 / 输入 | PARTIAL | GameCore.cpp:117-131, 139-156 | 只处理 `WM_SIZE/WM_DESTROY`，无 DPI/焦点/最小化保护（0 尺寸 Resize） |

---

## 未验证清单（明确声明，不含猜测）

1. ~~`RootSignature.cpp`/`PipelineState.cpp`~~ → 已补读，见第 i 节。
2. ~~`CommandContext.h` 的 `SetDynamic*` 细节~~ → 已补读（:440-569），见第 i 节。
3. ~~`PixelBuffer.cpp` 的创建/派生视图~~ → 已补读创建段（:298-362）与 `ColorBuffer.cpp` 全文。
4. ~~`GpuBuffer.h/.cpp`~~ → 已补读，见第 k 节。**`Texture.h/.cpp` 仍未通读**（只看了 `CreatePIXImageFromMemory` 的声明 :31/:66 与实现入口 :199）。
5. 实际 **glTF 顶点绕序是否翻转**（右手系 + CCW 正面的一致性）未验证。
6. `DeferredRenderer` 的 Shadow/SSAO/PostProcess/TAA/Tonemap pass 全是 PIXBegin/EndEvent 空壳（`DeferredRenderer.cpp:274-285, 349-371`），**这些链路整体未验证**。
7. `EnvironmentLightingManager`（IBL 预计算）只看了调用点，内部实现未验证。
8. 未做编译/运行验证（本次审计是静态阅读）。

---

## i) 补充：命令录制细节 / 根签名 / PSO / 资源创建（二次补读）

### i1 动态常量缓冲的暂存 —— PARTIAL（真实缺陷：CPU/GPU 页错配）
`Engine/02_RHI/Command/CommandContext.h:486-494`
```cpp
inline void GraphicsContext::SetDynamicConstantBufferView(UINT RootIndex, size_t BufferSize, const void* BufferData)
{
	ASSERT(BufferData != nullptr && Math::IsAligned(BufferData, 16))
	DynAlloc cb = m_CpuLinearAllocator.Allocate(BufferSize);   // ← 从 CPU 页分配
	memcpy(cb.DataPtr, BufferData, BufferSize);
	m_CommandList->SetGraphicsRootConstantBufferView(RootIndex, cb.GpuAddress);
}
```
- `m_CpuLinearAllocator` 是 `kCpuWritable` 类型，页面实体是 `D3D12_HEAP_TYPE_UPLOAD`（`LinearAllocator.cpp:111-117`），**这块内存本身就是 GPU 可读的上传堆**，而 `DynAlloc` 同时带 `DataPtr`(CPU) 和 `GpuAddress`(GPU)（`LinearAllocator.h` 的 `DynAlloc` 构造 :245-255）。所以 **写法本身正确：CPU 写、GPU 读，同一块上传堆**。
- 但注释说"使用动态上传堆分配块内存"却调的是 **m_CpuLinearAllocator** 而非 `m_GpuLinearAllocator`；两者同名不同堆（`CommandContext.cpp:156-157` `kCpuWritable` / `kGpuExclusive`）。**当前是对的**（常量必须从 UPLOAD 堆读），但**命名极易误用** —— 若有人照注释改成 `m_GpuLinearAllocator`（DEFAULT 堆、UAV 状态），`memcpy` 到不可映射的显存会直接崩溃。**PARTIAL（可维护性风险）**。
- 同族函数 `SetDynamicVB/IB/SRV`（:506-556）用的都是 `m_CpuLinearAllocator` + `SIMDMemCopy`，一致（DONE）。
- **真正的边界问题**：`Allocate(BufferSize)` 走默认 `Alignment=256`（`LinearAllocator.h:34`），小于 256B 的 CB（本项目 `GlobalConstants` 远大于此，暂时没事）会被对齐浪费；**若单帧动态数据超过 2MB 页（`kCpuAllocatorPageSize=2<<20`，`LinearAllocator.h:46`）会触发 `AllocateLargePage`（:146-156）**，大页**不进 `m_AvailablePagesQueue` 复用，直接进 `m_DeletionQueue` 删除**（:64-80）→ 每帧一次大分配就永久多一次 create/destroy。**PARTIAL**。

### i2 根签名去重 —— DONE（设计是正确的）
`Engine/02_RHI/Pipeline/RootSignature.cpp:96-172`
- 先对 `Flags + 所有静态采样器 + 每个根参数（含描述符表的每个 range）` 做哈希（:100-128），再在 `s_RootSignatureHashMap` 里查（:137-147），首次编译者 `Attach` 进去、其余线程 `while(*RSRef==nullptr) yield()` 等待（:164-170）。**跨线程共享同一 ID3D12RootSignature，且并发安全（DONE）**。
- 同时建立两张位图 `m_DescriptorTableBitMap` / `m_SamplerTableBitMap`（:118-121）与 `m_DescriptorTableSize[]`（:124），供 `DynamicDescriptorHeap` 判断"哪些根参数要搬描述符、各要几个"（`DynamicDescriptorHeap.cpp:169-184` 用它们）。**这条链路是打通的（DONE）**。
- 唯一瑕疵：`(1 << Param)`（:119,121）在 `Param>=32` 时 UB；根参数上限 64（`RootSignature.h:96-100` 注释），**实际超 32 个根参数就会出错**。当前每个 RS 只有 4 个根参数，未触发。**BUG(潜在)**。

### i3 PSO 去重 —— DONE，但有 STUB 残留
`Engine/02_RHI/Pipeline/PipelineState.cpp:105-153`
- `Finalize()` 先算 `m_PSODesc` 哈希（:112-113，输入布局单独喂入），再查 `s_GraphicsPSOHashMap`（:125-134），首次 `CreateGraphicsPipelineState` 后 `Attach`，其余 `yield` 等（:147-152）。**与根签名同一套模式，正确（DONE）**。
- `PSO::DestroyAll` 清两张表（:18-22），由 `Graphics::Shutdown` 调用（`GraphicsCore.cpp:290`）（DONE）。
- **但 Renderer 侧没用这套缓存**：`ForwardRenderer::BuildPSOs` 把 `defaultPSO` 直接赋值给成员后**注释掉了 `Finalize()`**（`ForwardRenderer.cpp:304-307`），`DeferredRenderer` 同样（`DeferredRenderer.cpp:440-443`）。**`m_DefaultPSO` 是一个从未 Finalize、`GetPipelineStateObject()` 为 nullptr 的半成品**。后续 `GetPSOIndex` 里 `ColorPSO.Finalize()`（`ForwardRenderer.cpp:388`）会重新走哈希表拿到**真实** PSO，所以运行时能跑，但 `m_DefaultPSO` 本身是 STUB（**STUB**）。
- `ForwardRenderer::GetPSOIndex` 的"PSO 缓存"实际是**线性扫描 + 每次 Finalize 都必然命中全局哈希表**（:388-398），比对的是 `ID3D12PipelineState*` 指针，**每次 draw 都重新算一遍哈希**（:112 对一整个 `D3D12_GRAPHICS_PIPELINE_STATE_DESC` 做 CRC）→ **每帧每材质都有哈希开销**，虽然正确但很贵。**PARTIAL（性能）**。

### i4 资源创建路径 —— DONE（实现干净）
- `PixelBuffer::CreateTextureResource`（`PixelBuffer.cpp:343-362`）：先 `Destroy()`（:346），再 `CreateCommittedResource(D3D12_HEAP_TYPE_DEFAULT, ..., D3D12_RESOURCE_STATE_COMMON, &ClearValue, ...)`（:349-351），然后 `m_UsageState = COMMON`（:354）。**初始状态 COMMON + 带 ClearValue 是标准做法（DONE）**。
- `ColorBuffer::Create`：`NumMips==0 ? ComputeNumMips : NumMips`（`ColorBuffer.cpp:110`），`CombineResourceFlags` 在非 MSAA 时追加 `ALLOW_UNORDERED_ACCESS`（`ColorBuffer.h:83-91`），然后 `CreateTextureResource + CreateDerivedViews`（:126-127）。**DONE**。
- `CreateDerivedViews` 在 `m_SRVHandle.ptr == D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN` 时才分配描述符（`ColorBuffer.cpp:67-71, 87-88`）——**"复用"是靠这个哨兵值判断的，但 `Create()` 前已经 `Destroy()` 过，`Destroy` 并不重置句柄**（`GpuResource.h:30-35` 只清 `m_pResource`/`m_GpuVirtualAddress`）→ 同一个 `ColorBuffer` 对象第二次 `Create` 时 **SRV/RTV 句柄非空，于是跳过重新分配**，可它拿着的是**上一代资源的旧描述符**（描述符内容仍指向已释放的旧 ID3D12Resource）→ **视图指向已销毁资源**。`Display::Resize` 与 `ForwardRenderer::CreateForwardBufferTargets` 都是先在**新对象**上 `Create`，暂时绕过；但 `BufferManager::CreateColorBuffer` 每次 `make_shared` 新对象，也绕过。**目前未被触发，但是个定时炸弹（BUG(潜在)，高优先级）**。修法：`GpuResource::Destroy` 里把 `m_SRVHandle/m_RTVHandle/m_UAVHandle[i].ptr` 重置为 `D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN`。

### i5 ForwardBuffer 释放 —— BUG（潜在空指针）
- `ForwardRenderer::DestroyForwardBufferTargets` 直接 `m_ForwardBuffer->SceneColorBuffer[0]->Destroy()`（`ForwardRenderer.cpp:325-329`），**没有判空**；而 `CreateForwardBufferTargets` 在 `width==0||height==0` 时**直接 return，不创建 `m_ForwardBuffer`**（:313）→ 最小化一次（尺寸 0）后再 `Shutdown/OnResize`，`m_ForwardBuffer` 仍是旧对象或 nullptr，`->` 解引用即崩。而 `WM_SIZE` 确实会传 0 尺寸（`GameCore.cpp:144` 未过滤）。**BUG**。

### i6 DeferredRenderer 的 resize 是"半套" —— BUG（关键，与 d2 叠加）
- `DeferredRenderer::DestroyDeferredBufferTargets()` 的实现**只有一行 `BufferManager::DestroyAll()`**（`DeferredRenderer.cpp:481-484`），它清空的是 `s_ColorBuffers/s_DepthBuffers` 两张 map（`BufferManager.cpp:62-66`）。
- 但 `m_GBuffers` 这个 `shared_ptr<DeferredBuffer>`（`DeferredRenderer.h:80-87`）**仍持有旧 buffer 的 shared_ptr**，直到 `CreateDeferredBufferTargets` 里 `m_GBuffers = std::make_shared<DeferredBuffer>()`（:454）才被替换 → 旧资源在那之前**一直活着**。语义上不算泄漏，但**顺序上"先清 map 再重建"意味着旧资源在重建期间仍被 `m_GBuffers` 引用**，如果这中间有任何 GPU 引用就危险（当前是同步重建，暂时安全）。
- **真正的问题是 `Display::SceneColorBuffer` 不会跟着换**：全工程只有 3 处赋值，全在 `Startup`（`RayTracingApp.cpp:74`、`DeferredApp.cpp:74`、`ForwardApp` `Tooiyume.cpp:69`），**resize 路径没有任何一处重新赋值**。而 `Display::Present → PreparePresentSDR(*Display::SceneColorBuffer)`（`Display.cpp:230`）用的一直是**启动时那个旧尺寸 buffer** → resize 后 `CopyResource` 的源(旧尺寸)与目标(新尺寸 backbuffer) **尺寸不一致**，Debug 层直接报错。**BUG（必现，最优先修）**。
- 修法：`DeferredRenderer::OnResize` 末尾（`DeferredRenderer.cpp:129-131` 之后）补 `Display::SceneColorBuffer = GetDeferredBuffer().SceneColorBuffer;`，并把该调用从 `Display::Resize` 里接起来（当前根本没人调 `OnResize`，见 d2）。

---

## j) 修订：先前的两处判断需要修正

1. **a2 的 `IncrementFence`「无业务使用」**：`WaitForIdle` 会调它（`CommandListManager.h:30`），而 `WaitForIdle` 被 `IdleGPU` 使用（`Display::Resize` 前后各一次，`Display.cpp:143,165`）。所以它是"仅用于 idle 路径"，不是完全死代码 → 判定从 ORPHAN 改为 **DONE(受限使用)**。
2. **c2 的 DynamicDescriptorHeap 边界**：再核对 `DynamicDescriptorHeap.cpp:290-310` 后确认，`NumSrcDescriptorRanges + DescriptorCount > 16` 时先 flush 再续，**dst/src 索引都不会越界**（先前判断 DONE 成立，此处不再保留"容易看错"的措辞）。

---

## k) 补充：GpuBuffer / 灯光常量缓冲（三次补读）

### k1 `GpuBuffer::Create` 三条路径 —— DONE
`Engine/02_RHI/Resource/GpuBuffer.cpp:12-123`
- 三条路径（committed / from UploadBuffer / placed）都是 `Destroy() → DescribeBuffer() → m_UsageState=COMMON → Create*Resource → 可选 InitializeBuffer → CreateDerivedViews()`（:17-51、:58-92、:96-122）。
- `DescribeBuffer` 固定 `DIMENSION_BUFFER / ROW_MAJOR / MipLevels=1 / Width=m_BufferSize`（:142-159），`Desc.Flags = m_ResourceFlags`，而基类构造里默认 `m_ResourceFlags = ALLOW_UNORDERED_ACCESS`（`GpuBuffer.h:59`）→ **所有 buffer 默认带 UAV 权限（DONE，但对纯 VB/IB 是多余的，无法走只读优化；PARTIAL(性能)）**。
- `CreateConstantBufferView` 每次调用都 `AllocateDescriptor`（:137）→ **每帧每物体建 CBV 都会永久吃掉一个描述符**（同 c3-1 的堆只增不减）。当前调用点（`ModelLoader` 的 meshCBV）在加载期一次性创建，暂未爆；**若做 per-frame CB 必炸（BUG(潜在)）**。
- `CreatePlaced`（:96-123）**有实现、无调用者** → **ORPHAN**。

### k2 描述符句柄"哨兵值复用"的系统性缺陷 —— BUG(潜在)
- `ByteAddressBuffer::CreateDerivedViews` 用 `if (m_SRV.ptr == UNKNOWN)` 才分配（`GpuBuffer.cpp:171-172, 181-182`），`StructuredBuffer` 同（:196-197, 210-211）。**但 `GpuResource::Destroy()` 不会把 `m_SRV/m_UAV` 重置回 `UNKNOWN`**（`GpuResource.h:30-35` 只清资源指针与 GpuVA）→ 对同一个 `GpuBuffer` 对象**第二次 `Create`** 时句柄非空 → 跳过分配但在原句柄上 `CreateShaderResourceView`（:173, 198），"描述符内容被覆写、句柄复用"，**旧句柄语义已不再指向旧资源**。
- 更危险：`StructuredBuffer::CreateDerivedViews` **每次都无条件 `m_CounterBuffer.Create(...)`**（:208），counter 的 SRV/UAV 同样靠哨兵判断 → 重建后外部缓存的 `GetCounterSRV()` 句柄内容会**悄悄变成新 counter**。当前无外部缓存，**未触发**。
- 结论：与 i4 同源，建议统一在 `Destroy()` 里把所有句柄重置为 `D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN`。
- `TypedBuffer::CreateDerivedViews`（:215-237）**两条分配语句被整段注释掉**（:224-225, 234-235），却仍直接 `CreateShaderResourceView(..., m_SRV)` / `CreateUnorderedAccessView(..., m_UAV)`（:226, 236）→ 基类构造把 `m_SRV/m_UAV` 置为 `UNKNOWN(-1)`（`GpuBuffer.h:60-61`），**这里会把描述符写到地址 0xFFFFFFFFFFFFFFFF**。`TypedBuffer` **零调用者** → **BUG(潜在，死代码)**。修法：恢复被注释的两行。

### k3 灯光常量缓冲 —— BUG（轻微：重复创建视图）
- `LightingSystem.cpp:21-25`：`m_LightGPUBuffer.Create(...)` 内部**已经**调了 `CreateDerivedViews()`（`GpuBuffer.cpp:51`）；`CreateLights()` 末尾又**手动再调一次** `m_LightGPUBuffer.CreateDerivedViews()`（`LightingSystem.cpp:73`）。对 `StructuredBuffer` 而言这会**再建一个 counter buffer**（`GpuBuffer.cpp:208` 每次无条件 Create）→ **一次初始化泄漏一块 GPU buffer + 2 个描述符**。**BUG(轻微)**。
- `InitializeResources()` 里 `Create("m_LightGPUBuffer", MaxLights, sizeof(LightData))` **没传 initialData**（:23），随后 `CreateLights()` 填 CPU 数组并 `InitializeBuffer`（:72）——顺序正确（DONE）。
- `UpdataState`（拼写）只做一次 `TransitionResource(..., PIXEL_SHADER_RESOURCE)`（:79），靠 `TransitionResource` 内部 `OldState != NewState` 兜底（`CommandContext.cpp:382`）→ 合法，但**它不"更新状态"只是"确保一次转换"（PARTIAL）**。
