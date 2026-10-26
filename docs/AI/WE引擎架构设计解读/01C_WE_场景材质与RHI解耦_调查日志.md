# 01C_WE_场景材质与RHI解耦 调查日志

## 1. 任务与产出
- 任务：task-3 / 精读 WickedEngine，产出「场景/材质与底层 RHI 解耦」源码级证据报告。
- 产出 1：`docs/AI/ToiyumeRender 架构演进专项约束/01C_WE_场景材质与RHI解耦.md`（本报告）。
- 产出 2：本日志。
- 约束遵守：未修改任何运行时代码；未执行 git checkout/restore/stash；未运行构建。

## 2. 实际读了哪些文件（行数来自 `wc -l`，实测）
| 文件 | 行数 | 读取方式 | 覆盖度 |
| --- | --- | --- | --- |
| `wiScene.h` | 711 | 全读 | 100% |
| `wiScene_Components.h` | 2780 | 分段全读（Material/Mesh/Object/顶点格式区） | ~95%（未读：Lua/Character/Hair 等无关段） |
| `wiECS.h` | 919 | 全读 | 100% |
| `wiGraphics.h` | 2340 | 全读（含 deprecated 段略读） | ~90% |
| `wiGraphicsDevice.h` | 545 | 全读 | 100% |
| `wiGraphicsDevice_DX12.h` | 646 | 全读 | 100% |
| `wiGraphicsDevice_DX12.cpp` | 7858 | grep 定位 + 精确读 3887-3964（CreateShader/root signature）、4089-4207（CreatePipelineState）、5388-5462（SubmitCommandLists）；grep 行号：CreateBuffer2:3313 / CreateTexture:3544 / CreatePipelineState:4089 / CreateSubresource:4686,5038 / GetDescriptorIndex:5182,5220 / BeginCommandList:5284 / SubmitCommandLists:5388 / WaitForGPU:5813 | ~5% |
| `wiResourceManager.h` | 153 | 全读 | 100% |
| `wiResourceManager.cpp` | 1667 | grep 定位 + 精确读 244-275（类型表）、930-995（BC 压缩调度）、1036-1145（Load 缓存判定）；流送:1167,1256,1295,1444,1514 | ~12% |
| `wiTextureHelper.h` | 90 | 全读 | 100% |
| `wiGaussianSplatModel.h` | 68 | 全读 | 100% |
| `wiGaussianSplatModel.cpp` | 517 | 关键区间全读：1-30（静态 shader/PSO 声明）、150-250（CreateRenderData/Update/Initialize）、284-370（MakeReservations/UpdateGPU 写入）、490-520（Draw/Clear）+ grep 全符号表 | ~45% |
| `wiGPUBVH.h` | 28 | 全读 | 100% |
| `wiRenderer.h` | 1468 | grep 定位（visibleObjects:150,174；VISIBILITY_BLOCKSIZE:56-57） | ~5% |
| `wiRenderer.cpp` | 19686 | 分段读 330-420（变体/注册表）、791-892（LoadShader）、1925-2230（PSO 全变体预热）、3300-3460（取 PSO→DrawIndexedInstanced/实例批）、3590-3625（延迟 MIP/BC 执行）、19355-19375（AddDeferredMIPGen）；grep 行号：PSO_object:364-368 / LoadShaders:895 / DrawIndexed*:3128,3406,3418,3525,8596,9739,9806,11093 / visibleObjects:3727,3805,3991,5849,5853,7223 / DrawScene:7179 / renderQueue:338-339,3427,6813-6817,7168-7169,7222-7245 | ~8% |
| `wiScene.cpp` | 368039 B | grep 定位 + 精确读 1078-1130（CopyBufferAsync 上传）、4255-4300（BLAS 脏）、4328-4390（材质写入）、9201-9250（LoadModel2）；grep 行号：bindless 上传:124-195,930-932 / 写入点:4237,4352,4784,5243,5268,5358,5371 / 3DGS:5405-5419 / 更新系统:34,414,418,434,4106,4298,4558 | ~4% |
| `shaders/ShaderInterop_Renderer.h` | — | 精确读 393-523 | 该段 100% |
| `wiScene_Components.cpp` | 107126 B | 精确读 282-530（WriteShaderMaterial 等）+ 532-678 + 1141-1327 | ~3% |
| `Editor/ModelImporter.h` | 7 | 全读 | 100% |
| `Editor/Editor.cpp` / `Editor/ContentBrowserWindow.cpp` | 大 | 仅 grep（ImportModel_*:5665-5677 / 支持类型:1179 / wiscene 列表:317,378） | <1% |

## 3. 用过的命令（全部只读）
- `bash: wc -l` / `ls -l` 统计文件规模（得到 wiRenderer.cpp 19686 行等）。
- 工具 `read`（带 offset/limit）读取上述精确区间。
- 工具 `grep`（ripgrep 语法）定位符号：`CreatePipelineState|LoadShader|PipelineState`、`instanceArrayMapped|...`、`GaussianSplat|gaussian_splat`、`GraphicsDevice_DX12::<方法>` 等。
- 未使用 `sed/awk`（`read` 带行号更不易错），未运行任何构建。

