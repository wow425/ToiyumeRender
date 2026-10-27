# R6switch —— R6 阶段 `-R3Test` 切换链路回归

命令（在 `out/build/Debug/Demos/RendererDemoApp/` 下）：

```
RendererDemoApp.exe -StartRenderer RayTracingRenderer -R3Test <本目录>
```

日志关键行（完整、无异常）：

```
[R3] 切换 Renderer： -> RayTracingRenderer
[R3] 切换完成：RayTracingRenderer（输出 已重绑）
[R3] 切换 Renderer：RayTracingRenderer -> ForwardRenderer
[DXR] RayTracingRenderer 已关闭（累计 DispatchRays 帧数=70）
[R3] 切换完成：ForwardRenderer（输出 已重绑）
[R3] 切换 Renderer：ForwardRenderer -> DeferredRenderer
[R3] 切换完成：DeferredRenderer（输出 已重绑）
```

## 与 R5 的比对（证明 R6 改动没破坏光栅链路）

| 文件 | 与 `docs/AI/R5switch/` 同名文件比对 | 说明 |
|---|---|---|
| `r3_07_forward.png` | **md5 完全相同** `ec5736bafa8120dbac9e58c299f3580b` | 已从本目录删除，见 `docs/AI/R5switch/` |
| `r3_08_back_deferred.png` | **md5 完全相同** `16c8aeec294844b9e3fedde4df78a349` | 已从本目录删除，见 `docs/AI/R5switch/` |
| `r3_09_imgui_panels.png` | 不同（面板新增「Ray Tracing (R6)」一节，且 FPS 文本变化） | **预期差异**，已从本目录删除，见本文说明 |
| `r3_00` ~ `r3_06` | R5 阶段没有这些文件（R5 的 `R3Test` 只存了 3 张） | 保留在本目录作补充证据 |

`r3_00_initial_deferred.png` 的 md5 是 `a43b605977a626e747c91559dcb1bb7f` —— 与 DXR 基线 `docs/AI/R6test/r6_00_baseline_shadow_on.png` **相同**。文件名里的 `deferred` 是脚本命名沿用的旧称：该步实际启动的是 `-StartRenderer RayTracingRenderer`，切换前渲染器就是光追，且参数与 `r6_00` 一致，故输出同图。

## 结论

切换链路 `RayTracingRenderer → ForwardRenderer → DeferredRenderer` 全程正常，光栅路径**零回归**。
