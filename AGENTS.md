# TooiyumeRender — AI 施工常驻规则

> 完整流程见 `docs/AI/渲染器AI编程流程skill/SKILL.md`。本文件只列**没读 skill 也不许犯**的错误。

## 构建与运行（唯一入口）

```bash
set VCPKG_ROOT=F:/VS/rep/vcpkg        # 每 shell 必设
scripts\build.bat Debug               # 新增 .cpp / 改 CMakeLists 时加 clean
scripts\build.bat Release             # 必须与 Debug 同跑
scripts\run.bat Debug Deferred
```

- **禁止 `cmake --preset`**：产物落 `out/build/x64-debug`，与 `build.bat` 的 `out/build/<Config>` 不同。两目录并存会导致「构建了」≠「跑的是新 exe」。
- 产物：`out/build/<Config>/Demos/<App>/`；exe 名不统一（Forward=`TooiyumeApp.exe`）。
- 新增 `.cpp` 必须 `clean`（`GLOB_RECURSE` 无 `CONFIGURE_DEPENDS`）。

## 验证（分层，缺层必须标注）

1. 构建 Debug+Release exit 0 → 2. 启动不崩 → 3. 日志无 error → 4. **画面只认 GPU 回读** `-Screenshot <dir>` / `-ResizeTest` → 5. Debug Layer 无 ERROR → 6. 人肉项（AI 必须写「未验证」）

- **禁止**用 PrintWindow 截图当画质证据（抓 DX12 会得陈旧帧）。
- **禁止**把 `timeout` 的 124（超时被杀）写成「运行通过」。
- **禁止**把编译通过当成功能完成。

## 不可违反

- 不得 `git reset --hard` / `git clean -fd` / 覆盖用户未提交改动（本仓长期有 30–60 项未提交改动）。
- 一次只做一个可独立验收的工作包；发现新问题登记，不顺手做。
- 改 HLSL register / CB 布局 / 材质槽位 → 必须同步所有消费点，并 `-RebuildModels`。
- `DeferredRenderer` 有 6 段空壳（SSAO/Shadow/Transparent/PostProcess/TAA/Tonemap），**只有 `PIXBeginEvent` 没有 GPU 命令** —— 不得当作已实现，也不得在架构迁移期顺手填。
- 架构迁移完成前禁止新增大规模渲染特效、RenderGraph、完整 ECS。

## 施工文档（每次必写）

`docs/AIWorkLogs/TR-YYYYMMDD-NN_<slug>.md`，模板见 skill 的 `assets/worklog-template.md`。
每条结论带 `路径:行号` 并注明基线；未验证项必须显式列出。

## 状态用词

`DONE` / `IMPLEMENTED_UNVERIFIED` / `PARTIAL` / `BLOCKED` / `REJECTED`。证据不足只能降低状态，不得拔高。

## 自检

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File "docs/AI/渲染器AI编程流程skill/scripts/ai-check.ps1"
```