## 4. 实测 vs 推断
**实测（命令输出中直接看到）**：
- 所有带 `file:line` 的结论，除报告中标 `【推断】/【待补】` 的外，均为实测。
- 关键实测：PSO 缓存结构（wiRenderer.cpp:364-368）；`ObjectRenderingVariant` 位域（:348-363）；`resources` 缓存为 `weak_ptr`（wiResourceManager.cpp:236）；`AddDeferredMIPGen` 在 wiRenderer.cpp:19360 而非 wiTextureHelper.h；3DGS 独立类型（wiGaussianSplatModel.h:15-67）。
**已由后续读取转为实测的原推断**：
- 「instanceCount 来自 GPU stream compaction」——更正：`instanceCount` 来自 CPU 侧 `renderQueue.batches` 的实例批累积（`wiRenderer.cpp:3427-3450`）；GPU stream compaction 只产出 `visibleObjects` 索引（`:3805` 写入、`:3991` resize）。
- `ObjectComponent/MeshComponent::CreateRenderData` 调用时机——已在 `wiScene.cpp:1815/1874/1954` 与 `wiScene_Components.cpp:532/559/1369/2131` 确认。
- `GaussianSplatScene::Draw` 是否复用渲染器 PSO 缓存——已确认**不复用**：它用本模块 `static PipelineState pipelineState`（`wiGaussianSplatModel.cpp:24, 236-243, 493-505`），只复用 `wi::renderer::LoadShader`。
**仍为推断/未确认（报告内均显式标注）**：
- `visibility` 压缩列表到 `visibleObjects` 的整体链路（仅读到写入点与消费点，未读 `UpdateVisibility`/stream compaction 的完整实现，`wiRenderer.cpp:3619-4100` 未精读）。
- `cpu_gpu_mapped_resource_index` 的翻转时机（未定位其赋值行）。
- `RenderQueue` 类型定义所在头文件（grep `wiRenderer.h` 未命中）。
- `wiGraphicsDevice_DX12.cpp:3313` `CreateBuffer2` 的 D3D12MA 分配路径未读。

## 5. 剩余未读清单（报告已交付，以下为可选深挖项）
1. `wiGraphicsDevice_DX12.cpp:121-5388` 之间的资源创建实现：`CreateBuffer2`(3313) 的 D3D12MA 分配/`CreateTexture`(3544)/`CreateSubresource`(4686,5038)/`GetDescriptorIndex`(5182,5220)/`BeginCommandList`(5284) 的落地细节（`DescriptorHeapGPU` 环形分配与 `fence` 回收策略只读到声明，未读实现）；
2. `wiGraphicsDevice_DX12.cpp:5460-5813` `WaitForGPU` 与 `AllocationHandler::Update` 的调用点（本次只读了头文件里的实现）；
3. `wiRenderer.cpp:3619-4100` `UpdateVisibility` / `stream compaction` 如何产出 `visibleObjects`（本次只读了写入点 3727/3805/3991）；
4. `wiRenderer.cpp:5800-7300` `DrawScene` 的完整 pass 顺序与 `Visibility` 生命周期（只读了片段 5849-5853、6813-6817、7168-7169、7179-7245）；
5. `RenderQueue` 类型定义与 `add/sort_opaque/sort_transparent` 的排序键（需在 `wiRenderer.h` 之外的头文件确认）；
6. `wiScene.cpp:4200-4250 / 4770-4800 / 5230-5380` bindless 几何/实例/雨水的逐帧写入细节；
7. `wiGPUBVH.cpp`（若存在）的 BVH 构建实现；SSS/地形/体素等未涉及本题范围；
8. `wiRenderPath3D.cpp`（3326 行）与 `wiRenderPath*` 系列：本次完全未读（属 task 其他成员范围）；
9. `wiScene_Components.cpp:1304-1450`（streamout/RT 渲染数据）与 `:2089`（lightmap BC 压缩）细节；
10. `wiShaderCompiler`（HLSL→DXIL 编译管线、shader dump 缓存）实现。

## 6. 踩到的坑
1. **grep 首次定位失败**：`struct ShaderMaterial` 不在 `WickedEngine/*.h`，而在 `shaders/ShaderInterop_Renderer.h`（CPU/GPU 共享互操作头），最初在错误目录搜索。
2. **`wiTextureHelper.h` 不含 `AddDeferredMIPGen`**：任务描述暗示它在纹素帮助层，实测定义在 `wiRenderer.cpp:19360` —— 与预期相反，已作为“资产层反向依赖渲染层”的证据记录。
3. **延迟销毁机制没有 grep 命中**：`DeferredDelete/destroyQueue` 等命名均不存在，真实机制是 `AllocationHandler::Update` + `destroyer_*` 队列，靠 `BUFFERCOUNT` 帧差判据。
4. **上下文压缩工具使用坑**：压缩摘要字符串里出现裸反引号会破坏内部解析（报 `Expected ',', got 'while'`）；必须对每块摘要做 `JSON.stringify` 后再拼接。另：摘要字符上限约 6000+。
5. **文件规模与预算**：`wiRenderer.cpp` 19686 行 / 729 KB，`wiScene.cpp` 368 KB，`wiGraphicsDevice_DX12.cpp` 7858 行 —— 不可整读，必须 grep 定位后按区间读（Lead 亦提醒）。

## 7. 时间线与状态
- 已完成（本轮）：Scene/ECS、wiGraphics 全套类型、DX12 后端头文件、ShaderMaterial 打包链路、Mesh/Object 组件 GPU 资源、渲染器 PSO 全变体预热与运行时取用、LoadShader 编译/加载、DX12 CreateShader/CreatePipelineState/SubmitCommandLists、资源管理器 Load 与延迟 MIP/BC 调度、模型导入（Editor 侧）、3DGS 全链路、适用性判断表（报告 §9.4）。
- 报告最终状态：**9 个必答问题全部由实测证据支撑**；全文仅 §1.4、§5.4 各 1 条【未确认】。
- 交付物：报告 258→约 320 行（含 §9.4 汇总表）；本日志。
- 约束：全程只读 WE 源码 + 只写本日志与报告两个文件；未 git checkout/restore/stash；未运行构建。