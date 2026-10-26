# 06 App / Build / Assets / Docs 审计

审计范围: `Demos/`(三个入口) + 构建系统(CMake/CMakePresets/vcpkg/ShaderCompiler) + `Assets/` + 全部文档 + DXR 现状。
审计时间: 2026-10-24。审计方式: 只读源码/配置/构建副本,未修改任何源码,未做 git 操作,未启动 exe。
结论分级: DONE / PARTIAL / ORPHAN / STUB / MISSING / BUG / UNKNOWN。每条结论附 `文件:行号` 证据。
未验证的项目显式标注 UNKNOWN,不臆测。

---

## 0. 结论速览

| # | 结论 | 影响 |
| --- | --- | --- |
| 1 | 三个 Demo 是同一份代码的 3 份拷贝,Deferred 与 RayTracing **96/100 行完全相同** | 三处维护,必然漂移 |
| 2 | RayTracingApp **不是 DXR**,是 DeferredApp 换模型;`RequiresRaytracingSupport()` 签名错误导致 DXR 请求被静默丢弃 | DXR 从零开始 |
| 3 | **`Assets/Model` 目录根本不存在**,且被 `.gitignore:386` 忽略;仓库里 0 个 gltf/glb/ty | 现在任何人 clone 下来都渲染不出东西 |
| 4 | 加载模型用**硬编码绝对路径** `D:/CS-Self-Study/...`,共 18 处 | 换机器必失败 |
| 5 | `EnvironmentLightingManager::BakeEnvironmentTextures` 因 `m_HDRLoaded` 永不置位而实际不可达? —— 见 §5.3,**需实测** | IBL 管线能否跑起来存疑 |
| 6 | Forward 路径用的是**入库的 2026-05-04 的旧 .h 字节码**,改 .hlsl 完全无效 | 隐性坑 |
| 7 | `CMakeSettings.json` 是**非法 JSON 片段**(无外层大括号) | 文件本身是坏的 |
| 8 | README 链接的 `docs/Architecture.md` **不存在**;各模块 md 用的是早已废弃的编号 | 文档失效 |
| 9 | vcpkg **不含 imgui**,加 imgui 需改 3 个文件 + 1 个构建宏 | §7 给清单 |
| 10 | 模型加载失败**不崩溃**,静默 `nullptr` → 黑屏 | 调试体验差 |

---

## 1. 三个 Demo 的差异与重复度

### 1.1 规模与重复度(实测统计)

用去空行后的行集合做比较:

| 文件 | 非空行数 |
| --- | --- |
| `Demos/ForwardApp/Tooiyume.cpp` | 103 |
| `Demos/DeferredApp/DeferredApp.cpp` | 100 |
| `Demos/RayTracingApp/RayTracingApp.cpp` | 101 |

| 比较对 | 相同行数 | 占比 |
| --- | --- | --- |
| 三者共有(逐行多重集计数) | **72** | 70% 的 Forward 行在另两份中都出现 |
| Deferred vs RayTracing(按行序对齐 LCS) | **96 / 100** | **96%** |
| Forward vs Deferred(按行序对齐 LCS) | **72 / 103** | **70%** |

统计口径:
- "三者共有"用 `collections.Counter` 做多重集交集(与顺序无关);
- "LCS"用最长公共子序列(保序),故 Forward vs Deferred 的 LCS(72)与三者共有(72)数值一致而含义不同,已在文中分别标注。
- 原始脚本:`_tmp_diff.py`(审计临时脚本,已删除,不影响仓库)。

### 1.2 精确 diff(实际执行 `diff`)

DeferredApp → RayTracingApp 的全部差异只有 4 处:

    Demos/RayTracingApp/RayTracingApp.cpp:34   virtual bool RequiresRaytracingSupport();      // 仅 RT 有
    Demos/RayTracingApp/RayTracingApp.cpp:64   bool Tooiyume::RequiresRaytracingSupport() { return true; }
    Demos/DeferredApp/DeferredApp.cpp:62,64    多两个未使用变量 leimiaier / merlin
    Demos/RayTracingApp/RayTracingApp.cpp:78,79 加载 saberCaster3NoArmor / ChosenSword(Deferred 用 merlin / leimiaier)

ForwardApp → DeferredApp 差异集中在:渲染器类型(Forward vs Deferred)、`Display::SceneColorBuffer` 赋值源、`SetZRange`、以及 Forward 版本在 `Update/RenderScene` 里**手动遍历 Models** 而 Deferred 版本填 `RenderFrameDesc.Models`。

### 1.3 各 Demo 实际做了什么

| 环节 | ForwardApp | DeferredApp | RayTracingApp |
| --- | --- | --- | --- |
| 渲染器 | `Renderer::Forward::ForwardRenderer`(`Tooiyume.cpp:51`) | `Renderer::Deferred::DeferredRenderer`(`DeferredApp.cpp:53`) | **同为 DeferredRenderer**(`RayTracingApp.cpp:55`) |
| 初始化 | `ForwardRenderer.Initialize(desc)`(`Tooiyume.cpp:68`) | `DeferredRenderer.Initialize(desc)`(`DeferredApp.cpp:73`) | `DeferredRenderer.Initialize(desc)`(`RayTracingApp.cpp:73`) |
| Present 源 | `GetForwardBuffer().SceneColorBuffer[0]`(`Tooiyume.cpp:69`) | `GetDeferredBuffer().SceneColorBuffer`(`DeferredApp.cpp:74`) | 同 Deferred(`RayTracingApp.cpp:74`) |
| 模型 | saberCaster3NoArmor + ChosenSword | merlin + leimiaier | saberCaster3NoArmor + ChosenSword |
| 相机 ZRange | 1.0 ~ 10000.0(`Tooiyume.cpp:79`) | 0.1 ~ 100.0(`DeferredApp.cpp:84`) | 0.1 ~ 100.0(`RayTracingApp.cpp:84`) |
| DrawPass | `kOpaque`(`Tooiyume.cpp:123`) | `kOpaque`(`DeferredApp.cpp:121`) | `kOpaque`(`RayTracingApp.cpp:121`) |

三个 Demo 的 `Startup/Cleanup/Update/RenderScene/wWinMain` 结构完全同构,`Cleanup` 里三份都清到 `Models[2]`(数组是 `Models[10]`,见 `Tooiyume.cpp:40` / `DeferredApp.cpp:42` / `RayTracingApp.cpp:44`),但只加载了 `Models[0]`、`Models[1]` 两个:`Tooiyume.cpp:137-139`、`DeferredApp.cpp:132-134`、`RayTracingApp.cpp:132-134`。**清第 3 个槽位属于复制粘贴残留(不越界,数组长度 10,故不是内存错误,只是逻辑冗余)**。

### 1.4 可合并性判断

**可合并,且成本低**。建议合并为单一入口 + 命令行/配置选择渲染器:

- 三个文件共 72~96 行重复(见 §1.1 实测统计);`wWinMain`(`Tooiyume.cpp:46-49` / `DeferredApp.cpp:48-51` / `RayTracingApp.cpp:50-53`)完全一样;
- 差异点只有:渲染器对象类型、`GetXxxBuffer()` 取值方式、`Update` 里是否手工遍历 Models;
- 引擎已经提供了 `RendererRegistry`,两侧**一半接了一半没接**:
  ```
  注册侧(已接):
    Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:33  RendererAutoRegister<DeferredRenderer> s_RegisterForwardRenderer(L"DeferredRenderer");
    Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp:27    RendererAutoRegister<ForwardRenderer> s_RegisterForwardRenderer(L"ForwardRenderer ");
  查找侧(未接):
    grep "CreateRenderer" → 只有 RendererRegistry.h:38 声明 + RendererRegistry.cpp:16 定义,**0 个调用点**
    三个 Demo 全是全局对象直接构造(Tooiyume.cpp:51 / DeferredApp.cpp:53 / RayTracingApp.cpp:55)
  ```
  即:注册表被静态注册填充了,但**从未被查询** → 查找侧 **ORPHAN**。
