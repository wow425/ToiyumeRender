# 07 BuildVerify — 构建实证审计

审计时间: 2026-10-24 11:58 ~ 12:15 (Asia/Shanghai)
审计方式: **实际执行命令**(唯一允许执行命令的审计任务)。本文件区分「我实际执行的」与「我推测的」,推测部分显式标注。
源码未做任何修改,未做 git 操作。构建产物仅写入 out/audit_build/ 与复用 out/build/x64-debug。

---

## 0. 结论速览(TL;DR)

| 问题 | 结论 |
| --- | --- |
| 能否构建? | **能。三个 Demo 全部产出 exe。** 全新配置目录干净构建 83/83 步全通过 |
| 复用 out/build/x64-debug 直接构建? | **第一次失败**,三个 target 全部 exit=1,错误 fatal error C1853 |
| 失败根因 | 该目录的 PCH 由**已不存在的工具集** 14.44.35225.00 生成,当前 cl 为 19.44.35229.0 |
| 修复方式 | 删除 cmake_pch.cxx.pch 后重建 → 三个 target 全部 exit=0。**无需改源码** |
| 是否缺少 VS / vcpkg? | 均具备,无环境阻碍 |
| 最严重代码问题 | RequiresRaytracingSupport() **const 签名不匹配**,RayTracingApp 的 DXR 请求被静默丢弃 |

---

## 1. 环境清单(我实际执行的)

    cmake --version   → cmake version 3.31.6-msvc6
                        (F:/VS/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe)
    ninja --version   → 1.12.1
                        (F:/VS/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe)
    where cl          → F:/VS/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe
    cl /Bv            → 19.44.35229.0 (x64); c1.dll / c1xx.dll / c2.dll 同版本

关键点:**系统 PATH 中原本没有 cmake / ninja / cl**。
- 直接执行得到 `cmake: command not found`、`ninja: command not found`;`where cl` → "无法找到文件"(GBK 乱码原文)。
- 必须先 `call F:/VS/Community/VC/Auxiliary/Build/vcvars64.bat` 才可用。
- vswhere 报告 VS 安装在 **F:/VS/Community**(VS 2022 Community,v17.14.42)。

vcpkg / 依赖探测:

| 项目 | 实际路径 | 存在? |
| --- | --- | --- |
| vcpkg toolchain | F:/VS/rep/vcpkg/scripts/buildsystems/vcpkg.cmake | 存在(48552 B) |
| 项目根 vcpkg_installed | <root>/vcpkg_installed | 存在 |
| build 目录 vcpkg_installed | <root>/out/build/x64-debug/vcpkg_installed | 存在 |

CMakeCache.txt 历史值(实际读取):

    out/build/x64-debug/CMakeCache.txt:220  CMAKE_TOOLCHAIN_FILE:STRING=F:/VS/rep/vcpkg/scripts/buildsystems/vcpkg.cmake
    out/build/x64-debug/CMakeCache.txt:27   CMAKE_CXX_COMPILER:STRING=cl.exe
    out/build/x64-debug/CMakeCache.txt:99   CMAKE_MAKE_PROGRAM:UNINITIALIZED=F:/VS/COMMUNITY/COMMON7/IDE/COMMONEXTENSIONS/MICROSOFT/CMAKE/Ninja/ninja.exe
    out/build/x64-debug/CMakeCache.txt:408  CMAKE_GENERATOR:INTERNAL=Ninja
    out/build/x64-debug/CMakeCache.txt:297  VCPKG_TARGET_TRIPLET:STRING=x64-windows
    out/build/x64-debug/CMakeCache.txt:21   CMAKE_BUILD_TYPE:STRING=Debug

> 推测(未验证): CMAKE_CXX_COMPILER 记录为裸 cl.exe,是该目录由 CLion 而非手工 cmake 命令生成的特征;CMakeFiles/ 下同时残留 clion-environment.txt 与 clion-x64 Debug-log.txt,支持此判断。

