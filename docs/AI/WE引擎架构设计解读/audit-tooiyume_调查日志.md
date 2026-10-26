# 调查日志：TooiyumeRender 当前真实架构审计（task-4 / 交付物 02）

- 执行者：队友 `audit-tooiyume`；时间窗 2026-10-24 17:37 – 18:35（Asia/Shanghai）。
- 任务：共享任务 `task-4`；产出 `docs/AI/ToiyumeRender 架构演进专项约束/02_架构现状审计_audit-tooiyume.md`（888 行）与本日志。
- 只读约束：**未修改任何运行时代码**；未执行 `git checkout/restore/stash`；未构建。写入的文件仅上述两处。

## 1. 实际读过的文件与量（按「全读 / 区间读 / grep 级」标注）

### 1.1 文档

| 文件 | 行数 | 方式 |
| --- | --- | --- |
| `docs/AI/ToiyumeRender 架构演进专项约束/ToiyumeRender 架构演进专项约束.md` | 121 | 全读（6 原则 + 交付要求） |
| `docs/AI/01_Audit_2026-10-24/00_审计总报告.md` | 700 | 全读（旧结论逐条待复核清单） |
| `docs/AI/01_Audit_2026-10-24/raw/{03_RHI,04_Asset_Scene,05_Renderer_Shader,06_App_Build_Docs,07_BuildVerify}.md` | 317/369/331/574/413 | 通过总报告引用 + grep 定点核对（**未逐行全读**） |

### 1.2 源码（全读或逐行读完的区间）