- 顺带一个真 BUG:`ForwardRenderer.cpp:27` 注册名是 `L"ForwardRenderer "`,**结尾多一个空格**;将来用 `CreateRenderer(L"ForwardRenderer")` 查表会返回 `nullptr`(`RendererRegistry.cpp:18-19` 的 `if (it == m_Creators.end()) return nullptr;`)。

- 合并方案(最小改动):统一用一个 `AppBase` 持有 `RendererPtr`,通过 `IGameApp::RequiresRaytracingSupport` 或命令行选择策略;把 `Models[N]` 的脏标记遍历下沉到基类,消除 Forward 版手写循环(`Tooiyume.cpp:93-104`)与 Deferred 版 `RenderFrameDesc.Models`(`DeferredApp.cpp:96-98`)的重复。

---

## 2. RayTracingApp 到底是什么(逐行判定)

**结论:纯光栅化,不是 DXR。是一个"举着坏掉的 DXR 旗子"的 DeferredApp 副本。**

### 2.1 DXR 旗子为什么是坏的(BUG,推理链完整)

    Engine/01_Application/GameCore.h:26     virtual bool RequiresRaytracingSupport() const { return false; }
    Demos/RayTracingApp/RayTracingApp.cpp:34   virtual bool RequiresRaytracingSupport();        // 缺 const
    Demos/RayTracingApp/RayTracingApp.cpp:64   bool Tooiyume::RequiresRaytracingSupport() { return true; }
    Engine/01_Application/GameCore.cpp:29      Graphics::Initialize(game.RequiresRaytracingSupport());
    Engine/02_RHI/GraphicsCore.h:20            void Initialize(bool RequireDXRsupport = false);
    Engine/02_RHI/GraphicsCore.cpp:171         if (RequireDXRSupport == true)

推理:

1. 派生类声明少了 `const`,与基类虚函数**签名不同** → 构成重载,**不是覆盖**;且未写 `override`,编译器无从检查。
2. `GameCore.cpp:29` 里 `game` 静态类型是 `IGameApp&`,可见的只有基类 `... () const`,**永远命中基类版,恒返回 false**。
3. 因此 `GraphicsCore.cpp:171` 的 `if` 永不进入,`g_bRaytracingSupported`(`GraphicsCore.cpp:40`)恒 false,`g_DeviceDXR` 永不 `QueryInterface`。
4. 反向验证手段:给 `RayTracingApp.cpp:34/64` 加 `const` 与 `override`,若签名仍不匹配会立即编译失败。

补充证据:全仓库 `g_bRaytracingSupported` 只有 `GraphicsCore.cpp:40/179/197/204` 四处(声明 + 3 处赋值),**没有任何读取者**;且它没有 `extern` 声明在 `GraphicsCore.h`(该文件对外只暴露 `g_Device`/`g_DXGIFactory` 等,`GraphicsCore.h:26-34`,以及 `GraphicsCore.h:20` 的 `Initialize`)。

### 2.2 渲染路径判定:没有一行 RT

    全仓库 grep DispatchRays / AccelerationStructure / D3D12_BUILD_RAYTRACING / lib_6_ / ID3D12Device5 使用点:
      Engine/02_RHI/GraphicsCore.cpp:39          仅 ComPtr<ID3D12Device5> g_DeviceDXR 声明
      Engine/02_RHI/GraphicsCore.cpp:183         仅 QueryInterface 赋值
      Engine/02_RHI/Command/CommandContext.h:189 ComPtr<ID3D12GraphicsCommandList4> m_CommandList(类型而已,未调 RT 方法)
      → 除以上 3 处,Engine/Demos/ThirdParty 全部 0 命中 DispatchRays

- `RayTracingApp.cpp:9` include 的是 `04_Renderer/Renderer/Deferred/DeferredRenderer.h`;
- `RayTracingApp.cpp:55` 是 `Renderer::Deferred::DeferredRenderer DeferredRenderer;`;
- `RayTracingApp.cpp:73/121/123` 走 `Initialize / Render(kOpaque) / EndFrame`,与 `DeferredApp.cpp:73/121/123` 一字不差。

**状态:ORPHAN + BUG**。RayTracingApp 存在,但既没有 DXR 设备,也没有 RTPSO/SBT/TLAS/BLAS,更没有 RayTracing 渲染器目录(`Engine/04_Renderer/Renderer/` 下只有 `Base/ Deferred/ Forward/ RendererConfig.h`,`find` 实测)。

### 2.3 对 DXR 实施的直接含义

- `ID3D12Device5` 已预留(`GraphicsCore.cpp:39`)但拿不到实例 → 第一步是修 §2.1 的签名;
- `CommandContext` 的命令列表**已是 `ID3D12GraphicsCommandList4`**(`CommandContext.h:189`),`CommandListManager::CreateNewCommandList` 用 `riid + TY_IID_PPV_ARGS` 按实际类型创建(`CommandListManager.cpp:102`),**这一点已经满足 DXR 要求,不用改**;
- 缺的是:RTPSO、SBT、AS 构建、`DispatchRays` 封装、以及 `lib_6_x` 的 shader 编译分支(`ShaderCompiler.cmake` 只支持 `vs/ps/cs_6_4`,见 §6)。

---

## 3. 硬编码绝对路径全清单(grep 全仓库,排除 out/ 与 vcpkg_installed/)

### 3.1 源码内的绝对路径(18 处,全部指向 `D:/CS-Self-Study/...`)

| 文件:行 | 内容 |
| --- | --- |
| `Demos/ForwardApp/Tooiyume.cpp:53` | `Assets/Model/Saber/saber_emiya.glb` |
| `Demos/ForwardApp/Tooiyume.cpp:54` | `Assets/Model/toneriko/toneriko1.gltf` |
| `Demos/ForwardApp/Tooiyume.cpp:55` | `Assets/Model/saberCaster/saberCaster3NoArmor.gltf` |
| `Demos/ForwardApp/Tooiyume.cpp:56` | `Assets/Model/saberCaster/ChosenSword.gltf` |
| `Demos/ForwardApp/Tooiyume.cpp:58` | `Assets/Model/Kyomachiya/KyomachiyaA.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:55` | `Assets/Model/Saber/saber_emiya.glb` |
| `Demos/DeferredApp/DeferredApp.cpp:56` | `Assets/Model/toneriko/toneriko1.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:57` | `Assets/Model/saberCaster/saberCaster3NoArmor.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:58` | `Assets/Model/saberCaster/ChosenSword.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:60` | `Assets/Model/Kyomachiya/KyomachiyaA.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:62` | `Assets/Model/leimiaier/leimiaierALL.gltf` |
| `Demos/DeferredApp/DeferredApp.cpp:64` | `Assets/Model/Merlin/merlin.gltf` |
| `Demos/RayTracingApp/RayTracingApp.cpp:57` | `Assets/Model/Saber/saber_emiya.glb` |
| `Demos/RayTracingApp/RayTracingApp.cpp:58` | `Assets/Model/toneriko/toneriko1.gltf` |
| `Demos/RayTracingApp/RayTracingApp.cpp:59` | `Assets/Model/saberCaster/saberCaster3NoArmor.gltf` |
| `Demos/RayTracingApp/RayTracingApp.cpp:60` | `Assets/Model/saberCaster/ChosenSword.gltf` |
| `Demos/RayTracingApp/RayTracingApp.cpp:62` | `Assets/Model/Kyomachiya/KyomachiyaA.gltf` |
| `Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:40` | `Assets/Skybox/grasslands_sunset_2k.dds` |

注:每份 Demo 都声明了 5~7 个路径变量,但**实际只用到 2 个**(`Tooiyume.cpp:73-74`、`DeferredApp.cpp:78-79`、`RayTracingApp.cpp:78-79`),其余是死变量(编译器不报未使用因为它们是全局变量)。