**构建目录陈旧性证据**(实际读取 CMakeFiles/):

    out/build/x64-debug/CMakeFiles/3.31.6-msvc6/CMakeCXXCompiler.cmake:4  CMAKE_CXX_COMPILER_VERSION "19.44.35225.0"
    out/build/x64-debug/CMakeFiles/4.3.2/CMakeCXXCompiler.cmake:4         CMAKE_CXX_COMPILER_VERSION "19.44.35225.0"

同一 build 目录存在 **两套 CMake 版本号子目录**(3.31.6-msvc6 与 4.3.2),且两处记录的编译器版本都是 19.44.35225.0,而磁盘上现存工具集为 14.44.35207(cl 自报 19.44.35229.0)。
→ 构建目录是**跨工具集升级遗留**的。

---

## 2. 配置结果(我实际执行的)

### 2.1 复用的旧 build 目录

未重新配置,直接 ninja(见第 3 节)。

### 2.2 全新配置(干净验证)

    cmake -S <root> -B <root>/out/audit_build/cmake_verify -G Ninja
          -DCMAKE_BUILD_TYPE=Debug
          -DCMAKE_TOOLCHAIN_FILE=F:/VS/rep/vcpkg/scripts/buildsystems/vcpkg.cmake
          -DVCPKG_TARGET_TRIPLET=x64-windows
          -DCMAKE_MAKE_PROGRAM=F:/VS/Community/.../Ninja/ninja.exe

结果 **CONFIGURE_EXIT=0**,日志 out/audit_build/configure_fresh.log(203 行):

    Compiler found: F:/VS/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe
    The following packages will be built and installed:
        directx-dxc:x64-windows@2026-02-20
        directx-headers:x64-windows@1.619.1
      * directxmath:x64-windows@2026-03-12
        directxmesh[core,dx12]:x64-windows@2026-03-31
        directxtex[core,dx11,dx12]:x64-windows@2026-03-31
      * vcpkg-cmake  * vcpkg-cmake-config
    All requested installations completed successfully in: 1.4 min
    -- Running vcpkg install - done
    -- The CXX compiler identification is MSVC 19.44.35229.0
    -- Found OpenMP_C: -openmp (found version "2.0")
    -- Found OpenMP_CXX: -openmp (found version "2.0")
    -- Configuring done (106.2s)
    -- Generating done (0.1s)
    -- Build files have been written to: .../out/audit_build/cmake_verify

- **配置零 warning、零 error。**
- DXC_EXECUTABLE 正确解析(实际读取 cache:221):
  DXC_EXECUTABLE:FILEPATH=<build>/vcpkg_installed/x64-windows/tools/directx-dxc/dxc.exe
- 4 个 target 全部注册成功(cmake_verify/build.ninja 实际 grep):TooiyumeEngine / TooiyumeApp / DeferredApp / RayTracingApp。

---

## 3. 构建结果(我实际执行的)

### 3.1 第一次:复用 out/build/x64-debug —— **失败**

先 dry-run 确认待办步数(实际执行 ninja -n TooiyumeApp):

    [1/6] Compiling Shader: Shader/02_Deferred/LightingPS.hlsl
    [2/6] Generating Header: .../ShaderCache/LightingPS.h
    [3/6] Building CXX object Engine/CMakeFiles/TooiyumeEngine.dir/03_AssetSystem/Importers/glTF.cpp.obj
    [4/6] Building CXX object Engine/CMakeFiles/TooiyumeEngine.dir/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp.obj
    [5/6] Linking CXX static library Engine/TooiyumeEngine.lib
    [6/6] Linking CXX executable Demos/ForwardApp/TooiyumeApp.exe

实际构建(ninja -v,三个 target):

    TOOIYUMEAPP_EXIT=1
    DEFERREDAPP_EXIT=1
    RAYTRACINGAPP_EXIT=1

**三个 target 全部失败,且是同一个错误。**

### 3.2 第二次:删除过期 PCH 后重建 —— **成功**

    del /f /q Engine/CMakeFiles/TooiyumeEngine.dir/cmake_pch.cxx.pch
    del /f /q Engine/CMakeFiles/TooiyumeEngine.dir/cmake_pch.cxx.obj

    TOOIYUMEAPP_EXIT=0     (58/58,含全引擎 57 个 obj 重编)
    DEFERREDAPP_EXIT=0     ([1/1] Linking CXX executable Demos/DeferredApp/DeferredApp.exe)
    RAYTRACINGAPP_EXIT=0   ([1/1] Linking CXX executable Demos/RayTracingApp/RayTracingApp.exe)

