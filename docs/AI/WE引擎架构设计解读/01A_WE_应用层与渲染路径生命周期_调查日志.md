# 01A_WE_应用层与渲染路径生命周期_调查日志

> 作者：we-lifecycle（队友，共享任务 task-1）
> 调查对象：WickedEngine（`D:\CS-Self-Study\Computer_Graphics\DX12\工程实现参考\WickedEngine-master\`）+ TooiyumeRender 现状（工作树，HEAD `51c09d0`）
> 产出：`docs/AI/ToiyumeRender 架构演进专项约束/01A_WE_应用层与渲染路径生命周期.md`
> 约束遵守：未修改任何运行时代码；未执行 `git checkout/restore/stash`；未运行构建；只写了本日志与上述报告两个文件。

---

## 1. 实际读过的文件与行数（逐条）

### 1.1 WE 侧（全部整读或定位区间读）

| 文件 | 总行数 | 读取方式 | 读取行区间 |
| --- | --- | --- | --- |
| `WickedEngine/wiApplication.h` | 157 | 整读 | 1-157 |
| `WickedEngine/wiApplication.cpp` | 938 | 整读（分块，read limit=2000） | 1-938；另二次精读 355-390、718-869 |
| `WickedEngine/wiRenderPath.h` | 51 | 整读 | 1-51；**确认 .cpp 不存在**（glob/grep 实测） |
| `WickedEngine/wiRenderPath2D.h` | 142 | 整读 | 1-142 |
| `WickedEngine/wiRenderPath2D.cpp` | 550 | 整读 | 1-550 |
| `WickedEngine/wiRenderPath3D.h` | 402 | 整读 | 1-402 |
| `WickedEngine/wiRenderPath3D.cpp` | 3326 | **定位区间读**（Lead 要求控制预算）：函数行号索引 + 17-94 / 96-341 / 343-373 / 375-830 / 832-1786（骨架）/ 1788-1824 / 2334-2564 | 未整读，未读：1826-2333（RenderAO/SSR/SSGI/Outline/LightShafts/Volumetrics/SceneMIPChain/Transparents 函数体）、2566-2790（RenderCameraComponents）、2790-3326（setter 与截图） |
| `WickedEngine/wiRenderPath3D_PathTracing.h` | 53 | 整读 | 1-53 |
| `WickedEngine/wiRenderPath3D_PathTracing.cpp` | 733 | 定位区间读 | 45-48 / 50-143 / 145-300 / 302-712（骨架）/ 714-731 |
| `WickedEngine/wiFadeManager.h` | 76 | 整读 | 1-76 |
| `WickedEngine/wiFadeManager.cpp` | 63 | 整读 | 1-63 |
| `WickedEngine/wiCanvas.h` | 106 | 整读 | 1-106 |
| `WickedEngine/WickedEngine.h` | — | grep 定位 | 仅 :137（MainComponent 别名）；未整读 |
| `WickedEngine/wiScene.h` | — | 定位区间读 | 610-670（全局 Scene/Camera 单例） |
| `WickedEngine/wiJobSystem.h` | — | grep 定位 | API 行号 17-75 |
| `WickedEngine/wiGraphicsDevice_DX12.cpp` | 5800+ | 定位区间读 | 2686-2704 / 3090-3219 / 3220-3299 / 5388-5407 / 5456-5527 / 5528-5567 |
| `WickedEngine/wiLua.h`、`wiLua.cpp`、`wiLoadingScreen.h/.cpp`、`wiApplication_BindLua.cpp` | — | grep + 定点读 | 仅被引用的行（见报告附录 A） |
| `Content/Documentation/WickedEngine-Documentation.md` | — | grep 定位 | :293、:361 |
| `Samples/Template_Windows/main.cpp` | 100 | 整读 | 1-100 |
| `Samples/Example_ImGui/Example_ImGui.h/.cpp` | 24 / 444 | 定位读 | h 全读；cpp 108-158、160-310（骨架）、312-360、366-444 |
| `Samples/Example_ImGui/main_Windows.cpp` | 229 | 整读 | 1-229 |

**注意**：任务描述里的 `WickedEngine\wiRenderPath.cpp`、`WickedEngine\Tests\`、`Samples\Example_ImGui\main.cpp` 三个路径在本地**不存在**，已用实际路径替代（`wiRenderPath.h` header-only、`Samples/Tests/`、`main_Windows.cpp`）。

### 1.2 TooiyumeRender 侧

| 文件 | 总行数 | 读取方式 |
| --- | --- | --- |
| `docs/AI/ToiyumeRender 架构演进专项约束/ToiyumeRender 架构演进专项约束.md` | 121 | 整读 |
| `docs/AI/ToiyumeRender 架构演进专项约束/00_总览与阅读指南.md` | 133 | 整读 |
| `Engine/01_Application/GameCore.h` | 43 | 整读 |
| `Engine/01_Application/GameCore.cpp` | 179 | 整读 |
| `Engine/01_Application/Display.h` | 45 | 整读 |
| `Engine/01_Application/Display.cpp` | 267 | 整读 |
| `Engine/04_Renderer/Renderer/Base/BaseRenderer.h` | 218 | 整读 |
| `Engine/04_Renderer/Renderer/Base/RendererRegistry.h` | 60 | 整读 |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.h` | 105 | 整读 |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.h` | 154 | 整读 |
| `Engine/04_Renderer/Renderer/RendererConfig.h` | 33 | 整读 |
| `Demos/DeferredApp/DeferredApp.cpp` | 148 | 整读 |
| `Demos/ForwardApp/Tooiyume.cpp` | 157 | 整读 |

## 2. 用到的命令（可复现）

```bash
# 目录/体积统计
find Engine -type d | head -80
find 01_Application 04_Renderer 02_RHI 05_Scene 00_Core -name '*.h' -o -name '*.cpp' | xargs wc -l | sort -rn
# 反向依赖与耦合面
grep -rn 'include .*wiApplication\.h' WickedEngine-master
grep -rn 'wi::Application|GetActivePath' WickedEngine-master/WickedEngine
grep -rn 'frame_fence' WickedEngine-master/WickedEngine/wiGraphicsDevice_DX12.cpp
grep -rn 'CreateSwapChain' WickedEngine-master/WickedEngine/wiGraphicsDevice_DX12.cpp
grep -rn 'class Canvas|struct Canvas' WickedEngine-master
grep -rn 'GetScene\(\)|GetCamera\(\)' WickedEngine-master/WickedEngine/wiScene.h
grep -rn 'MainComponent' WickedEngine-master
# TooiyumeRender 现状缺口
grep -rn 'OnResize|IdleGPU|Display::Resize' Engine
grep -rn 'OnResize|IdleGPU|Display::Resize' Demos
# 基线核对（只读）
git log -1 --format='%H %ci'
git status --short
for f in ...; do printf '%s worktree=%s HEAD=%s\n' "$f" "$(wc -l < $f)" "$(git show HEAD:$f | wc -l)"; done
git diff --stat -- Engine/01_Application/GameCore.cpp Engine/01_Application/Display.cpp ...
git diff -U2 -- Engine/01_Application/GameCore.cpp | head -80
```

所有读取均通过只读工具（`read`/`grep`/`glob`/`ops_bash` 的只读命令）完成。

## 3. 「实测」与「推断」的边界

**实测**（在命令输出里直接看到原文）：报告中所有带具体行号且引用了代码片段的结论，包括：
- `wiRenderPath.h` 全部虚函数清单与非纯虚事实；
- `ActivatePath` → `FadeManager` 的切换时序（`wiApplication.cpp:64-87` + `wiFadeManager.cpp:26-45`）；
- `Run()` 的 27 步帧序表；
- `wiApplication.cpp:388` 唯一提交点、`wiGraphicsDevice_DX12.cpp:5473/5495` Present、`:5510-5525` 帧末队列互等、`:5531-5546` 帧首三缓冲 fence；
- `wiGraphicsDevice_DX12.cpp:3108` 交换链重建时的 `WaitForGPU()`、`:3233-3244` ResizeBuffers 分支；
- `wiRenderPath3D.cpp:96-341` 全部 RT 的格式/分辨率/采样数；`:2473` tonemap 分界；`:2544 lastPostprocessRT`；
- PathTracing 的 4 个 override + `:142` 跳过基类注释；
- TooiyumeRender 的 `IGameApp` 接口、主循环、`OnResize` 无调用者、HDR 缺失（`RendererConfig.h:17`）、Demo 直接持有具体 Renderer。

**推断**（报告中已逐条标 `[推断]`）：
1. §2.3「把切换放到 Update 阶段是为了保证同帧内不发生渲染中途换路径」——依据是相邻行顺序，无文字说明。
2. §2.4/§6.4「`Stop()` 立即置空 RT 的安全性依赖设备延迟释放 + 帧首 fence」——未读设备释放路径。
3. §4.4/§10.3「全局 GetDevice/Scene 是路径零 Application 依赖的代价」——依据声明与调用点推断。
4. §5.3 四个结果 getter 的「最小契约」意图 —— 依据调用点与默认返回值推断。
5. §7.2「`RenderPath3D::ResizeBuffers` 无 fence 而安全」——同上未读释放路径。
6. §9.2「PathTracing 沿用基类 `PreRender` 导致冗余分配」——**未确认**，见 §5 未读清单第 1 条。
7. §9.3 新增路径的最小清单、§10.2 逐条适用性判断、§11.3 四条行动建议 —— 均为基于实测证据的设计判断，非 WE 源码事实。

## 4. 关键取证难点与踩坑记录

1. **任务描述的文件路径有 3 处不存在**：`wiRenderPath.cpp`（RenderPath 是纯 header-only，51 行全内联）、`WickedEngine/Tests/`（实际 `Samples/Tests/`）、`Samples/Example_ImGui/main.cpp`（实际 `main_Windows.cpp` / `main_SDL2.cpp`）。用 `ls`/`glob` 核对目录后才继续。
2. **`glob` 对 `Samples/**/main.cpp` 返回空**：模式与实际目录层级不匹配，改用 `ops_bash` 的 `find/ls` 列出真实文件名。
3. **`read` 的 limit 上限 2000 行**：`wiRenderPath3D.cpp`（3326）与 `wiApplication.cpp`（938）必须分块。按 Lead 第 1 点提示，`wiRenderer.cpp`（19686 行）**完全没有整读**，只用 grep 取了 `:2147-2149` 的 renderpass 格式行。
4. **大文件整读会耗尽上下文**：第 1 轮我把 `wiRenderPath3D.cpp` 的骨架与 `wiRenderPath3D_PathTracing.cpp` 全部读完后触发了压缩（本次会话共压缩 4 个区间）。后续改为「grep 定位函数符号 → read 定位区间」。
5. **工作树与 HEAD 不一致**：`git status` 显示 20+ 文件被修改。00_总览 §3.2 记录的「`DeferredRenderer.cpp` 被截断成 30 行」在当前工作树**已不存在**（现 655 行，HEAD 638 行）。为让 TooiyumeRender 侧引用可用，我逐文件比对了 `wc -l` 与 `git diff`：引用的 `GameCore.cpp`（+19 行，全是 asset root 与 `Printf` 日志）、`Display.cpp`（+2 行日志）结论在 HEAD 上同样成立，已写入报告 §11.1 末尾。
6. **`RenderPath` 没有 `RenderPath.cpp` 这一点很容易误导**：51 行 header 全部内联空实现，因此「基类是不是纯虚接口」必须通读才能确认（结论：一个纯虚都没有）。
7. **「Present 在哪一行」不是显式代码**：WE 不在 Application 里写 `Present()`，而是 `RenderPassBegin(&swapChain, cmd)` 把交换链登记进 `commandlist.swapchains`（`wiGraphicsDevice_DX12.cpp:5473`）。第一轮通读 `wiApplication.cpp` 时容易误以为没有 Present。

## 5. 剩余未读清单（每条写「为什么要读」）

| # | 未读内容 | 为什么要读 | 状态 |
| --- | --- | --- | --- |
| 1 | `wiRenderPath3D.cpp:375-830` 中每个资源创建条件（尤其 `:587-617 rtVelocity`、`:650-674 visibilityResources`） | 判断 PathTracing 沿用基类 `PreRender` 是否真的产生冗余分配 | 未确认（报告 §9.2/§12.1） |
| 2 | `wiGraphicsDevice_DX12.cpp` 的资源销毁路径（`DestroyResource` / deferred deallocation / `FlushDeallocations`） | 确认 `Stop()` 立即置空 RT 的 GPU 安全性（原则⑤核心） | 未确认（报告 §2.4/§7.2/§12.2） |
| 3 | `wiRenderPath3D.cpp:1826-2333` 的子阶段函数体（RenderAO/SSR/SSGI/Outline/LightShafts/Volumetrics/SceneMIPChain/Transparents） | 判断这些 virtual 子阶段是否可作为「公共功能边界」（更偏 01B 的职责） | 未读，可留给 01B |
| 4 | `wiLoadingScreen.h/.cpp` 全文 | 完整解释「路径反向依赖 Application」的唯一案例 | 只读了 3 行 |
| 5 | `Editor/Editor.cpp`（尤其 :360 附近 `renderComponent` 声明） | 多路径共存的真实持有方式与 CrossFade 实用法 | 未读 |
| 6 | `Engine/02_RHI/Command/CommandContext.cpp`（`Finish`）、`CommandListManager.h:98`（`IdleGPU`） | 本项目侧同步语义（对比 WE 帧 fence） | 未读，偏 02 审计 |
| 7 | `DeferredRenderer.cpp:139`/`ForwardRenderer.cpp:95` 的 `OnResize` 函数体 | 确认「补上调用点」后是否真的能重建 GBuffer | 未读 |
| 8 | `Samples/Tests/Tests.cpp`（1261 行）、`Samples/Example_ImGui_Docking/*` | 多路径/多窗口用法补充 | 未读（低优先，Samples 证据已足够） |

## 6. 交付物状态

| 交付物 | 状态 |
| --- | --- |
| `docs/AI/ToiyumeRender 架构演进专项约束/01A_WE_应用层与渲染路径生命周期.md` | **已完成 v1**（782 行）：§0 摘要 8 条、§1 术语与 MainComponent 事实、§2-§10 对应问题 1-9、§11 TooiyumeRender 对照（含体积表与 4 条行动建议）、§12 未确认清单 7 条、附录 A/B 证据索引 |
| 本日志 | 已完成 |
| 运行时代码 | 未改动（只读） |
| 构建/测试 | 未运行（阶段禁止） |