### 3.2 构建系统内的绝对路径

| 文件:行 | 内容 | 该路径是否存在 |
| --- | --- | --- |
| `CMakePresets.json:16` | `CMAKE_TOOLCHAIN_FILE = F:/VS/rep/vcpkg/scripts/buildsystems/vcpkg.cmake` | **存在**(48552 B,实测 `ls`) |
| `CMakeSettings.json:4` | 同上 | 存在 |
| `Engine/20_cmake/ShaderCompiler.cmake:9` | `"${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/tools/directx-dxc"` | 是相对 `CMAKE_SOURCE_DIR` 的路径,但**硬编码了 triplet `x64-windows`**,换 triplet/换 vcpkg 根目录即失效 |

- `CMakePresets.json` 的 toolchain 指向 `F:/VS/rep/vcpkg`,**换机必断**;应改为 `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` 或环境变量注入。
- `ShaderCompiler.cmake:9` 查 dxc 用 `find_program(... PATHS ".../vcpkg_installed/x64-windows/tools/directx-dxc" REQUIRED)`,只覆盖 `x64-windows`;若改用 `x64-windows-static` 或 manifest 安装到别处,配置阶段直接 `REQUIRED` 失败。实测该目录存在且含 `dxc.exe / dxcompiler.dll / dxil.dll`。

### 3.3 `CMakeSettings.json` 是非法 JSON(BUG)

    CMakeSettings.json 全文 215 字节,以 ""variables": [" 开头,末尾 "]" —— 没有外层 { }。
    实测: python -c "json.load(...)" → JSONDecodeError: Extra data: line 1 column 12

这个文件本来是 VS 的 `CMakeSettings.json`,但**缺了 `{"configurations": [...]}` 外壳**,任何工具解析都会失败。当前工程实际靠 `CMakePresets.json`(`json.load` 通过,presets: windows-base/x64-debug/x64-release/x86-debug/x86-release)驱动,所以没炸,但该文件应当删除或修好。

---

## 4. Assets 现状(关键阻塞项)

### 4.1 实际清单(递归 `find`,全仓库)

    Assets/ 仅 3 个文件:
      Assets/Skybox/grasslands_sunset_2k.dds   22369752 B
      Assets/Skybox/grasslands_sunset_2k.hdr    6585113 B
      Assets/Skybox/新建文本文档.txt                  0 B(空文件,垃圾)

    按扩展名: txt 1, hdr 1, dds 1
    **gltf / glb / bin / ktx / png 数量: 0**
    **.ty(引擎自有烘焙格式): 0**

### 4.2 `Assets/Model` 目录根本不存在(MISSING)

    ls -d Assets/Model  → No such file or directory

而三个 Demo 全部从 `Assets/Model/...` 加载模型(§3.1)。`.gitignore:386` 明确写了 `/Assets/Model`,且 `.gitignore:382-385` 的注释解释了原因("模型和贴图文件太大…建议不要直接传 Git"):

    .gitignore:382-386
      # 外部资源目录 (可选)
      # 如果你的模型和贴图文件太大（比如好几个GB），建议不要直接传 Git
      # 而是放在一个特定的 Assets/ 文件夹并使用 Git LFS 管理
      # 或者暂时忽略它们，在 README 中提供下载链接
      /Assets/Model

**后果:任何一次干净 clone 都无法验证渲染。** `README.md` 里**也没有**提供模型下载链接(`README.md:1-20` 全文只有两段,见 §8.1)。

### 4.3 git 跟踪状态(实测 `git ls-files`)

- `Assets/` 被跟踪的只有 3 个文件:`grasslands_sunset_2k.dds / .hdr / 新建文本文档.txt`;
- `.gitattributes` 有 63 行,第 4 行 `* text=auto`,`grep -c "filter=lfs" → 0`,**没有配置 Git LFS**。注释里提到的"使用 Git LFS"并未落地。
- 结论:**"大资源不入库"的意图已有,但替代方案(下载脚本 / LFS / README 链接)一个都没有 → 资产链是断的。**

### 4.4 Skybox DDS 是合法 DDS(实测二进制头)

    偏移 0:  "DDS " magic 正确
    偏移 4:  dwSize=124
    flags=0x2100F, height=1024, width=2048, pitchOrLinearSize=16384, mipMapCount=12
    ddspf: size=32, flags=0x4 (DDPF_FOURCC), fourCC="q\0\0\0"

- 2048×1024 的 equirectangular HDR,12 级 mip,**格式是 FourCC 编码且 fourCC 非 DX10(`DX10`)** —— 从四字节 `71 00 00 00` 看不是标准 ASCII 四字符码,DDSTextureLoader 会走 legacy FourCC 分支。`LoadDDSFromFile` 失败时会**回退到 kMagenta2D 品红占位纹理**(`TextureManager.cpp:239`,以及 `TextureRef::GetSRV` 的 `TextureManager.cpp:236-241`),不报错、不崩溃 → 画面变品红。
- 这是"静默失败"的典型路径:着色器能跑,但看到的是品红天空。

---

## 5. 运行时资产定位与失败行为

### 5.1 路径解析链路

    Demo 硬编码绝对路径 (Tooiyume.cpp:73) 
      → Scene::Model::LoadModel(filePath, forceRebuild)         (ModelLoader.cpp:189)
      → 缺失则 nullptr 返回,不抛异常                            (ModelLoader.cpp:211-215)
      → ModelInstance::operator= 对 nullptr 有防御分支           (Model.cpp:82-93, if (sourceModel == nullptr) Destroy())
      → ModelInstance::Update 首行 if (m_Model == nullptr) return (Model.cpp:107-108)
      → GatherRenderables 也是 if (m_Model != nullptr)           (Model.cpp:47-53)

**结论:模型缺失不会崩溃,不会弹窗,只在 debug 输出里打一行 `Error: Could not find %ws`(`ModelLoader.cpp:213`),然后黑屏。** 对调试极不友好,建议改为 HALT / 断言或提供内置 fallback 模型。

### 5.2 `forceRebuild = true` 是性能地雷

三个 Demo 都写死 `bool forceRebuild = true;`(`Tooiyume.cpp:72`、`DeferredApp.cpp:77`、`RayTracingApp.cpp:77`)。`ModelLoader.cpp:217-221` 的语义是 `needBuild = forceRebuild` 优先,即**每帧/每次启动都重新从 glTF 烘焙 .ty**,缓存(file mtime 比对)形同虚设。这与 `ModelLoader.cpp:219-221` 精心写的缓存逻辑自相矛盾。

### 5.3 `m_HDRLoaded` 从不被置位(BUG,影响 IBL)

    Engine/04_Renderer/EnvironmentLightingManager.cpp:150   if (m_Baked || m_HDRLoaded || !m_Textures.HDRTexture.IsValid()) return;
    Engine/04_Renderer/EnvironmentLightingManager.h:100      bool m_HDRLoaded = false;
    全仓库 m_HDRLoaded 只有这 2 处 → 无任何赋值语句

即 `BakeEnvironmentTextures` 的守卫里那个 `m_HDRLoaded` 分支永远为 false,`LoadHDR`(`EnvironmentLightingManager.cpp:170-178`)也不会把它置 true。当前能跑只是因为 `m_Baked` 是唯一有效的短路条件,且 `HDRTexture.IsValid()` 会拦住加载失败的场景。**这是一个死字段,会让"已加载 HDR 就跳过 bake"的意图失效。**

另外两个同源问题:

- `EnvironmentLightingManager::SaveDDS()` **函数体是空的**(`EnvironmentLightingManager.cpp:122-125`),`environmentlightingmanager.txt:1,7,14` 里规划的"DDS 结果落盘/二次启动直接 LoadDDS"完全没有实现 → 每次启动都要重新烘焙 IBL(4 个 compute pass)。
- `LoadHDR` 调用 `CompileTextureOnDemand(HDRPath, kDefaultBC | kQualityBC)`(`EnvironmentLightingManager.cpp:174`),但传入的 `HDRtexture` 是 **.dds 而非 .hdr**(`DeferredRenderer.cpp:40`),`ConvertToDDS` 面对已是 dds 的输入行为 **UNKNOWN**(未实测)。

### 5.4 Texture 根路径为空字符串

    Engine/03_AssetSystem/Importers/Texture/TextureManager.cpp:53     wstring s_RootPath = L"";
    Engine/03_AssetSystem/Importers/Texture/TextureManager.cpp:58     s_RootPath = TextureLibRoot;
    两个 Demo 都传空: DeferredRenderer.cpp:85 / ForwardRenderer.cpp:60  →  TextureManager::Initialize(L"");
    最终拼接: TextureManager.cpp:97  Utility::ReadFileSync(s_RootPath + fileName)

所以纹理加载完全依赖调用方传绝对路径(`ModelLoader.cpp:62` 的 `basePath + textureNames[ti]`,basePath 来自 `GetBasePath(filePath)`)。这条链是自洽的,但**依赖 Demo 传入绝对路径**,与 §3.1 的硬编码问题耦合。

---

## 6. Shader 编译流程

### 6.1 机制(可用,PARTIAL)

    Engine/CMakeLists.txt:21-89        10 处 compile_shader(...)
    Engine/CMakeLists.txt:94-105       add_dependencies(TooiyumeEngine ... 10 个 *_Shader)
    Engine/20_cmake/ShaderCompiler.cmake:30-131
      ├ 71-92   add_custom_command: dxc -E <entry> -T <profile> -I <Engine/Shader> <flags> -Fo <out.cso>
      └ 98-111  add_custom_command: cmake -P BinToHeader.cmake  (cso → .h)
    Engine/20_cmake/BinToHeader.cmake:1-30   file(READ ... HEX) → foreach 2 字节 → "0x..,"
    生成物: out/build/<preset>/ShaderCache/<name>.h   (实测存在,首行 "// Auto Generated Shader Binary",
           符号 "unsigned char <name>_cso[] = {" )

Debug/Release 分支:`ShaderCompiler.cmake:51-65`,Debug = `-Zi -Qembed_debug -Od`,Release = `-O3`。**两者都会编译**,无"Debug 不编"的问题。

### 6.2 三个真实缺陷

**(a) DEPENDS 只写了主 shader,不追踪 .hlsli(BUG)**

    Engine/20_cmake/ShaderCompiler.cmake:87    DEPENDS ${SHADER_SOURCE}

只有主 `.hlsl`。实测反例:`Engine/Shader/02_Deferred/DeferredCommon.hlsli` mtime = 2026-07-14 20:33,而 `LightingPS.hlsl` 是 2026-10-16,`StaticMeshVS.hlsl` 是 2026-05-26 —— 修改 `DeferredCommon.hlsli` **不会触发任何 shader 重编**。`Common.hlsli`(`Engine/Shader/01_Default/Common.hlsli`,2026-05-08)同理。

**(b) `Shader/01_Default` 两个 shader 完全没纳入编译(ORPHAN)**

    Engine/CMakeLists.txt 里 grep "01_Default" → 0 命中;compile_shader 共 10 次,全在 02_Deferred / 06_Environment。
    但 ForwardRenderer.cpp:16-17 仍在 include:
      #include "Shader/01_Default/DefaultVS.h"
      #include "Shader/01_Default/DefaultPS.h"

