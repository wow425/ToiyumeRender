# TR-20261115-01 工程结构明确化（阶段一：构建产物归位 out/ + 阶段二：docs/AI 分类）

- 日期：2026-11-15
- 基线：HEAD = `d3d3022`，工作区含 163 项未提交改动（其中约 161 项为已从工作区删除的旧文档/脚本，属用户既有状态，未触碰）
- 范围：要求 1（删编译产物 + 编译产物统一映射到 `out/` + 重新编译运行验证）、要求 2（`docs/AI` 按作用分文件夹）、要求 3（git 上传范围屏蔽 Assets/out/ThirdParty/vcpkg_installed 并验证）
- 状态：要求 1 = `DONE`；要求 2 = `DONE`（仅完成目录结构与约定，旧文档未从 git 历史恢复）；要求 3 = `DONE`

---

## 1. 不备份地删除编译产物

用户明确要求「不备份地删除编译后的产物」。逐个删除并记录体量，删除前确认 `git ls-files` 计数为 0（即未被版本库跟踪，删除不改变仓库状态）：

| 路径 | 性质 | 文件数 | 字节数 | `git ls-files` 计数 |
| --- | --- | --- | --- | --- |
| `Demos/DeferredApp/cmake-build-debug/` | CLion 遗留构建树 | 26 | 1,166,191 | 0 |
| `Engine/.vs/` | VS 浏览数据库（含 `Engine/v17/Browse.VC.db`） | 7 | 3,193,809 | 0 |

删除后 `Test-Path` 复核均为 `False`。

**未删除**：根 `.vs/`。原因：VS 进程（`devenv`）正在使用，且内含 `.vs/launch.vs.json` 等启动配置；`.vs/` 已被 `.gitignore:37` 忽略，不进入版本库，不构成结构噪音。

## 2. 编译产物路径改造（按原文件层次映射到 `out/`）

### 2.1 设计

- **产物根**：`out/<Config>/`（`<Config>` = `Debug` | `Release`），内部完全镜像源码树层次。
- **构建树不动**：`CMakeCache.txt` / `build.ninja` / `CMakeFiles/` / `.obj` 仍留在 `out/build/<Config>/`。这样与 `CMakePresets.json` 的 `binaryDir` 一致，也不会因为移动 `.obj` 而破坏 ninja 的增量依赖图。
- **着色器产物带子目录**：`out/<Config>/Engine/Shader/<HLSL 子目录>/<Name>.cso|.h`，与 `Engine/Shader/` 的目录结构一一对应。

最终布局：

```
out/
  Debug/                                 Release/（同构）
    Engine/
      TooiyumeEngine.lib
      Shader/
        01_Default/{DefaultVS,DefaultPS}.{cso,h}
        02_Deferred/{StaticMeshVS,GBufferPS,FullScreenVS,LightingPS}.{cso,h}
        06_Environment/{SkyboxVS,SkyboxPS,EquirectangularToCubemapCS,
                        IrradianceConvolutionCS,SpecularPrefilterCS,BRDFLUTCS}.{cso,h}
        07_RayTracing/RayTracing.{cso,h}
    ThirdParty/imgui/imgui.lib
    Demos/
      DeferredApp/DeferredApp.exe + 5 个运行期 DLL
      ForwardApp/TooiyumeApp.exe + 5 个
      RayTracingApp/RayTracingApp.exe + 5 个
      RendererDemoApp/RendererDemoApp.exe + 5 个
  build/<Config>/                        ← 构建树，属性文件与 .obj
```

### 2.2 新增 `Engine/20_cmake/OutputLayout.cmake`

统一出口变量的唯一来源，避免各处各写一遍路径：