随后再次执行 cmake --build ... --target TooiyumeApp --target DeferredApp --target RayTracingApp:

    CMAKE_BUILD_EXIT=0
    ninja: no work to do.

→ **复用旧目录在清掉 PCH 后已完全可用。**

### 3.3 第三次:全新目录干净全量构建 —— **成功**

    FULLCLEAN_EXIT=0

产物时间戳(实际 ls,out/audit_build/cmake_verify/):

    Engine/TooiyumeEngine.lib                  29119276 B   2026-10-24 12:11:48
    Demos/ForwardApp/TooiyumeApp.exe            2401792 B   2026-10-24 12:11:51
    Demos/DeferredApp/DeferredApp.exe           2682368 B   2026-10-24 12:11:50
    Demos/RayTracingApp/RayTracingApp.exe       2681856 B   2026-10-24 12:11:51

干净构建共 **83 步**:10 Shader 编译 + 10 Header 生成 + 59 个 CXX obj + 1 lib + 3 exe。**0 FAILED,0 条 error C**。

时间成本:配置 106.2 s(vcpkg 装包 1.4 min 占大头),干净全量编译约 31 s(16 核机器);增量构建(2 obj + 2 link) < 5 s。**远低于 25 分钟预算。**

### 3.4 三个 exe 的实际状态

| Demo | 产物 | 备注 |
| --- | --- | --- |
| ForwardApp → TooiyumeApp.exe | 生成 | 链接成功 |
| DeferredApp → DeferredApp.exe | 生成 | 链接成功 |
| RayTracingApp → RayTracingApp.exe | 生成 | **可链接,但工程内无任何 DXR 实现**,见 5.2 |

按任务要求**未启动任何 exe**。

---


## 4. 原始报错摘录

### 4.1 唯一的失败错误(原文,GBK 已转 UTF-8)

    Engine/03_AssetSystem/Importers/glTF.cpp: fatal error C1853:
      ".../out/build/x64-debug/Engine/CMakeFiles/TooiyumeEngine.dir/./cmake_pch.cxx.pch"
      预编译头文件来自编译器的其他版本,或者预编译头为 C++ 而在 C 中使用它(或相反)

    Engine/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp: fatal error C1853: (同上)

    FAILED: Engine/CMakeFiles/TooiyumeEngine.dir/03_AssetSystem/Importers/glTF.cpp.obj
    FAILED: Engine/CMakeFiles/TooiyumeEngine.dir/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp.obj
    ninja: build stopped: subcommand failed.

只有 **2 条**编译错误(同一 C1853),每条紧跟一条 FAILED: 行。无链接错误。

### 4.2 根因实证(二进制级证据)

直接读取 .pch 头部 48 字节:

    失败时:      VCPCH0......E.g.pX.'14.44.35225.00...........
    删除重建后:  VCPCH0......E.g.pX.'14.44.35229.00...........

- 旧 PCH 内嵌版本 14.44.35225.00,与旧 CMakeCXXCompiler.cmake 记录的 19.44.35225.0 一致。
- 当前 cl 为 19.44.35229.0(= 14.44.35229.00)。**MSVC 次版本号不同,PCH 不兼容。**
- 推断链(部分推测):VS 工具集被增量升级(14.44.35207 目录内 c1xx.dll 从 .35225 更新到 .35229)-> CMakeCXXCompiler.cmake 未被重新探测 -> PCH 未被判定失效 -> cl 拒绝加载 -> C1853。
- **机制要点**:cmake_pch.hxx、cmake_pch.cxx、PCH.h 的 mtime 都是 2026-05-15 15:19:57,而 PCH 是 2026-07-27 22:00:51。源文件没变但生成它的编译器变了,ninja 的 mtime 依赖判定认为 PCH 仍然新鲜 -> **不会自动重编**。这是该 bug 会反复出现的根本原因。