这 2 个 `.h` 是 **2026-05-04 手工入库的旧产物**(实测 mtime:`DefaultVS.h` 2026-05-04 14:33 60167 B,`DefaultPS.h` 2026-05-04 20:06 42742 B),内部还残留 `#if 0` 里的旧调试信息:

    Engine/Shader/01_Default/DefaultVS.h:23  ; shader debug name: D:\...\ToiyumeRender\Build\CompiledShaders\Model\PDBs\DefaultVS.pdb
    (符号定义在 DefaultVS.h:341  const unsigned char g_pDefaultVS[] = { ... ),被 ForwardRenderer.cpp:302/303/369/370 直接使用

因为 `Engine/CMakeLists.txt:107-110 / 112-115 / 126-128` 的 include 路径同时含 `${CMAKE_CURRENT_SOURCE_DIR}`(= Engine)与 `${CMAKE_BINARY_DIR}/ShaderCache`,`#include "Shader/01_Default/DefaultVS.h"` 这种带相对目录的写法**只会命中源码树里的旧 .h**,不会命中生成的。**结果:Forward 渲染路径的 VS/PS 字节码自 2026-05-04 起从未被 dxc 重编,改 `DefaultVS.hlsl` / `DefaultPS.hlsl` 对 Forward 完全无效。**

**(c) 生成的头文件没有 include guard / 无 `inline`(潜在 ODR 风险)**

`BinToHeader.cmake:9-28` 直接拼 `unsigned char <name>_cso[] = {...};`(实测 `ShaderCache/SkyboxVS.h` 尾部无 `#pragma once`、符号非 `static`、非 `inline`)。当前每个头只被一个 .cpp include(实测:`SkyboxVS.h/SkyboxPS.h/IrradianceConvolutionCS.h/BRDFLUTCS.h` 只在 `EnvironmentLightingManager.cpp:19-24`;`StaticMeshVS.h/GBufferPS.h/FullScreenVS.h/LightingPS.h` 只在 `DeferredRenderer.cpp:19-22`),所以**暂时不炸**。一旦第二个 .cpp include 同一个头(比如 TAA 也要 `FullScreenVS`),立刻 LNK2005 重定义。**属于必须提前修的结构性缺陷。**

### 6.3 `compile_shader` 会在无提示下静默失败? —— 部分 UNKNOWN

- 不利证据:`add_custom_command` 未设 `${DXC_EXECUTABLE} ... RESULT_VARIABLE`,但 ninja 本身会捕获非 0 退出码并 FAILED,不会静默通过。**因此"编译失败无人知"的说法不成立**。
- 但 `.hlsl` 的**警告**(如 `DeferredCommon.hlsli:60` 的 implicit truncation)只出现在构建日志,不影响产物。
- `Engine/20_cmake/ShaderCompiler.cmake:6-11` 的 `find_program(DXC_EXECUTABLE ... REQUIRED)` 若找不到会在 configure 阶段硬失败,**不会静默**。

---

## 7. vcpkg / 依赖 与 imgui 接入清单

### 7.1 当前依赖(实测 `vcpkg.json`)

    vcpkg.json:4-15
      directxtex (features: dx12)
      directxmesh (features: dx12)
      directx-headers
      directx-dxc

根 `CMakeLists.txt:25-28` 对应 `find_package(directxtex / directxmesh / DirectX-Headers CONFIG REQUIRED)`,`CMakeLists.txt:28` 注释说明 dxc "稍后可以在 Engine 中直接链接 dxcompiler",而 `Engine/CMakeLists.txt:150` 确实 `target_link_libraries(... dxcompiler)`。

**imgui:确认不存在**(vcpkg.json 4 个依赖里没有;全仓库 `grep -i imgui` 于 Engine/Demos 源码 0 命中)。

### 7.2 增加 imgui 的完整改动清单

| # | 文件 | 改什么 |
| --- | --- | --- |
| 1 | `vcpkg.json` | dependencies 增加 `{"name":"imgui","features":["dx12-binding","win32-binding"]}`(feature 名实测来自 vcpkg port:`ports/imgui/vcpkg.json` 的 `dx12-binding` / `win32-binding`) |
| 2 | 根 `CMakeLists.txt` | `find_package(imgui CONFIG REQUIRED)`(与 `CMakeLists.txt:25-27` 同风格) |
| 3 | `Engine/CMakeLists.txt` | `target_link_libraries(TooiyumeEngine PUBLIC imgui::imgui)`(`Engine/CMakeLists.txt:145-154` 那段) |
| 4 | 新文件 | `Engine/01_Application/ImGuiLayer.{h,cpp}`:初始化 `ImGui::CreateContext`,`ImGui_ImplWin32_Init(GameCore::g_hWnd)`,`ImGui_ImplDX12_Init(...)` |
| 5 | `Engine/01_Application/Display.h:18-24` | 需要暴露 **back buffer 的 RTV 与帧索引**给 ImGui。当前 `Display.h` **只暴露 `SceneColorBuffer`**,`g_DisplayPlane[] / g_CurrentBuffer` 是 `Display.cpp:131-132` 的**匿名命名空间全局**,外部拿不到 → **必须先加一个 `GetCurrentBackBuffer()` 接口** |

另外注意:

- `Engine/01_Application/GameCore.h:24` 已经有 `virtual void RenderUI(GraphicsContext&) {}` 钩子,但**全仓库无任何调用者**(实测 `grep RenderUI → 只有 GameCore.h:24 这条声明**)。接 ImGui 时正好在 `GameCore.cpp:50-54` 的渲染块里调用它。
- `GameCore.cpp:139-157` 的 `WndProc` 只处理 `WM_SIZE / WM_DESTROY`,**没有 `WM_KEYDOWN/WM_MOUSEMOVE`**;ImGui Win32 后端需要把它们转给 `ImGui_ImplWin32_WndProcHandler`,否则 UI 无法交互。
- `GameInput`(`Engine/01_Application/GameInput.cpp`)是独立的一套输入(`s_DXKeyMapping`,`GameInput.cpp:95/201`),与 ImGui 的输入不冲突但也不共享,需要保证 ImGui 抢占键盘时相机不动。

---

## 8. 文档现状

### 8.1 README 失效链接(逐条实测)

    README.md:9   | 📖 [Learning Log](docs/LearningLog.md) |  → 存在(413+ 行)
    README.md:10  | 🏗️ [Architecture](docs/Architecture.md) |  → **不存在**(docs/ 下只有 LearningLog.md / Roadmap.md / Showcase.md,实测 find)
    README.md:11  | 🛣️ [Roadmap](docs/Roadmap.md) |         → 存在(18 行)
    README.md:12  | 🎬 [Showcase](docs/Showcase.md) |        → 存在(26 行)

`README.md` 本身只有 20 行,`# Project Structure`(`README.md:16`)之后是**空的**——没有任何目录说明、构建步骤、依赖说明。新接手的成本极高。

### 8.2 `docs/Showcase.md` 是坏 HTML 片段

    docs/Showcase.md:3   </td>
    docs/Showcase.md:4   <td width="50%" valign="top">
    docs/Showcase.md:24  </td>
    docs/Showcase.md:25  </tr>
    docs/Showcase.md:26  </table>

文件**以 `</td>` 开头、以 `</table>` 结尾**,是某个 README 表格的一小块被单独抽出来;没有 `<table>` / `<tr>` / `<td>` 的开标签。GitHub 渲染会退化成普通文本。
另外其中 3 张图是 GitHub user-attachments 外链(`Showcase.md:9/11/18`),与仓库脱钩。
**而且图片内容宣称 "TAA 前/TAA 后"(`Showcase.md:9-14`),但 TAA 在代码里是空壳**:

    Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:361-365   // 9. TAA Pass    <- 只有 PIXBeginEvent/PIXEndEvent,中间没有任何绘制
    Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp:367-371   // 10. Tonemap Pass <- 同样是空 block
    Engine/04_Renderer/TemporalEffects.cpp:19-20  两行 include 分别是 // #include "ResolveTAACS.h" 与 // #include "SharpenTAACS.h",均被注释
    grep TAACS -> 全仓库只有上面这 2 行注释;Engine/Shader/04_PostProcess/TAACS.hlsl 无任何 include/引用者
    TemporalEffects 整体只有 Engine/02_RHI/GraphicsCore.cpp:269 的 Initialize() 被调用,没有一个 Pass 被使用

-> **Showcase 宣传的能力与代码不符**(PIXBeginEvent 标记存在 != 有实现)。

### 8.3 各模块 md 用的是**已废弃的目录编号**(ORPHAN)

实际磁盘目录与文档自称完全对不上:

| 实际目录 | 文档第一行自称 | 文档里引用的不存在的模块 |
| --- | --- | --- |
| `Engine/00_Core/Core.md` | `00_Core`(对) | — |
| `Engine/00_Core/Math/Math.md:1` | `13_Math` | 13_Math |
| `Engine/00_Core/Utility/Utility.md:1` | `12_Utility` | 12_Utility |
| `Engine/01_Application/Application.md:17-19` | — | `09_Renderer`、`10_Scene`、`02_Camera` |
| `Engine/02_RHI/RHI.md:19-20` | `02_RHI`(对) | `13_Math`、`12_Utility`、`RenderGraph` |
| `Engine/03_AssetSystem/AssetSystem.md:1` | `05_ResourceSystem` | `04_GPUInfrastructure`、`RenderGraph` |
| `Engine/05_Scene/Scene.md:1` | `10_Scene` | `05_ResourceSystem`、`13_Math`、`02_Camera` |
| `Engine/Shader/Shaders.md:1` | `11_Shaders` | — |

- 引用到的 `09_Renderer / 10_Scene / 02_Camera / 05_ResourceSystem / 04_GPUInfrastructure / 11_Shaders / 12_Utility / 13_Math / RenderGraph` **全部在磁盘上不存在**(实测:Engine 下只有 `00_Core / 01_Application / 02_RHI / 03_AssetSystem / 04_Renderer / 05_Scene / Shader / 20_cmake`)。
- 现有 md 全是"职责 / 允许依赖 / 被谁依赖 / 禁止内容"的 20~30 行模板,信息量极低,且依赖方向描述与实际代码不符(例如 `Engine/02_RHI/RHI.md:19-20` 说 RHI 依赖 `12_Utility`,但实际 `02_RHI` 大量直接 include `00_Core/PCH.h`、`00_Core/Utility/Utility.h`)。
- `Engine/Engine.md` 是唯一质量较高的(91 行,讲清分层与依赖方向,`Engine.md:53` 给了完整依赖链),`Engine.md:3-5` 还写明了坐标系/行主序约定——**这个应当被提升到 README**。

### 8.4 `docs/Roadmap.md` 有重复标题(BUG)

    docs/Roadmap.md:3    ## Version 1.0     ← Forward Rendering
    docs/Roadmap.md:7    ## Version 1.0     ← 延迟管线 + TAA + shadow + pbr/ibl + ssao  (编号重复)

两个 `Version 1.0`,第二个应为 `Version 1.5` 或 `Version 2.0`。另外 `Version 1.0` 里列出的 TAA / shadow / ssao **在代码里均未实现**(见 §9)。

### 8.5 `docs/LearningLog.md` HTML 结构不闭合

    实测: <details> 开标签 46 个,</details> 闭标签 45 个,最终嵌套深度 = 1(不闭合)
    实测逐行统计: 文件末尾 docs/LearningLog.md:367 为 <br>,之前的 :365 是一个 </details>,
          之后 file end;结构上 :365 的 </details> 关闭的是 :187 的 <details open>(2026-05 月节点),
          而 :10/:16 的 <details open>(2026年 / 2026-04 月)始终没有闭合 → 最终嵌套深度 = 1。
          GitHub 渲染时最外层两个折叠块会"吞掉"后续内容。

内容本身是有价值的(35 个日期条目,日期从 2026-04-08 到 2026-07-25,但 `2026-04` 与 `2026-05` 之后的**6 月、7 月只有 1 条**,`docs/LearningLog.md:343` 是 `2026-06-1` "6月复习,暂停进度",`:349` 是 `2026-07-25` "啃DXR管线")。结构问题会让折叠区在 GitHub 上渲染错乱。

---

## 9. 构建系统其它问题

### 9.1 三个 Demo 引用不存在的 include 目录(PARTIAL/潜伏 BUG)

    Demos/DeferredApp/CMakeLists.txt:23     ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache
    Demos/RayTracingApp/CMakeLists.txt:23   ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache

实测 `ls <root>/cmake-build-debug` → **No such file or directory**;磁盘上只有一个残留 `Demos/DeferredApp/cmake-build-debug/`(`CMakeCache.txt` 里 `CMAKE_MAKE_PROGRAM=F:/Clion/CLion 2026.1.3/bin/ninja/win/x64/ninja.exe`,是 CLion 旧默认目录)。因为 `TooiyumeEngine` 的 PUBLIC include 里有 `${CMAKE_BINARY_DIR}/ShaderCache` 兜底,现在不报错——**但它是"清掉 ShaderCache 才引爆"的潜伏地雷**。ForwardApp 则**没有**这两行(`Demos/ForwardApp/CMakeLists.txt` 只有 36 行),三个 Demo 的 CMake 并不一致。

### 9.2 `Engine/CMakeLists.txt` 的 GLOB 与显式重复(PARTIAL)

    Engine/CMakeLists.txt:5-9     file(GLOB_RECURSE ENGINE_SOURCES "*.cpp" "*.h" "*.hpp")
    Engine/CMakeLists.txt:14-16   add_library(TooiyumeEngine STATIC ${ENGINE_SOURCES}
                                            04_Renderer/TemporalEffects.cpp
                                            04_Renderer/TemporalEffects.h)

`GLOB_RECURSE` 已经包含 `TemporalEffects.cpp`,又显式列了一遍 → 同一文件被加两次。CMake 会去重:实测 `build.ninja` 里 `TemporalEffects.cpp.obj` 只出现 2 次,分别是**编译规则**(:528)与**链接对象列表**(:599),不存在"编译两次"——若真重复,编译规则会出现 2 条。**当前无害,但属于冗余**。另外 `GLOB_RECURSE` 无 `CONFIGURE_DEPENDS`(全仓库 `grep CONFIGURE_DEPENDS` → 0),**新增 .cpp 若不重新 configure 不会进构建**,这是新人最常踩的坑。实测当前 `Engine` 下 132 个 cpp/h 文件(不含 `.vs/`);但 `.vs/` 目录确实存在污染风险:`./.vs`、`./Engine/.vs`、`./Demos/ForwardApp/.vs` 三个目录在磁盘上存在(实测 `find`),只是**未被 git 跟踪**(`git ls-files | grep ".vs/"` → 0)。因为 `GLOB_RECURSE "*.cpp" "*.h"` 是**按扩展名**匹配,`.vs/` 里没有 .cpp 所以当前没混进构建;但若哪天 `.vs/` 下生成 .h(如 IntelliSense 缓存),就会静默进入编译 → 建议把 `GLOB_RECURSE` 加 `LIST_DIRECTORIES false` + 显式排除,或改用显式源文件清单。

### 9.3 POST_BUILD 拷 dll 用的是源码树 vcpkg_installed(PARTIAL)

    Demos/*/CMakeLists.txt:25,33   ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxcompiler.dll (及 dxil.dll)

但在 manifest 模式下,实际安装目录是 **build 目录**的 `${VCPKG_INSTALLED_DIR}`(实测 `CMakeCache.txt`:`VCPKG_INSTALLED_DIR = <root>/out/build/x64-debug/vcpkg_installed`,`VCPKG_MANIFEST_MODE:BOOL=ON`)。源码树那份 `<root>/vcpkg_installed` 是历史遗留。

- 实测两份 `dxcompiler.dll` 的 **md5 完全一致**(`69663ce3c009bd9fb3082a2bac130755`),所以现在没出问题;
- 但正确写法应是 `$<TARGET_FILE:...>` 或 `${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/...`。一旦 manifest 更新,源码树那份会过期 → 目标目录被拷进旧 dll 且 `copy_if_different` **不会报错**。

- 另外:`ThirdParty/PIX/bin/*.dll` 被 `.gitignore:30 ([Bb]in/)` 忽略(实测 `git check-ignore` 命中),**入库的只有 `ThirdParty/PIX/include/*.h` 与 `ThirdParty/PIX/lib/WinPixEventRuntime.lib`**。clean clone 后 `Demos/*/CMakeLists.txt:17,36` 的 `copy_if_different` 会因源文件不存在而**构建失败**(cmake -E copy_if_different 对不存在的源文件返回非 0)。这是 clean clone 的**第二个硬阻塞点**(第一个是 §4.2 模型缺失)。

### 9.4 `bin/` 被全局忽略导致 PIX dll 永不入库(实测)

    .gitignore:30      [Bb]in/
    git ls-files ThirdParty
      → 只有 6 个文件:include/PIXEvents.h, PIXEventsCommon.h, PIXEventsLegacy.h, pix3.h, pix3_win.h, lib/WinPixEventRuntime.lib
      → bin/WinPixEventRuntime.dll 与 bin/WinPixEventRuntime_UAP.dll **不在跟踪列表**
    ls ThirdParty/PIX/bin  → 两个 dll 在磁盘上存在(仅本机),说明被 .gitignore 挡在版本库外

修复:用 `git add -f ThirdParty/PIX/bin/WinPixEventRuntime.dll`,或把 `.gitignore` 的 `[Bb]in/` 改成只在根目录生效 `/bin/`,或改用 `!` 例外规则。

### 9.5 Debug/Release 的宏定义对不上(需实测确认)

    Engine/00_Core/Utility/Utility.h:89    #ifdef RELEASE
    Engine/00_Core/Utility/Utility.h:33    #ifndef RELEASE
    但全仓库没有任何地方 #define RELEASE / NDEBUG 映射:
      grep "RELEASE\|NDEBUG" Engine/00_Core/PCH.h Engine/00_Core/Utility/Utility.h
      → 只有上面两行判断,无定义

CMake 的 `CMAKE_BUILD_TYPE=Debug` 只会让 MSVC 加 `/MDd`(实测 build.ninja 里 63 处 `MDd`),*不*会自动定义 `RELEASE`。**结论:`RELEASE` 宏永远是未定义 → 即使在 Release 构建里,`ASSERT` 也照常生效(`__debugbreak`)**。这意味着 release 包会带断言并且可能崩在用户机器上。这是必须修的(应在 CMake 里 `target_compile_definitions(TooiyumeEngine PRIVATE $<$<CONFIG:Release>:RELEASE>)`)。

注意:CMake 对 Ninja 多配置生成器用的是 `$<CONFIG:...>` 生成表达式,单配置(Debug/Release 分开 configure)时也可直接 `if(CMAKE_BUILD_TYPE STREQUAL "Release") target_compile_definitions(... RELEASE)`。

**状态:BUG(逻辑推断;未跑 Release 构建实证,故可复现性标 UNKNOWN)。**

### 9.6 其它小问题

- `Demos/ForwardApp/CMakeLists.txt:5` 目标名 `TooiyumeApp` 与源文件 `Tooiyume.cpp` 不一致(另两个是 `DeferredApp.cpp`/`RayTracingApp.cpp`),写脚本/IDE run config 易踩空。
- `Engine/CMakeLists.txt:107-110`、`112-115`、`126-128` **三处重复**的 `target_include_directories`(前两处内容完全相同),复制粘贴残留。
- `Engine/CMakeLists.txt:106` 序号位置与 `# ==========` 注释块错位(注释在 91-93 行,实际 `add_dependencies` 在 94 行,中间夹了 PCH 段),可读性差但无害。
- `Engine/04_Renderer/environmentlightingmanager.txt` **是一个被 git 跟踪的临时笔记**(`git ls-files` 命中),内容是 IBL 实现步骤草稿(`environmentlightingmanager.txt:1-23`),不属于源码目录该有的东西。应移入 `docs/` 或删除。

---

## 10. Present / 帧循环现状(与 Demo 直接相关)

    Engine/01_Application/GameCore.cpp:39-58   UpdateApplication: game.Update(dt) → game.RenderScene() → Display::Present()
    Engine/01_Application/Display.cpp:227-234  Present(): PreparePresentSDR(*Display::SceneColorBuffer) → s_SwapChain1->Present(0,0)
    Engine/01_Application/Display.cpp:229      行内注释原文:"// 最终 Present Pass（后处理→屏幕）。… 目前阉割"
    Engine/01_Application/Display.cpp:249-260  PreparePresentSDR: Transition → CopyBuffer(displayPlane ← sceneColor) → Transition → Finish

**这是一个纯 CPU 端拷贝到 back buffer 的"阉割版 Present"**(代码注释自认:`Display.cpp:229` 原文含"目前阉割")。含义:

- 没有 tonemap、没有色彩空间转换(`SwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM`,`Display.cpp:29`),HDR 场景色会被直接截断;
- `RenderUI` 钩子(`GameCore.h:24`)在帧循环里**根本没有调用点**(实测 grep 全仓库 0 调用者)→ 接 ImGui 时需要在 `GameCore.cpp:50-54` 手动插入。

`Display::SceneColorBuffer` 是唯一的呈现入口(`Display.h:18`),`g_DisplayPlane[]` / `g_CurrentBuffer` 在 `Display.cpp:131-132` 的**匿名命名空间**里,**外部无法访问 back buffer** —— 这是接 ImGui(`ImGui_ImplDX12_RenderDrawData` 需要 RTV)的直接阻碍。

---

## 11. 汇总表

| 功能 / 项 | 状态 | 文件:行 | 证据说明 |
| --- | --- | --- | --- |
| 三个 Demo 结构同构、大量重复 | BUG(可维护性) | `Demos/ForwardApp/Tooiyume.cpp` / `DeferredApp.cpp` / `RayTracingApp.cpp` 全文 | Deferred 与 RT 96/100 行相同;三份 `Cleanup` 都多清一个从未加载的 `Models[2]`(:137/:132/:132,数组长度 10,不越界) |
| 合并统一入口可行性 | DONE(可行) | `RendererRegistry.cpp:16` 有 `CreateRenderer`,`RendererRegistry.h:38` 声明 | **但 0 个调用者** → 注册表本身是 ORPHAN |
| RendererRegistry 自动注册机制 | PARTIAL(注册已接/查找 ORPHAN) | 注册:`DeferredRenderer.cpp:33`、`ForwardRenderer.cpp:27`;查找:`RendererRegistry.cpp:16` 0 调用点 | 注册表被填充但 `CreateRenderer` 无人调用;且 `ForwardRenderer.cpp:27` 注册名尾部多空格 |
| RayTracingApp 是否为 DXR | ORPHAN | `RayTracingApp.cpp:55,73,121` | 用的是 `DeferredRenderer`;全仓库 0 处 `DispatchRays` |
| DXR 请求被静默丢弃 | BUG | `RayTracingApp.cpp:34,64` vs `GameCore.h:26`、`GameCore.cpp:29`、`GraphicsCore.cpp:171` | 签名少 `const`,非 override → 恒调基类返回 false |
| `g_bRaytracingSupported` | ORPHAN | `GraphicsCore.cpp:40,179,197,204`;未在 `GraphicsCore.h` extern | 只有写、无读 |
| `g_DeviceDXR`(`ID3D12Device5`) | PARTIAL | `GraphicsCore.cpp:39,183`;`GraphicsCore.h` 未暴露 | 已预留未获取/未暴露 |
| CommandList 是否满足 DXR | DONE | `CommandContext.h:189`(`ID3D12GraphicsCommandList4`);`CommandListManager.cpp:102` | 类型已够 RT,无需改 |
| 硬编码绝对路径 | BUG | 18 处,见 §3.1 | 全部 `D:/CS-Self-Study/...`,换机必挂 |
| `CMakePresets.json` toolchain 路径 | BUG(可移植性) | `CMakePresets.json:16` | `F:/VS/rep/vcpkg/...`,实测存在但不可移植 |
| `CMakeSettings.json` | BUG | `CMakeSettings.json:1-9` | 非法 JSON 片段(无外层 `{}`),`json.load` 报 Extra data |
| `ShaderCompiler.cmake` 定位 dxc | PARTIAL | `ShaderCompiler.cmake:6-11` | hardcode `x64-windows` triplet,换 triplet 即 REQUIRED 失败 |
| Shader 编译流水线(hlsl→cso→h) | DONE | `ShaderCompiler.cmake:30-131`、`BinToHeader.cmake:1-30` | 实测生成 `out/build/x64-debug/ShaderCache/*.h`,10 个 shader |
| Shader `.hlsli` 依赖追踪 | MISSING | `ShaderCompiler.cmake:87` | `DEPENDS` 只有主 .hlsl;改 `DeferredCommon.hlsli` 不触发重编 |
| `Shader/01_Default` 纳入编译 | MISSING | `Engine/CMakeLists.txt:21-89`(0 处 `01_Default`) | Forward 用的是 2026-05-04 入库旧 `.h`:`ForwardRenderer.cpp:16-17` |
| 生成头文件缺 include guard/inline | BUG(潜在) | `BinToHeader.cmake:9-28`;`ShaderCache/SkyboxVS.h` 尾部无 guard | 单 .cpp include 故暂无 LNK2005,第二个 include 者立刻炸 |
| Debug/Release shader flag 分支 | DONE | `ShaderCompiler.cmake:51-65` | Debug=`-Zi -Qembed_debug -Od`,Release=`-O3`,都编 |
| Shader 编译静默失败 | MISSING(不成立) | `ShaderCompiler.cmake:71-92` | ninja 会捕获非 0 退出码 → 不会静默通过 |
| `Assets/Model` 目录 | MISSING | `.gitignore:386`;`ls Assets/Model` 失败 | 三个 Demo 全部依赖它 |
| `Assets/` 实际内容 | PARTIAL | `Assets/Skybox/*`(3 文件) | dds+hdr+空 txt;**0 个 gltf/glb/ty** |
| 模型分发方案(LFS/下载脚本/README) | MISSING | `.gitattributes` 无 `filter=lfs`;`README.md:1-20` 无链接 | `.gitignore:382-386` 注释提了"建议 LFS",未落地 |
| Skybox DDS 有效性 | DONE | 二进制头实测:2048×1024,12 mips,FourCC | 文件合法;加载失败会静默回退品红纹理(`TextureManager.cpp:239`) |
| 模型缺失时的行为 | BUG(体验) | `ModelLoader.cpp:211-215`;`Model.cpp:47-53,107-108` | 只打印一行错误 → `nullptr` → 黑屏,不崩不提示 |
| `forceRebuild=true` 写死 | BUG | `Tooiyume.cpp:72`、`DeferredApp.cpp:77`、`RayTracingApp.cpp:77` | 抵消了 `ModelLoader.cpp:219-221` 的 mtime 缓存 |
| `m_HDRLoaded` 永不置位 | BUG | `EnvironmentLightingManager.cpp:150`;`.h:100` | 全仓库仅 2 处,无赋值 → bake 守卫该分支失效 |
| `EnvironmentLightingManager::SaveDDS` | STUB | `EnvironmentLightingManager.cpp:122-125` | 函数体为空;IBL 结果不落盘,每次启动重算 |
| `LoadHDR` 传入 .dds 而非 .hdr | UNKNOWN | `DeferredRenderer.cpp:40`(路径是 .dds);`EnvironmentLightingManager.cpp:174` | `CompileTextureOnDemand` 对 dds 输入的行为未实测 |
| TextureManager 根路径为空 | PARTIAL | `TextureManager.cpp:53,58`;`TextureManager.cpp:97` | `Initialize(L"")` → 完全依赖调用方绝对路径 |
| Present 为阉割版(直接 CopyBuffer) | PARTIAL | `Display.cpp:227-234,249-260` | 无 tonemap / 无 sRGB 转换;`Display.cpp:229` 注释自认"阉割" |
| RenderUI 钩子 | ORPHAN | `GameCore.h:24` 声明;全仓库 0 调用者 | 接 ImGui 需在 `GameCore.cpp:50-54` 插入 |
| back buffer 对外暴露 | MISSING | `Display.h:16-25` 只暴露 `SceneColorBuffer`;`g_DisplayPlane[]` 在 `Display.cpp:131` 匿名命名空间 | ImGui DX12 后端拿不到 RTV 的直接阻碍 |
| WndProc 键盘/鼠标消息 | MISSING | `GameCore.cpp:139-157` 只处理 `WM_SIZE/WM_DESTROY` | ImGui 交互必需补齐 |
| 三 Demo 引用不存在的 include 目录 | PARTIAL(潜伏 BUG) | `DeferredApp/CMakeLists.txt:23`、`RayTracingApp/CMakeLists.txt:23` | `<root>/cmake-build-debug` 实测不存在;ForwardApp 无此两行 |
| GLOB_RECURSE 无 CONFIGURE_DEPENDS | PARTIAL | `Engine/CMakeLists.txt:5-9` | 新增 .cpp 不 reconfigure 不进构建(全仓库 0 处 CONFIGURE_DEPENDS) |
| `TemporalEffects.cpp` 重复列入 | PARTIAL(无害) | `Engine/CMakeLists.txt:14-16` | GLOB 已含,又显式列;实测 build.ninja 只 1 个 obj(去重) |
| POST_BUILD 拷 dll 用源码树 vcpkg_installed | PARTIAL | `Demos/*/CMakeLists.txt:25,33` | manifest 模式实际在 `out/build/.../vcpkg_installed`;实测两份 md5 相同(暂无事故) |
| PIX dll 未入库 → clean clone 构建失败 | BUG | `.gitignore:30`;`git ls-files ThirdParty` 只有 include+lib,无 bin | `Demos/*/CMakeLists.txt:17,36` 的 `copy_if_different` 会因源缺失而失败 |
| `RELEASE` 宏永未定义 | BUG(未跑 Release 验证) | `Utility.h:89,33` 判断;全仓库无 `#define RELEASE` | Release 构建里 ASSERT 仍生效 → 可能 `__debugbreak` |
| 目标名与源文件名不一致 | PARTIAL | `Demos/ForwardApp/CMakeLists.txt:5` | `TooiyumeApp` vs `Tooiyume.cpp` |
| 三处重复 include 目录 | PARTIAL(无害) | `Engine/CMakeLists.txt:107-110,112-115,126-128` | 前两组完全相同 |
| `environmentlightingmanager.txt` | ORPHAN | `Engine/04_Renderer/environmentlightingmanager.txt:1-23`(git 跟踪中) | 临时笔记放入源码目录 |
| vcpkg 依赖 | DONE | `vcpkg.json:4-15`;`CMakeLists.txt:25-28`;`Engine/CMakeLists.txt:145-154` | 4 个依赖,链路完整 |
| imgui 依赖 | MISSING | `vcpkg.json` 无;全仓库 0 命中 | 接入清单见 §7.2 |
| README 结构说明 | MISSING | `README.md:16-20` | `# Project Structure` 之后是空的 |
| README → `docs/Architecture.md` | MISSING | `README.md:10` | 该文件不存在 |
| `docs/Showcase.md` HTML | BUG | `Showcase.md:3,4,24,25,26` | 以 `</td>` 开头、`</table>` 结尾,无开标签 |
| Showcase 宣称 TAA 与代码不符 | BUG | `Showcase.md:9-14` vs `TemporalEffects.cpp:19-20` | 图片是"TAA 前/后",代码里 TAA 全被注释 |
| 模块 md 的目录编号 | ORPHAN | `Math.md:1`(`13_Math`)、`Utility.md:1`(`12_Utility`)、`Scene.md:1`(`10_Scene`)、`Shaders.md:1`(`11_Shaders`)、`AssetSystem.md:1`(`05_ResourceSystem`) | 这些目录全部不存在 |
| 模块 md 引用的不存在模块 | ORPHAN | `Application.md:17-19`(`09_Renderer/10_Scene/02_Camera`);`RHI.md:19-20`(`13_Math/12_Utility`);`AssetSystem.md:16`(`04_GPUInfrastructure`) | 磁盘上无对应目录 |
| `Engine/Engine.md` 质量 | DONE | `Engine.md:3-5,27-91` | 唯一高质量文档(坐标系约定 + 依赖方向),应提升到 README |
| `docs/Roadmap.md` 重复标题 | BUG | `Roadmap.md:3,7` | 两个 `## Version 1.0` |
| `docs/LearningLog.md` HTML 不闭合 | BUG | `LearningLog.md` 全文件(`<details>` 46 vs `</details>` 45) | 最终嵌套深度 1;2026-07-25 条目后缺闭合 |
| `ThirdParty/PIX/include + lib` | DONE | `git ls-files ThirdParty` → 6 文件 | 头与 lib 已入库,仅 bin 缺失 |

---

## 12. 给后续实施的优先动作(按阻塞程度)

1. **【阻塞】补资产**:要么用 `git add -f Assets/Model` 入库,要么在 `README.md` 写明模型下载地址 + 加一个 `fetch_assets.ps1`;否则新机器无法验证任何渲染。同时清掉 `Assets/Skybox/新建文本文档.txt`。
2. **【阻塞】PIX dll**:`git add -f ThirdParty/PIX/bin/WinPixEventRuntime.dll`(及 UAP 版可选),否则 clean clone 在 POST_BUILD 阶段直接失败。
3. **【高】去绝对路径**:把 18 处 `D:/CS-Self-Study/...` 换成"可执行文件目录 + 相对路径"或统一 `AssetsRoot` 全局变量;顺带把每份 Demo 里 5~7 个死变量收敛到 2 个。
4. **【高】合并三个 Demo**:先做 §1.4 的统一入口,再谈 DXR;否则 DXR 会变成第 4 份拷贝。
5. **【高】修 `RequiresRaytracingSupport` 签名**(加 `const` + `override`),让 DXR 请求真正生效——这是 DXR 的第一块多米诺。
6. **【高】Shader 依赖**:给 `compile_shader` 的 `DEPENDS` 加 `file(GLOB SHADER_HEADERS "Engine/Shader/**/*.hlsli")`,否则每次改公共库都要手动清 `ShaderCache`。
7. **【高】`Shader/01_Default` 纳入编译 + 删掉入库的 `.h`**,并把 `ForwardRenderer.cpp:16-17` 的 include 改成不带目录的 `"DefaultVS.h"`(与 Deferred 一致,命中 `ShaderCache`)。
8. **【中】生成头加 `#pragma once` + `static`/`inline`**,避免第二个 include 者触发 LNK2005。
9. **【中】修 `RELEASE` 宏**:CMake 里加 `target_compile_definitions(TooiyumeEngine PRIVATE $<$<CONFIG:Release>:RELEASE>)`。
10. **【中】文档**:删 `CMakeSettings.json` 或补外壳;修 `Showcase.md` 的 HTML 与 TAA 宣传;删 `README.md:10` 的 `Architecture.md` 链接或补写;把 `Engine/Engine.md` 提到 README;模块 md 统一按 `00_Core/01_Application/02_RHI/...` 真实编号重写。
11. **【低】ImGui**:按 §7.2 五步接入;注意先给 `Display` 加 back buffer RTV 访问接口、给 `WndProc` 加键鼠消息、在 `GameCore.cpp:50-54` 调 `RenderUI`。
