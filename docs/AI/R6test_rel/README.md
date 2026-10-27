# R6test_rel —— Release 构建的 -R6Test 结果（去重后）

Release 树（`out/build/Release`）跑 `-R6Test` 产出的 12 张 PNG，与 Debug 树（`docs/AI/R6test/`）逐张比对：

- **11 张渲染图 md5 完全相同** → Release 构建对渲染结果零影响。为避免仓库里存两份相同图片，这 11 张已删除，只在 `docs/AI/R6test/` 保留一份。
- **仅 `r6_11_imgui_panel.png` 不同** → 因为面板里显示 FPS / 帧时间文本，每次运行都不同（Debug 自身重跑一次 md5 也会变）。**这是预期差异，不是缺陷**；该图保留在此作为 Release 版的样本。
- 参考值：Release `9275810a4516da54a7264f60cc707cd8`；Debug 重跑样本 `058e1e2c94b272d20ce6ae3bbf14c592`（证明该图非确定性）。

复现命令（在 `out/build/Release/Demos/RendererDemoApp/` 下）：

```
RendererDemoApp.exe -StartRenderer RayTracingRenderer -R6Test <输出目录>
```