### 4.3 警告(全部,共 5 条)

C++ 警告(2 条,干净构建同样复现):

    Engine/00_Core/Utility/Utility.cpp(97): warning C5033: "register"不再为受支持的存储类
    Engine/04_Renderer/Renderer/Base/BaseRenderer.cpp(77): warning C4319: "~": 将"uint32_t"扩展到更大的"uint64_t"时为零

HLSL 警告(3 条,同一处,每个 include 它的 shader 各报一次):

    Engine/Shader/02_Deferred/DeferredCommon.hlsli:60:12: warning: implicit truncation of vector type [-Wconversion]
        return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
               ^

分别由 StaticMeshVS.hlsl:7、GBufferPS.hlsl:1、LightingPS.hlsl:4 include 触发(干净构建日志 [1/83][2/83][4/83])。

**警告定位与判断:**

- Utility.cpp:97 —— 代码为 register const __m128i Source = _mm_castps_si128(FillVector);(位于 SIMDMemFill 内)。C++17 起 register 已移除,C++20 下处于硬错误边缘;此处仅 warning。**建议直接删掉 register**。
- BaseRenderer.cpp:77 —— 代码为 key.key = ~dist.u;(透明 pass 反序排序)。SortKey 位域定义在 BaseRenderer.h:116-127:uint64_t objectIdx:16 / psoIdx:12 / key:32 / passID:4。~dist.u 是 uint32_t,零扩展进 32-bit 位域**数值正确**,warning 属良性。但同函数 BaseRenderer.cpp:59-60 把负距离钳成 0(dist.f = distance > 0.0f ? distance : 0.0f),与 ~dist.u 的反序意图叠加后,距离为 0 的透明物体排序会退化 —— 属设计噪声,非构建阻塞。
- DeferredCommon.hlsli:60 —— FresnelSchlick 返回 F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);。(1.0 - F0) 是 float3,pow(float,float) 返回标量 float,float3 * float 合法;warning 指向标量与 float3 混合提升的隐式截断。**不影响正确性**,但建议显式写出标量再广播以消警。

---

## 5. 构建暴露的代码/工程问题(带推理链)

### 5.1 BUG —— RequiresRaytracingSupport() const 签名不匹配,DXR 请求被静默丢弃

证据(全部实际读取):

    Engine/01_Application/GameCore.h:26        virtual bool RequiresRaytracingSupport() const { return false; }
    Demos/RayTracingApp/RayTracingApp.cpp:34   virtual bool RequiresRaytracingSupport();          // 少了 const
    Demos/RayTracingApp/RayTracingApp.cpp:64   bool Tooiyume::RequiresRaytracingSupport() { return true; }
    Engine/01_Application/GameCore.cpp:29      Graphics::Initialize(game.RequiresRaytracingSupport());
    Engine/02_RHI/GraphicsCore.h:20            void Initialize(bool RequireDXRsupport = false);
    Engine/02_RHI/GraphicsCore.cpp:169         if (RequireDXRSupport == true)

推理链:
1. Tooiyume::RequiresRaytracingSupport() 非 const,且**没有 override** 基类虚函数,是一个**独立的新函数**(仅名字相同)。
2. GameCore.cpp:29 的 game 静态类型为 IGameApp&,可见的只有 IGameApp::RequiresRaytracingSupport() const。
3. 因此该调用**永远命中基类版**,恒返回 false。
4. GraphicsCore.cpp:169 的 if 永不进入 -> g_bRaytracingSupported(GraphicsCore.cpp:40)恒为 false,g_DeviceDXR 永不 QueryInterface。
5. 结果:RayTracingApp 在 RayTracingApp.cpp:64 声明要 DXR,**实际拿不到 DXR 设备接口**。

为何编译不报错:两者签名不同,构成重载而非覆盖;且派生类未用 override 关键字,编译器无从检查。
**验证/修复手段**:把 RayTracingApp.cpp:34 改成 virtual bool RequiresRaytracingSupport() const override;、行 64 同步加 const。加了 override 后,若签名仍不匹配会立即编译失败 —— 可作反向验证。