| 文件 | 行数 | 方式 |
| --- | --- | --- |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp`（HEAD） | 638 | **全读**（六段 STUB 与三真实 pass 逐行确认） |
| `Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp` | 413 | **全读** |
| `Engine/01_Application/Display.cpp` | 267 | **全读**（含 Present/PreparePresentSDR/Resize 全段） |
| `Engine/00_Core/Utility/AssetRoot.cpp` / `.h` | 228 / 45 | **全读** |
| `Engine/04_Renderer/EnvironmentLightingManager.cpp` | 389 | 区间读：`:1-70`、`:118-170`、`:325-360`、`:370-389` |
| `Engine/04_Renderer/EnvironmentLightingManager.h` | 135 | **全读** |
| `Engine/05_Scene/Model/ModelLoader.cpp` | 377 | 区间读：`:80-264`；其余靠 `git diff` |
| `Engine/05_Scene/Camera/Camera.cpp` | 114 | **全读** |
| `Engine/02_RHI/Pipeline/GraphicsCommon.cpp` | 186 | 区间读：`:88-170` |
| `Engine/02_RHI/Pipeline/SamplerManager.cpp` | 39 | **全读** |
| `Engine/02_RHI/Descriptor/DescriptorHeap.cpp` | 约 130 | 区间读：`:14-62` |
| `Engine/02_RHI/Command/CommandContext.cpp` | 约 640 | 区间读：`:378-470`；其余为 grep 定点 |
| `Engine/02_RHI/Command/CommandListManager.cpp` | 约 200 | grep 定点 + 关键 40 行 |
| `Engine/01_Application/GameCore.cpp` | WT 179 / HEAD 160 | 区间读 + `git diff` 全文 |
| `Engine/00_Core/Utility/Utility.h` | WT 198 / HEAD 150 | 区间读 + `git diff` 全文（RELEASE/ASSERT 宏） |
| `Engine/00_Core/Utility/CommandLineArg.cpp` / `.h` | WT 106/16、HEAD 75/11 | 区间读 + `git diff` |
| `Demos/DeferredApp/DeferredApp.cpp` | HEAD 143 / WT 148 | **全读** |
| `Demos/ForwardApp/Tooiyume.cpp` | HEAD 145 / WT 157 | **全读** |
| `Demos/RayTracingApp/RayTracingApp.cpp` | HEAD 143 / WT 148 | **全读** |
| `Engine/Shader/02_Deferred/StaticMeshVS.hlsl` | 61 | **全读** |
| `Engine/Shader/02_Deferred/LightingPS.hlsl` | 162 | 区间读：`:85-160` |
| `Engine/Shader/02_Deferred/GBufferPS.hlsl` | 约 130 | 区间读：`:82-124`（MainPS 全函数） |
| `Engine/Shader/02_Deferred/FullScreenVS.hlsl` | 15 | **全读** |
| `Engine/Shader/02_Deferred/DeferredCommon.hlsli` | 145 | 区间读：`:56-129`（BRDF 全套） |
| `Engine/Shader/06_Environment/SkyboxVS.hlsl` / `SkyboxPS.hlsl` | 24 / 40 | 区间读（`:19-21` / `:22-37`） |
| `Engine/Shader/01_Default/DefaultPS.hlsl` | 58 | 区间读：`:1-60`（覆盖全文件） |
| `Engine/Shader/01_Default/DefaultVS.hlsl` | 71 | grep（输入/输出结构 + NO_TANGENT_FRAME 分支） |
| `Engine/03_AssetSystem/Importers/glTF.h` | 约 170 | 区间读：`:118-165` |
| `Engine/03_AssetSystem/Importers/Model/ModelConvert.cpp` | 约 460 | 区间读：`:120-145`、`:268-295`、`:338-355` |
| `Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp` | 约 110 | **全读** |

### 1.3 头文件（全读，用于职责边界取证）

`Engine/03_AssetSystem/Assets/Constants/ConstantBuffers.h`(66)、`Engine/04_Renderer/Renderer/RendererConfig.h`(32)、`Engine/05_Scene/Camera/Camera.h`(170)、`Engine/05_Scene/Camera/ShadowCamera.h`+`.cpp`(28/11)、`Engine/05_Scene/Camera/CameraController.h`(106)、`Engine/04_Renderer/Renderer/Base/RendererRegistry.h`+`.cpp`(59/22)、`Engine/04_Renderer/Renderer/Base/BaseRenderer.h`(215)、`Engine/04_Renderer/BufferManager.h`+`.cpp`(49/79)、`Engine/04_Renderer/TemporalEffects.h`(39)、`Engine/04_Renderer/Material/Material.h`(48)、`Engine/04_Renderer/Pipeline/PipelineStateCache.h`+`.cpp`(22/33)、`Engine/04_Renderer/Renderer/Forward/ForwardRenderer.h`(约 100)、`Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.h`(约 150)、`Engine/05_Scene/Model/Model.h`(141)、`Engine/05_Scene/Lighting/LightingSystem.h`+`.cpp`(69/80)、`Engine/03_AssetSystem/Importers/Texture/TextureManager.h`(74)、`Engine/02_RHI/GraphicsCore.h`(42)、`Engine/02_RHI/Resource/GpuResource.h`(59)、`ColorBuffer.h`(114)、`DepthBuffer.h`(56)、`GpuBuffer.h`(142)、`PixelBuffer.h`(44)、`Heap/UploadBuffer.h`(22)、`Heap/ReadbackBuffer.h`(23)、`Command/CommandListManager.h`(111)、`Command/CommandContext.h`(约 330，重点 `:100-215`)、`Descriptor/DescriptorHeap.h`(133)、`Pipeline/RootSignature.h`(170)、`Pipeline/PipelineState.h`(93)、`Engine/01_Application/Display.h`(45)、`Engine/01_Application/GameCore.h`(43)。

### 1.4 只做 ls / wc -l / grep 级核查的（未读内容）

- 空文件与空目录：`Engine/04_Renderer/Shader/ShaderLibrary.h`+`.cpp`、`Pipeline/PSOBuilder.h`+`.cpp`、`Material/MaterialFlags.h`（全部 0 行）、`Engine/04_Renderer/RenderGraph/`、`Engine/Shader/03_Shadow/`、`Engine/Shader/Generated/`（空目录）。
- `Engine/Shader/` 全目录清单（01_Default / 02_Deferred / 03_Shadow / 04_PostProcess / 05_BRDF / 06_Environment / Generated）。
- `Engine/02_RHI`、`Engine/04_Renderer` 目录清单；最高编号目录为 `Engine/20_cmake`。

## 2. 用过的命令（全部只读）

```
git rev-parse HEAD; git log --oneline -5; git stash list
git status --porcelain=v1; git diff --stat; git diff --cached --stat; git diff --numstat
git diff -- <file>            # DeferredRenderer / ForwardRenderer / Display / GameCore / Utility / CommandLineArg / ModelLoader / ShaderCompiler / CMakeLists
git show HEAD:<file> | sed -n <A>,<B>p
git grep -n "D:/CS-Self-Study" HEAD -- Demos Engine      # 恰好 18 处
grep -rn "<D3D12_RESOURCE_STATE_ 等 8 个模式>" Engine      # 泄漏计数与行号
grep -rIl "TAACS" Engine Demos; grep -rn "OnResize" --include=*.cpp .
grep -n "ResizeDisplayDependentBuffers|RELEASE|ASSERT|FlushResourceBarriers|InsertUAVBarrier" <files>
for f in <paths>; do echo "[$(wc -l < $f)] $f"; done   # 空文件核查
ls Engine/Shader/*/ ; ls Engine/02_RHI/*/ ; ls docs/AI docs/AIWorkLogs
stat -c "%y %s" <files>; md5sum <files>               # 工作树漂移取证
```

## 3. 哪些结论是「实测」、哪些是「推断」

**实测（可重放）**
1. 全部 `file:line` 引用；六段 STUB 的实体（只有一对 PIX 事件、零 GPU 命令）；三真实 pass 的行号与内容。
2. git 计数：HEAD 绝对路径 18 处；未暂存 28 项（455+/548-）；索引删除 1533 行；lead 复测合计 38 项。
3. grep 计数：`D3D12_RESOURCE_STATE_*` 42 处（分布 11/11/7/4/3/2/1/1）；`D3D12_INPUT_ELEMENT_DESC` 代码处 5 处（另 1 处是注释）；`D3D12_CPU/GPU_DESCRIPTOR_HANDLE` 15 处；根参数构造约 40 处。
4. 常量与枚举：`ConstantBuffers.h:18-26`、`glTF.h:138`、`DefaultPS.hlsl:44-48`（三套顺序）；`GraphicsCommon.cpp:91-104`；`m_PrefilterMipCount=5` 对 `LightingPS.hlsl:143` 的 8.0。
5. 空文件/空目录、`ShaderLibrary` 零引用、`TAACS` 唯一提及在 `TemporalEffects.cpp`。
6. 断链取证：`Display.cpp:167` 注释；`ResizeDisplayDependentBuffers` 无实现；`OnResize` 仅两处定义零调用；`Camera::SetAspectRatio` 零调用。
7. 同步取证：`CommandContext` 四类 barrier 的 flush 判定（Transition/Begin 有 `==16`，UAV/Alias 没有）；`CommandListManager` 用 `std::max` 回写；`SamplerManager` 从不 insert；`DescriptorAllocator` 递增路径无锁。
8. 工作树漂移：17:37 `DeferredRenderer.cpp` = 1007 B / 30 行；17:48:05 = 655 行。

**推断（未运行验证；报告已就地标注）**
1. 三 Demo 重复比例约 85%（逐段人工比对 + 行数统计；未做 token 级 diff）。
2. 各类画面后果：尺寸不匹配的 CopyBuffer 报错、缺 sRGB 编码导致的色调偏暗、天空与物体响应曲线不一致、法线/AO/metallic 错配的具体表现、拉伸。
3. 严重度排序与「最小修复方向」的可行性（本阶段不改代码，未验证）。
4. `MeshSorter` 跨帧不 Reset 的实际错绘风险（只确认了「Forward Reset、Deferred 不 Reset」这一事实差异）。
5. `SamplerManager` 去重失效导致的描述符泄漏量级（只确认了「从不 insert」这一事实）。
6. `AssetRoot` 在根未解析时的具体退化表现（代码路径已读，未运行）。

## 4. 剩余未读清单（与报告 §9 一致）

1. `Engine/03_AssetSystem/Importers/Mesh/MeshConvert.cpp`（顶点布局、3 处 `D3D12_INPUT_ELEMENT_DESC` 行号）。
2. `Engine/Shader/02_Deferred/GBufferPS.hlsl:1-88`（VSOutput/常量/GetNormalWS/EncodeOctNormal）。
3. `Engine/Shader/01_Default/DefaultVS.hlsl:44-71`（主函数体）。
4. `Engine/04_Renderer/EnvironmentLightingManager.cpp:30-120`（CreateSkyboxPipeline 深度状态）+ `Engine/Shader/06_Environment/EnvironmentCommon.hlsli`。
5. `Engine/02_RHI/Memory/LinearAllocator.cpp`、`Command/CommandAllocatorPool.cpp`、`Descriptor/DynamicDescriptorHeap.cpp`。
6. `Engine/02_RHI/Command/CommandContext.h:195-330`（ContextManager/上下文池化）。
7. `Engine/03_AssetSystem/Importers/Texture/TextureConvert.cpp`、`DDSTextureLoader.cpp` 全文。
8. `Engine/03_AssetSystem/Importers/glTF.cpp` 全文（`:622-755` 主链、JOINTS/WEIGHTS/COLOR_0、normalTexture.scale）。
9. `Engine/02_RHI/Resource/Texture.h`+`.cpp`、`ShadowBuffer.h`+`.cpp`、`Command/CommandSignature.h`+`.cpp`。
10. `Engine/CMakeLists.txt`（WT 221 行）全文、根 `CMakeLists.txt`、`vcpkg.json`、`CMakePresets.json`、`Engine/20_cmake/*.cmake`、`scripts/*.bat`。
11. `Engine/01_Application/GameInput.cpp`（WT +83/-26）、`Engine/05_Scene/**` 其余未触及文件。
12. `Assets/**` 是否还有其他字节码/编译产物（只核对了 `git ls-files Assets` 的 3 个文件与目录 211 个文件）。

## 5. 踩到的坑（供后续会话避让）

1. **任务前提已过期**：「17 文件 / 34 insertions / 1373 deletions」我在 17:37 就对不上（我采集到 13 项改动 + 10 项删除；18:0x 变为未暂存 28 项 455+/548- 加索引删除 3 项 1533-；lead 18:17:49 复测 38 项）。任何以「工作树当前状态」为前提的结论都必须带时间戳。
2. **工作树被另一个并行开发会话持续写入**（lead 已核实）：mtime 从 17:22 推到 18:11，我审计期间有文件行数变化两次。故只用 HEAD 作事实基线，工作树只作风险记录。
3. **截断态真实出现过**：`DeferredRenderer.cpp` 17:37 实测 1007 B / 30 行，17:48:05 被对方恢复为 655 行。涉及该文件的结论必须用 `git show HEAD:` 或恢复后版本。
4. **旧审计有多处需修正**（报告 §2.5 列出 5 条）：fence 回写有 `std::max` 保护；barrier 越界只在 UAV/Alias 路径；`BakeEnvironmentTextures` 并非每帧重烘（`m_Baked` 守卫）；Display 里不存在名为 `OnResize` 的成员；`MeshConvert.cpp` 真实路径在 `Importers/Mesh/` 下。
5. **`compress` 工具两个硬限制**：① 单个 range 的摘要超过 20000 字符会导致整批压缩被拒（实测 21660 被拒，报 Summary too long）→ 大段必须拆块；② 摘要字符串放在 JS 模板字面量里时不能出现未转义的「美元加大括号」形式（我写 `DEPENDS` 加该形式时触发 ReferenceError: SHADER_SOURCE is not defined）→ 需转义或改用单引号字符串。
6. **`run_code` 拼接长 Markdown 的写法**：用双引号字符串加大量反斜杠转义会触发 TS 解析错误（Unexpected character 或 Expected 逗号）。**稳定做法：单引号字符串数组加 join**，且正文里不要出现 ASCII 单引号（我第一次写日志时在正文里带了两个 ASCII 单引号包住的字元，直接把字符串截断，导致 lexing error）。向已有长文件追加内容时，先写入标记行 `<!--MORE-->`，再用 edit 替换该标记追加。
7. **grep 路径假设错误**：`Engine/03_AssetSystem/Importers/Model/MeshConvert.cpp` 不存在（实际在 `Importers/Mesh/`；`Importers/` 下是 `Mesh/ Model/ Texture/ glTF.cpp glTF.h json.hpp`）。
8. **`git grep` 反斜杠路径转义**：`C:\...` 在 shell 与正则两层转义下容易漏匹配；我最终只按正斜杠形式统计到 18 处，**未穷举反斜杠形式**（报告 §3.4.3 已标注口径）。
9. **命名冲突避险**：`docs/AIWorkLogs/` 已有队友的 01A 与 01C 日志，`docs/AI/ToiyumeRender 架构演进专项约束/` 已有 00/01A/01C/04/06 文档 → 我用 `02_` 前缀，未触碰他人文件。
10. **路径含空格与中文**：`wc -l`、`git diff --` 必须给路径加引号，否则被当成多个参数。
11. **上下文预算**：lead 提醒后改为「批量 `grep -n` + `sed -n` 区间 + 一次 `Promise.all` 并行取证」，并对已消费大段做压缩；但压缩本身也消耗步骤（见坑 5/6）→ **先落盘关键结论再压缩**是更稳的顺序。

## 6. 交付与状态

- 报告：`docs/AI/ToiyumeRender 架构演进专项约束/02_架构现状审计_audit-tooiyume.md`（888 行；§0 速览 / §1 基线与工作树 P0 / §2 旧审计复核表 / §3 分层架构与依赖 / §4 RHI 边界与泄漏逐处 / §5 后处理与 HDR-Tonemap-LDR / §6 Resize 与 GPU 同步 / §7 六原则评分表 / §8 债务清单 30 条 / §9 未确认项 18 条 / 附录 A 方法）。
- 本日志：`docs/AIWorkLogs/audit-tooiyume_调查日志.md`。
- 未做（按约束）：未修改运行时代码、未 checkout/restore/stash、未构建、未运行程序。
- 待 lead 决策：D-01（并行会话与基线固定）、U-06/U-16（需运行与构建才能确认的两类结论）。
