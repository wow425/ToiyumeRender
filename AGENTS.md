# TooiyumeRender — AI 施工常驻规则

> 本文件只列**没读完整流程也不许犯**的错误。

## 构建与运行（唯一入口）

`scripts/` 已于 2026-11-15 移除，改为直接用 **VS 自带工具链**（不得脱离 VS 环境）：

```bat
call "F:\VS\Community\VC\Auxiliary\Build\vcvars64.bat"
:: 注意：必须 chcp 65001，否则 Ninja 的 msvc_deps_prefix 双重编码，改头文件不重编
set VCPKG_ROOT=

set CMAKE=F:\VS\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=F:\VS\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

:: 配置（Debug / Release 各一次；新增 .cpp 或改 CMakeLists 时删掉 out\build\<Config> 重配）
"%CMAKE%" -S . -B out\build\Debug -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe
:: 编译
"%CMAKE%" --build out\build\Debug --parallel
```

- 运行：`out\<Config>\Demos\<App>\<exe>`（例：`out\Debug\Demos\DeferredApp\DeferredApp.exe -Screenshot out\_shots`）。
- **禁止 `cmake --preset`**：预设会另建 `out/build/x64-debug`，与上面的 `out/build/<Config>` 形成双树，导致「构建了」≠「跑的是新 exe」。
- **产物（2026-11-15 起）**：`out/<Config>/{Engine,ThirdParty,Demos}/...`，镜像源码树层次；构建树（`CMakeCache.txt`/`.obj`/`build.ninja`）仍在 `out/build/<Config>/`。着色器产物 `out/<Config>/Engine/Shader/<HLSL子目录>/`。统一出口变量见 `Engine/20_cmake/OutputLayout.cmake`。
- exe 名不统一（Forward=`TooiyumeApp.exe`），路径为 `out/<Config>/Demos/<App>/`。
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

`docs/AI/worklogs/TR-YYYYMMDD-NN_<slug>.md`（`docs/AI` 的分类约定见 `docs/AI/README.md`）。
每条结论带 `路径:行号` 并注明基线；未验证项必须显式列出。

## 状态用词

`DONE` / `IMPLEMENTED_UNVERIFIED` / `PARTIAL` / `BLOCKED` / `REJECTED`。证据不足只能降低状态，不得拔高。