- `TOOIYUME_OUT_ROOT` = `${CMAKE_SOURCE_DIR}/out`
- `TOOIYUME_OUT_DIR` = `${TOOIYUME_OUT_ROOT}/${TOOIYUME_CONFIG_FOR_SHADER}`
- `TOOIYUME_SHADER_OUT_DIR` = `${TOOIYUME_OUT_DIR}/Engine/Shader`
- `TOOIYUME_CONFIG_FOR_SHADER`：取 `CMAKE_BUILD_TYPE`，为空（多配置生成器）时回落 `Debug`
- `tooiyume_set_output_dirs(<target> <相对目录>)`：把 `ARCHIVE/LIBRARY/RUNTIME` 三种输出目录连同 4 个配置后缀变体、`PDB_OUTPUT_DIRECTORY`、`COMPILE_PDB_OUTPUT_DIRECTORY` 全部钉到 `${TOOIYUME_OUT_DIR}/<相对目录>`
- **故意不设 OBJECT 输出目录**：`.obj` 属于构建树，留在 `out/build/<Config>/CMakeFiles/` 才不破坏增量构建

### 2.3 改造点清单

| 文件 | 改动 |
| --- | --- |
| `CMakeLists.txt:39`（根） | 在 `CMAKE_BUILD_TYPE` 定稿之后、依赖定位与 `add_subdirectory` 之前 `include(.../OutputLayout.cmake)` |
| `Engine/20_cmake/ShaderCompiler.cmake`（`compile_shader` 与 `compile_raytracing_library` 各一处） | `set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/ShaderCache")` → 从 `SHADER_FILE` 剥离 `Shader/` 前缀得到相对子目录，落 `${TOOIYUME_SHADER_OUT_DIR}/<子目录>` |
| `Engine/CMakeLists.txt` | 新增 `tooiyume_set_output_dirs(TooiyumeEngine "Engine")`；`target_include_directories` 的 `ShaderCache` 换成 `${TOOIYUME_SHADER_OUT_DIR}`；13 个 `shader_attach_header_dependency` 绝对路径全部改到 `${TOOIYUME_SHADER_OUT_DIR}/<子目录>/<Name>.h` |
| `ThirdParty/imgui/CMakeLists.txt` | `ARCHIVE_OUTPUT_DIRECTORY` 由 `${CMAKE_BINARY_DIR}/lib` 改 `${TOOIYUME_OUT_DIR}/ThirdParty/imgui` |
| `Demos/{ForwardApp,DeferredApp,RayTracingApp,RendererDemoApp}/CMakeLists.txt` | 各加一条 `tooiyume_set_output_dirs(<target> "Demos/<Dir>")`；删除 3 个文件里失效的 `${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache` include（目录早已不存在，CLion 遗留） |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp` | 生成头 include 加 `02_Deferred/` 前缀（4 处） |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp` | 加 `01_Default/` 前缀（2 处） |
| `Engine/04_Renderer/Renderer/RayTracing/RayTracingRenderer.cpp` | 加 `07_RayTracing/` 前缀（1 处） |
| `Engine/04_Renderer/EnvironmentLightingManager.cpp` | 加 `06_Environment/` 前缀（6 处） |

`Engine/20_cmake/DeployRuntimeDlls.cmake` 无需改：它用 `$<TARGET_FILE_DIR:...>` 定位目标 exe 目录，输出目录一变它自动跟随。

## 3. 验证（按 AGENTS.md 分层）