### 5.2 ORPHAN —— RayTracingApp 无任何光线追踪实现

证据:
- RayTracingApp.cpp:69-72 在 Renderer::RendererCreateDesc desc 之后直接 DeferredRenderer.Initialize(desc)(复用 Deferred 渲染器)。
- 在 Engine/ 与 Demos/ 的全部 .cpp/.h 中搜索 DispatchRays:除 Engine/02_RHI/d3dx12.h 辅助头外 **0 命中**。
- Engine/04_Renderer/Renderer/ 目录实际内容只有 Base / Deferred / Forward / RendererConfig.h —— **没有 RayTracing 渲染器**。
- diff Demos/DeferredApp/DeferredApp.cpp 与 Demos/RayTracingApp/RayTracingApp.cpp:差异仅为 RequiresRaytracingSupport 声明与加载的模型文件,其余逻辑同构。

-> RayTracingApp.exe 实质是「DeferredApp 换模型 + 举了个坏掉的 DXR 旗子」。

### 5.3 BUG/ORPHAN —— ForwardRenderer 使用**源码树内陈旧的**生成头,不走构建生成的 shader

证据:

    Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp:16  #include "Shader/01_Default/DefaultVS.h"
    Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp:17  #include "Shader/01_Default/DefaultPS.h"
    Engine/04_Renderer/Renderer/Forward/ForwardRenderer.cpp:302 defaultPSO.SetVertexShader(g_pDefaultVS, sizeof(g_pDefaultVS));

而 Engine/CMakeLists.txt 的 10 处 compile_shader()(行 21/28/35/42/49/56/63/70/77/84)**没有一处涉及 01_Default**;在其中 grep DefaultVS、DefaultPS 计数均为 **0**。

实际磁盘:

    Engine/Shader/01_Default/DefaultVS.h   60167 B  2026-05-04 14:33:20   入库的旧产物
    Engine/Shader/01_Default/DefaultPS.h   42742 B  2026-05-04 20:06:07   入库的旧产物

因 Engine/CMakeLists.txt:107-110 与 112-115 的 target_include_directories 同时包含 ${CMAKE_CURRENT_SOURCE_DIR}(= Engine)与 ${CMAKE_BINARY_DIR}/ShaderCache,include 解析先命中 **Engine/Shader/01_Default/DefaultVS.h**,而非 ShaderCache 里的生成头。该头内部还残留旧目录的调试信息(shader debug name: .../ToiyumeRender/Build/CompiledShaders/Model/PDBs/DefaultVS.pdb)。

后果:**Forward 路径的 VS/PS 字节码自 2026-05-04 起从未被 dxc 重新编译**;修改 DefaultVS.hlsl / DefaultPS.hlsl 对 Forward 渲染**完全无效**,且构建过程不给任何提示。这是本工程最容易被忽视的隐患。

### 5.4 PARTIAL —— 着色器编译流水线可用,但缺 hlsli 依赖追踪

- Engine/20_cmake/ShaderCompiler.cmake 实现了 compile_shader():dxc -> .cso -> BinToHeader.cmake -> .h,再由 add_custom_target + Engine/CMakeLists.txt:94-105 的 add_dependencies 挂到 TooiyumeEngine。**功能可用**,10 个 shader 全部编出(干净构建实证 [1/83]~[10/83])。
- 但 ShaderCompiler.cmake 的 add_custom_command **DEPENDS 只写了 ${SHADER_SOURCE} 主文件**,没有任何 .hlsli。
  -> 修改 DeferredCommon.hlsli / Common.hlsli **不会触发任何 shader 重编**。与 5.3 属同源的「陈旧产物不失效」缺陷。
- Debug 分支 -Zi -Qembed_debug -Od,Release 分支仅 -O3;符合预期。

### 5.5 ORPHAN —— 两个 .hlsl 永不编译

磁盘上 Engine/Shader 下共 13 个 .hlsl,注册进构建的只有 10 个。未注册:

| 文件 | 状态 | 证据 |
| --- | --- | --- |
| Engine/Shader/01_Default/DefaultVS.hlsl | ORPHAN | 见 5.3,改用入库的 .h,不经 dxc |
| Engine/Shader/01_Default/DefaultPS.hlsl | ORPHAN | 同上 |
| Engine/Shader/04_PostProcess/TAACS.hlsl | ORPHAN | TemporalEffects.cpp:19-20 只剩注释,全工程无引用者 |
| Engine/Shader/05_BRDF/BRDF.hlsl、BRDFHelp.hlsl | 辅助 include | 不产出独立 cso,属正常用法 |

### 5.6 STUB —— 后处理 / TAA 全是注释

Engine/04_Renderer/TemporalEffects.cpp:19-20 两行 include 均为注释状态(// #include "ResolveTAACS.h"、// #include "SharpenTAACS.h");对 TAACS 做全工程 grep,只有这两行注释命中。Engine/Shader/04_PostProcess/TAACS.hlsl 存在但无任何引用者。

### 5.7 工程配置瑕疵(不阻塞构建,但会咬人)

**(a) DeferredApp / RayTracingApp 的 include 指向不存在的目录**

    Demos/DeferredApp/CMakeLists.txt:23     ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache
    Demos/RayTracingApp/CMakeLists.txt:23   ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache

实际 ls <root>/cmake-build-debug -> **No such file or directory**。这是 CLion 旧默认 build 目录的残留。构建日志中该 -I 参数确实出现(build_deferredapp.log 第 3 步),因有 out/build/x64-debug/ShaderCache 兜底而未报错。它是「清掉 ShaderCache 才会引爆」的潜伏地雷。

**(b) ForwardApp 的目标名与源文件名不一致**

Demos/ForwardApp/CMakeLists.txt:5 -> add_executable(TooiyumeApp WIN32 Tooiyume.cpp)。目标名 TooiyumeApp、源文件 Tooiyume.cpp;另两者分别是 DeferredApp.cpp / RayTracingApp.cpp。写脚本或 IDE run config 时易踩空。

**(c) Engine/CMakeLists.txt 存在三处重复的 target_include_directories**

    Engine/CMakeLists.txt:107-110  (Engine; ${CMAKE_BINARY_DIR}/ShaderCache)
    Engine/CMakeLists.txt:112-115  (与上一组完全重复)
    Engine/CMakeLists.txt:120-123  (Engine,第三次)

无害,但说明有复制粘贴残留。

**(d) POST_BUILD 的 DLL 源用的是源码树 vcpkg_installed,而非 build 目录的**

    Demos/*/CMakeLists.txt:25  ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxcompiler.dll
    Demos/*/CMakeLists.txt:33  ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxil.dll
## 6. 可执行的修复清单(按优先级)

1. **【立刻,治本】让 PCH 能对工具集升级可靠失效**
   现状:mtime 判定挡不住工具集升级。最小可执行修复 —— 在本地脚本/CI 的构建前置步骤里,读取 `Engine/CMakeFiles/TooiyumeEngine.dir/cmake_pch.cxx.pch` 的头 48 字节内嵌版本串(形如 `14.44.35225.00`),与本机 `cl /Bv` 的 `19.44.35229.0` 比对,不一致则先删 `.pch` 与 `.obj` 再构建。
   长期做法:工具集升级后强制 reconfigure(删 `CMakeFiles/CMakeCXXCompiler.cmake` 或整个 build 目录),因为旧目录里记录的还是 `19.44.35225.0`。

2. **【立刻】修 RequiresRaytracingSupport 签名**
   `Demos/RayTracingApp/RayTracingApp.cpp:34` 与 `:64` 加 `const` 并加 `override`,让编译器帮你守住(见 5.1)。

3. **【高】把 01_Default 的两个 shader 纳入 compile_shader,并去掉入库的 .h**
   在 `Engine/CMakeLists.txt` 增加:
   ```
   compile_shader(DefaultVS Shader/01_Default/DefaultVS.hlsl MainVS vs_6_4)
   compile_shader(DefaultPS Shader/01_Default/DefaultPS.hlsl MainPS ps_6_4)
   ```
   并把 `ForwardRenderer.cpp:16-17` 的 include 改指向 ShaderCache 版本。
   **最直接的验证手段**:直接删掉 `Engine/Shader/01_Default/DefaultVS.h` 与 `DefaultPS.h`——若能编过,说明 include 已正确落到 ShaderCache;若编不过,说明管线还没接上。

4. **【高】给 compile_shader 补 .hlsli 依赖**
   `Engine/20_cmake/ShaderCompiler.cmake` 的 `add_custom_command` 增加 `DEPENDS` 该 shader 目录下所有 `.hlsli`(可 `file(GLOB)` 或显式列举),否则改公共 include 不重编。

5. **【中】清理 cmake-build-debug/ShaderCache 死路径**
   `Demos/DeferredApp/CMakeLists.txt:20-24`、`Demos/RayTracingApp/CMakeLists.txt:20-24` 改为 `${CMAKE_BINARY_DIR}/ShaderCache`。

6. **【中】POST_BUILD 的 DLL 源改用 build 目录**
   `${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin/`,避免「构建绿 / 运行缺 DLL」。

7. **【低】清 register 关键字**(`Engine/00_Core/Utility/Utility.cpp:97`),消 C5033。

---

## 7. 汇总表

| 功能 | 状态 | 文件:行 | 证据说明 |
| --- | --- | --- | --- |
| CMake 配置(全新目录) | DONE | 命令见 2.2 | CONFIGURE_EXIT=0,零 warning/error,4 target 全注册 |
| vcpkg 依赖解析 | DONE | <root>/vcpkg.json | 实装 7 包(directxtex / directxmesh / directx-headers / directx-dxc + 3 工具包),1.4 min 全成功 |
| dxc 可执行定位 | DONE | Engine/20_cmake/ShaderCompiler.cmake:7-13 | cache:221 解析到 .../tools/directx-dxc/dxc.exe |
| Shader 编译流水线 | DONE | ShaderCompiler.cmake:44-77 | 10 个 hlsl 全编出 .cso + .h(干净构建 [1/83]~[20/83] 实证) |
| Shader 对 .hlsli 的依赖追踪 | MISSING | ShaderCompiler.cmake(DEPENDS 仅 ${SHADER_SOURCE}) | 仅依赖主文件,改 .hlsli 不触发重编 |
| 引擎静态库构建 | DONE | Engine/CMakeLists.txt:14 | 干净构建 59 个 obj -> TooiyumeEngine.lib 29119276 B |
| ForwardApp 可执行 | DONE | Demos/ForwardApp/CMakeLists.txt:5 | TooiyumeApp.exe 2401792 B 链接成功 |
| DeferredApp 可执行 | DONE | Demos/DeferredApp/CMakeLists.txt:5 | DeferredApp.exe 2682368 B 链接成功 |
| RayTracingApp 可执行 | DONE(仅链接) | Demos/RayTracingApp/CMakeLists.txt:5 | RayTracingApp.exe 2681856 B 链接成功 |
| 增量构建(复用旧目录) | BUG | out/build/x64-debug | 首轮 3 target 全 exit=1,仅 2 条 C1853,根因 = 跨工具集 PCH |
| PCH 失效判定 | BUG | out/build/.../cmake_pch.cxx.pch | 内含 14.44.35225.00,需 19.44.35229.0;mtime 依赖挡不住工具集升级 |
| PCH 重建后增量构建 | DONE | — | 删 .pch/.obj 后 3 target 全 exit=0 |
| 干净全量构建 | DONE | out/audit_build/cmake_verify | FULLCLEAN_EXIT=0,83/83,0 FAILED,约 31 s |
| DXR 能力请求 | BUG | GameCore.h:26 / RayTracingApp.cpp:34,64 | 基类 const、派生非 const -> 恒走基类返回 false,DXR 永不启用 |
| 光线追踪渲染实现 | MISSING | Engine/04_Renderer/Renderer/ | 只有 Base/Deferred/Forward;DispatchRays 全工程 0 实现命中 |
| Forward 渲染的 shader 来源 | ORPHAN | ForwardRenderer.cpp:16-17 | include 解析到入库的 Engine/Shader/01_Default/DefaultVS.h(2026-05-04),非 ShaderCache |
| 01_Default shader 构建注册 | MISSING | Engine/CMakeLists.txt:21-89 | 10 处 compile_shader 无一涉及 01_Default;grep 计数 0 |
| TAACS / 后处理 | STUB | TemporalEffects.cpp:19-20 | 两行 include 均为注释;TAACS.hlsl 无引用者 |
| DeferredApp/RayTracingApp include 路径 | BUG(潜伏) | DeferredApp/CMakeLists.txt:23;RayTracingApp/CMakeLists.txt:23 | 指向不存在的 <root>/cmake-build-debug/ShaderCache |
| POST_BUILD DLL 拷贝 | PARTIAL | Demos/*/CMakeLists.txt:17,25,33 | 三份 DLL 实际拷入成功,但源用源码根 vcpkg_installed 而非 build 目录 |
| OpenMP 依赖 | DONE | 由 directxtex-config.cmake:32 引入 | 配置日志 Found OpenMP_CXX -openmp "2.0",链接通过 |
| register 存储类残留 | BUG(警告) | Utility.cpp:97 | warning C5033,C++20 下应删除 |
| SortKey 位域零扩展 | BUG(警告) | BaseRenderer.cpp:77 / BaseRenderer.h:116-127 | warning C4319,数值正确但语义有噪声 |
| HLSL FresnelSchlick 隐式截断 | BUG(警告) | Shader/02_Deferred/DeferredCommon.hlsli:60 | dxc -Wconversion,3 次(3 个 shader include 触发) |
| exe 运行行为 | UNKNOWN | — | 任务禁止启动 exe,仅验证构建 |
| gltf 资源加载 | UNKNOWN | ForwardApp/Tooiyume.cpp:54-57 | 硬编码绝对路径,未运行验证;换机必挂风险 |
| Release 构建 | UNKNOWN | CMakeLists.txt:12 | 本次仅验证 Debug,Release 的 -O3 分支未实跑 |

---

## 8. 复现步骤(可直接抄)

```bat
call "F:\VS\Community\VC\Auxiliary\Build\vcvars64.bat"

:: 方案 A:复用已有目录(先清过期 PCH)
del /f /q "<root>\out\build\x64-debug\Engine\CMakeFiles\TooiyumeEngine.dir\cmake_pch.cxx.pch"
del /f /q "<root>\out\build\x64-debug\Engine\CMakeFiles\TooiyumeEngine.dir\cmake_pch.cxx.obj"
cmake --build "<root>\out\build\x64-debug" --target TooiyumeApp --target DeferredApp --target RayTracingApp

:: 方案 B:全新目录
cmake -S "<root>" -B "<root>\out\audit_build\cmake_verify" -G Ninja ^
      -DCMAKE_BUILD_TYPE=Debug ^
      -DCMAKE_TOOLCHAIN_FILE=F:/VS/rep/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build "<root>\out\audit_build\cmake_verify"
```

本次审计产生的全部原始日志保留在 `out/audit_build/`:
- `build_tooiyumeapp.log` / `build_deferredapp.log` / `build_raytracingapp.log`(失败轮原文)
- `rebuild_tooiyumeapp.log` / `rebuild_deferredapp.log` / `rebuild_raytracingapp.log`(删 PCH 后成功轮)
- `configure_fresh.log`、`fullclean_build.log`(全新配置 + 干净构建成功轮)
- `dryrun_tooiyumeapp.log`、`cmake_build_verify.log`

### 附录:本文件中「实际执行」与「推测」的界线

- **实际执行(有原始输出支撑)**:全部环境探测、全部 cmake/ninja 调用及其 exit code、全部产物时间戳与大小、全部报错/警告原文、PCH 头部字节、CMakeCache 与 CMakeLists 的读取内容。
- **推测(已在正文标注)**:PCH 不兼容的**具体成因**(工具集被增量升级导致 c1xx.dll 版本从 .35225 变到 .35229)——「版本号不同故不兼容」是实测事实,但「何时、由谁升级」是我根据版本号与目录残留推断的,未取证。