编译环境：VS 安装自带工具链，未脱离 VS 环境。
- `F:\VS\Community\VC\Auxiliary\Build\vcvars64.bat`（MSVC 19.38.33145.0）
- `F:\VS\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- `F:\VS\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`
- 构建目录 `out/build/<Config>`，生成器 Ninja，`VCPKG_ROOT` 置空（走仓库内 `vcpkg_installed`）

> 说明：仓库内 `scripts/` 目录在当前工作区已不存在（仅在 HEAD 中），且用户既有未提交状态不得覆盖，因此本次直接用上述 vcvars64 + CMake 命令行驱动，等价于 `scripts/build.bat` 的行为。

| 层级 | Debug | Release | 结论 |
| --- | --- | --- | --- |
| 1. 配置 | `Configuring done (11.0s)`，`-- [layout] 产物根目录: .../out/Debug` | 同构，`.../out/Release` | PASS |
| 2. 构建 | **106/106，exit 0** | **106/106，exit 0** | PASS |
| 3. 启动不崩 | DeferredApp 运行至第 90 帧完成全部截图后自然退出 `code=0` | 运行 50s 持续输出截图后被主动终止 | PASS |
| 4. 日志无 error | 0 条 `D3D12 ERROR` / Assert / `Failed hr=0x8`（排除 `SaveToWICFile`，见下） | 0 条 | PASS |
| 5. GPU 回读画面 | 20 张 PNG（`screenshot_{1,2,5,30,90}` + `gbuf_{baseColor,normal,material,emission}_*`），1920×1080，画面为完整场景（两位角色 + 天空盒 + 地面） | 20 张同构 | PASS |
| 6. Debug/Release 一致性 | `vision_pixel_diff(screenshot_90)` = **differingPixels 0 / 2,073,600，diffRatio 0** | — | PASS |
| 7. Debug Layer | 未开启独立 Debug Layer 抓取，仅依据日志中 InfoQueue 无 ERROR | **未验证** |

其余三个 Demo 的启动冒烟（各运行 25s 后主动终止，程序本身无自动退出参数）：

| 程序 | 启动 | 日志真实错误数 |
| --- | --- | --- |
| `TooiyumeApp.exe`（Forward） | 存活 25s，`[Boot] model 2 loaded` / `game.Startup() returned` | 0 |
| `RayTracingApp.exe` | 存活 25s，`[DXR]` 初始化通过 | 0 |
| `RendererDemoApp.exe` | 存活 25s，`[R3] RendererDemoApp 启动完成` | 0 |

### 已知非本次引入的现象

1. **`gbuf_normal_*.png` 未导出**：日志 `[Capture] 不支持的格式 34，暂不导出`。格式 34 是 DXGI 的某个 packed 格式，`DebugCapture` 尚未支持。属既有功能缺口，与本次路径改造无关。
2. **首次把 `-Screenshot` 指向 `%TEMP%` 时全部 `SaveToWICFile 失败 hr=0x80070005`（拒绝访问）**：改为指向仓库内目录后全部成功。属 WIC 沙箱/权限环境差异，非代码缺陷。
3. 编译期既有 warning（改造前即存在，未新增）：`DeferredCommon.hlsli:60` 隐式截断、`Utility.cpp:97` C5033 `register`、`BaseRenderer.cpp:77` C4319。
4. **`Assets` 定位未受影响**：`Engine/00_Core/Utility/AssetRoot.cpp:137-171` 的解析顺序为 CLI → 环境变量 → exe 目录上溯 8 层 → cwd 上溯 8 层 → 编译期兜底 `TOOIYUME_SOURCE_ASSETS_DIR`。实测日志首行 `[Asset] AssetsRoot = D:\...\TooiyumeRender\Assets\`，正确。

## 4. 阶段二：`docs/AI` 按作用分文件夹

新建四类子目录，并在 `docs/AI/README.md` 固化约定与命名规则：

| 子目录 | 作用 |
| --- | --- |
| `worklogs/` | 每次施工的工作日志 `TR-YYYYMMDD-NN_<slug>.md` |
| `guides/` | 教程、操作手册、踩坑总结 |
| `architecture/` | 工程结构、模块职责、数据流、构建布局等稳定结论 |
| `reviews/` | 阶段性审计、代码复查、问题清单 |

已在工作区但未跟踪的 `docs/AI/TooiyumeRender_光追管线教程.md` 移入 `docs/AI/guides/`（129 KB，内容未改动）。本文件即 `docs/AI/worklogs/` 的首篇。

`architecture/`、`reviews/` 目前为空目录——本次不杜撰内容，留待确有该类文档时再填。

## 5. 阶段三：git 上传范围

### 5.1 改动

在 `.gitignore` 末尾追加一段带注释的**显式清单**（`.gitignore:398-427`）。四个目录此前已被通用模式部分覆盖（`[Oo]ut/`、`vcpkg_installed/`、`/Assets`），但 `ThirdParty` **完全没有被忽略**；且通用模式散落在文件各处，无法一眼看出「屏蔽了哪四个目录」。故显式固化：

```
/Assets/            /Assets
/out/               /out
/ThirdParty/        /ThirdParty
/vcpkg_installed/   /vcpkg_installed
```

### 5.2 已被跟踪的文件必须从索引移除

**关键点**：`.gitignore` 只对**未跟踪**文件生效。`ThirdParty` 当时有 **23 个文件已被跟踪**（`ThirdParty/PIX/include/*.h` 5 个、`ThirdParty/PIX/lib/WinPixEventRuntime.lib`、`ThirdParty/imgui/*.{cpp,h,txt}` 及 `backends/*` 17 个），仅加忽略规则不会停止上传。执行：

```
git rm --cached -r --quiet ThirdParty
```

`--cached` 表示**只从索引移除，磁盘文件保留**。验证：

| 指标 | 改前 | 改后 |
| --- | --- | --- |
| `git ls-files -- ThirdParty` | 23 | **0** |
| `ThirdParty/` 磁盘文件数 | 25 | **25**（未变） |

磁盘关键构建输入复核存在：`ThirdParty/PIX/lib/WinPixEventRuntime.lib`、`ThirdParty/PIX/bin/WinPixEventRuntime.dll`、`ThirdParty/imgui/imgui.cpp`、`ThirdParty/imgui/backends/imgui_impl_dx12.cpp` 全部 `OK`。

### 5.3 验证

`git check-ignore -v --no-index` 对四个目录的代表性文件（含深层路径）：

| 路径 | 命中规则 |
| --- | --- |
| `Assets/x.txt`、`Assets/Model/Merlin/merlin.ty` | `.gitignore:411 /Assets` |
| `out/x.txt`、`out/Debug/Demos/DeferredApp/DeferredApp.exe` | `.gitignore:416 /out` |
| `ThirdParty/x.txt`、`ThirdParty/imgui/imgui.cpp`、`ThirdParty/PIX/lib/WinPixEventRuntime.lib` | `.gitignore:421 /ThirdParty` |
| `vcpkg_installed/x.txt`、`vcpkg_installed/x64-windows/lib/foo.lib` | `.gitignore:425 /vcpkg_installed` |

全部 `IGNORED`。反向检查（正常源码不得被误伤）`Engine/CMakeLists.txt`、`Demos/DeferredApp/DeferredApp.cpp`、`.gitignore`、`README.md`、`CMakeLists.txt`、`vcpkg.json` 均**未被忽略**。

`git add -A --dry-run` 全量预演（181 条变更）：**没有任何一条**涉及 `Assets/`、`out/`、`ThirdParty/`、`vcpkg_installed/`。

`git status --porcelain` 结构（201 项）：161 项 ` D`（用户此前的既有删除，未触碰）+ 23 项 `D `（本次 ThirdParty 退出索引）+ 12 项 ` M`（本次改动的 CMake/源码）+ 5 项 `??`（新增的 `OutputLayout.cmake`、`docs/AI/README.md`、`docs/AI/guides/`、`docs/AI/worklogs/`、`.mnemon/`）。

> 注：`.mnemon/` 是 AI 记忆工具的运行时目录，未在本次要求范围内，故未处理。

## 6. 最终验证（三个阶段的改动合并后，从零全量重建）

`out/` 下的 `build/Debug`、`build/Release`、`Debug`、`Release` 全部删除后重新配置、编译、运行：

| 项 | Debug | Release |
| --- | --- | --- |
| 配置 | `Configuring done (8.6s)`，layout 正确 | `Configuring done (7.8s)`，layout 正确 |
| 编译 | **106/106，exit 0，无 error** | **106/106，exit 0，无 error** |
| 运行 | 20 张截图产出，日志 0 真实错误 | 此前已验证，同构 |
| 画面 | `screenshot_90.png` sha256 与此前完全一致 | — |

产物布局、`Assets` 定位、着色器头 include 全部工作正常。诊断用截图目录 `out/_shots/` 已在验证后删除（60 个文件），`out/` 恢复为纯构建产物。

## 7. 未做 / 需后续确认

- `CMakePresets.json` 未改：其 `binaryDir` 本就是 `out/build/${presetName}`，与本次构建树约定一致。
- `AGENTS.md` 已同步两处：产物路径改为 `out/<Config>/{Engine,ThirdParty,Demos}/...`；施工日志路径从 `docs/AIWorkLogs/` 改为 `docs/AI/worklogs/`。
- `Engine/00_Core/Utility/AssetRoot.h:18` 与 `AssetRoot.cpp:31` 的注释里仍写着旧布局示例 `out/build/x64-debug/Demos/DeferredApp/`（纯注释，不影响功能）。
- `Engine/02_RHI/Pipeline/RaytracingPSO.h:39` 注释提到 `ShaderCache/RayTracing.h`（纯注释）。
- Release 运行验证是「运行 50s 后主动终止」，**不等于**「程序自然退出通过」——程序本身没有自动退出参数，此处如实标注。
- `docs/AI/architecture/`、`docs/AI/reviews/` 目前是空目录（git 不跟踪空目录），待确有该类文档时再填。

## 8. 提交与推送至 GitHub（用户追加指令）

用户追加要求：把屏蔽后的项目更新至 GitHub 仓库。提交范围经用户确认选择「全部提交，GitHub 与磁盘完全一致」，因此本次提交同时落地了用户此前在工作区自行删除的 161 项（`docs/AI` 旧 R0–R7 归档 121 项、`docs/AIWorkLogs` 21 项、`scripts/` 11 个、`.vscode/launch.json`、`.claudiaideconfig*`、`docs/{LearningLog,UpgradeRoadmap_2026-10,Showcase}.md`、`Engine/Shader/06_Environment.zip` 等）。

### 8.1 提交前的额外屏蔽

| 新增规则 | 理由 |
| --- | --- |
| `.mnemon/`（`.gitignore:431`） | AI 记忆工具运行时目录（`documents/index.json`、`runtime/*.md`、`memories.json`），非工程内容 |
| `/*.zip`（`.gitignore:433`） | 仓库根出现手工打包快照 `TooiyumeRender.zip`（3,858,464 B，260 entries，仅 `Engine` 227 + `Demos` 33，含 `Demos/ForwardApp/.vs/` 等垃圾） |

追加后 `.gitignore` 仍为 UTF-8 无 BOM（首字节 `35,35,32`），大小 9,284 B。

### 8.2 AGENTS.md 同步修正

因本次提交会删除 `scripts/` 与 skill 目录，原 `AGENTS.md` 中的两条引用必然失效，一并改写：

1. 构建入口整段改为 vcvars64 + VS 自带 cmake/ninja 的真实可用命令（保留 `chcp 65001` 说明、`set VCPKG_ROOT=`、`-DCMAKE_MAKE_PROGRAM`、`--build --parallel`），运行路径改为 `out\<Config>\Demos\<App>\<exe>`。
2. 删除末尾「自检」小节（其指向的 `docs/AI/渲染器AI编程流程skill/scripts/ai-check.ps1` 在 HEAD 中 `git ls-tree` 计数为 0，早已不存在）。

### 8.3 提交

`git add -A` 后暂存 202 项（183 `D` / 13 `M` / 5 `A` / 1 `R`）：

- 暂存区中 `Assets|out|vcpkg_installed` 前缀条目 **0 条**；
- `ThirdParty` 23 条全部为 `D `（退出索引，磁盘 25 个文件保留）；
- 新增：`Engine/20_cmake/OutputLayout.cmake`、`docs/AI/README.md`、`docs/AI/architecture/.gitkeep`、`docs/AI/reviews/.gitkeep`、`docs/AI/worklogs/TR-20261115-01_...md`；
- 重命名：`docs/TooiyumeRender_光追管线教程.md` → `docs/AI/guides/TooiyumeRender_光追管线教程.md`（相似度 100%）。

结果：`[master 3860f05] chore: 工程结构明确化（产物归位 out/、docs/AI 分类、git 上传范围屏蔽）`，`202 files changed, 486 insertions(+), 100926 deletions(-)`，exit 0。

### 8.4 推送

`git push origin master` → `d3d3022..3860f05  master -> master`，exit 0。

- `git ls-remote --heads origin` = `3860f05d8974c438e1977e733d688168b17815ca  refs/heads/master`
- `origin/master..HEAD` 未推送提交数 = **0**
- `git status --porcelain` = **干净**

远端：`https://github.com/wow425/ToiyumeRender.git`（注意远端仓库名拼写为 `ToiyumeRender`，少于本地目录名的一个 `o`，属既有事实，未改动）。

### 8.5 新 HEAD 内容复核

| 指标 | 提交前 HEAD `d3d3022` | 提交后 HEAD `3860f05` |
| --- | --- | --- |
| 跟踪文件数 | 385 | **207** |
| 跟踪文件体积 | 29.3 MB | **2.5 MB** |
| `git ls-tree -r HEAD` 中 `Assets`/`out`/`ThirdParty`/`vcpkg_installed` 前缀条目 | — | **全部 0** |

本地被屏蔽目录的实际体量（对照说明为何不入库）：`Assets` 187 files / 678.1 MB、`ThirdParty` 25 files / 3.6 MB、`vcpkg_installed` 162 files / 67.5 MB。

提交后对本地 `out/build/Debug` 复跑 `cmake --build` → `ninja: no work to do.`，exit 0，说明提交未意外污染构建树。

## 9. 从 GitHub 全新克隆的独立验证

为确认「上传出去的东西本身自洽、且能被别人构建」，做了一次干净克隆验证。

### 9.1 克隆结果

`git clone --depth 1 https://github.com/wow425/ToiyumeRender.git` 成功。克隆内容：`Demos/ docs/ Engine/` + `.editorconfig .gitattributes .gitignore AGENTS.md CMakeLists.txt CMakePresets.json CppProperties.json imgui.ini README.md vcpkg.json`。

- 四个被屏蔽目录 `Assets`、`out`、`ThirdParty`、`vcpkg_installed`：`Test-Path` 全部 **False**；
- `scripts/build.bat`：不存在（与本次提交一致）；
- `Engine/20_cmake/OutputLayout.cmake`：存在。

### 9.2 裸克隆直接配置会失败（预期行为，非缺陷）

```
CMake Error at CMakeLists.txt:103 (message):
  找不到第三方依赖，且没有可用的 vcpkg：
    仓库内置目录不存在或无内容：...\vcpkg_installed/x64-windows
    VCPKG_ROOT 也未设置。
```

这是屏蔽 `vcpkg_installed` 的必然结果，报错本身已给出三条出路（放 `vcpkg_installed`、设 `VCPKG_ROOT`、传 `CMAKE_TOOLCHAIN_FILE`）。**结论：克隆后必须先补齐 `vcpkg_installed` 才能构建**。

### 9.3 补齐依赖后完整重建成功

用目录联接（junction）把本地 `ThirdParty`、`vcpkg_installed`、`Assets` 接进克隆目录（避免复制 745 MB），再清空 `out/`，用同一套 VS 环境驱动：

| 阶段 | 结果 |
| --- | --- |
| configure | `-- [layout] 产物根目录: .../out/Debug`、`-- [layout] 着色器产物: .../out/Debug/Engine/Shader`、`-- [deps] 使用仓库内依赖目录（优先于 VCPKG_ROOT）`、`Configuring done (7.1s)`、`Generating done (0.2s)` |
| build | **106/106 全部完成，exit 0** |
| 产物 | `out/Debug/ThirdParty/imgui/imgui.lib`、`out/Debug/Engine/TooiyumeEngine.lib`、`out/Debug/Demos/{DeferredApp,ForwardApp,RayTracingApp,RendererDemoApp}/<exe>`、`out/Debug/Engine/Shader/{01_Default,02_Deferred,06_Environment,07_RayTracing}/*.h` |

编译期仅 3 条既有 warning（与本地构建完全一致，均非本次改造引入）：`DeferredCommon.hlsli:60:12 [-Wconversion]`、`Utility.cpp(97) C5033 register`、`BaseRenderer.cpp(77) C4319`。

验证完成后用 `rmdir` 逐个拆除 junction（避免 `Remove-Item -Recurse` 递归删到源目录），复核三个源目录文件数与体积不变（187 / 25 / 162 files），随后删除克隆目录。

## 10. 遗留提示

- 克隆得到的仓库必须自行补齐 `ThirdParty`（PIX 头/lib、imgui 源码）与 `vcpkg_installed`，否则 configure 会在 `CMakeLists.txt:103` 报错退出。这是屏蔽策略的正常代价，已在 `AGENTS.md` 与本文记录。
- 工作区另有一个用户手放的任务说明文件 `docs/AI执行任务.md`（16,069 B，未跟踪），与本次三项要求无关，未纳入本次提交。
- `docs/AI/architecture/`、`docs/AI/reviews/` 目前仅含 `.gitkeep`，待确有对应文档时填充。