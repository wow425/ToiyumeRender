// =============================================================================
// Agility SDK 运行时选择（opt-in）
// =============================================================================
// 【这是什么】
// DirectX 12 从 2020 年起把运行时核心（D3D12Core.dll）从操作系统里拆出来，
// 变成可随应用分发的 NuGet 包 —— 微软把它叫 "Agility SDK"。
// 应用只要在**自己的可执行文件**里导出两个全局符号，D3D12 加载器就会改为
// 从 exe 同目录下的 `D3D12\` 加载这个自带的核心，而不是用 C:\Windows\System32
// 里那个随 Windows 更新停滞不前的旧核心。
//
// 【为什么本项目必须做这件事（本项目实测结论，非通用说教）】
// Win10 19045 自带的 d3d12.dll 是 10.0.19041.5794，其 HighestShaderModel 只到
// 0x65（即 SM 6.5）。同一块 RTX 4060 Ti 在加载 Agility SDK 1.620.2 之后，
// HighestShaderModel 立刻变成 0x66（SM 6.6）。
//   - 未接入前实测：SM66_SUPPORTED=NO / BINDLESS_TIER3=YES
//   - 接入后实测：  SM66_SUPPORTED=YES / SM66_AND_TIER3=YES
// 也就是说：**这是操作系统运行时版本的限制，不是显卡能力的限制**。
// 归档证据见 docs/AI/worklogs/TR-20261121-01_wp-0a-baseline.md §2.2。
//
// 【依赖来自哪里】
// 二进制与头文件全部由 vcpkg 提供，本仓库不再手工维护副本：
//   - vcpkg.json 依赖 "directx12-agility"
//   - 版本钉在 ThirdParty/vcpkg-overlays/directx12-agility/vcpkg.json（1.620.2）
//   - 落地目录 vcpkg_installed/x64-windows/{bin,debug/bin,include}
// 本文件只负责那两个导出符号，与部署逻辑（Engine/20_cmake/AgilitySdk.cmake）分离。
//
// 【DX12 不变量：这两个符号的位置是硬性的】
// 1. 必须定义在**最终 exe** 里（不是静态库里）。若只放进 TooiyumeEngine.lib，
//    链接器看不到任何对它们的引用，很可能根本不把那个 .obj 拉进来，
//    于是符号缺失、Agility SDK 静默失效 —— 表现为"HighestShaderModel 还是 0x65"，
//    而且没有任何报错，极难排查。因此本项目用 CMake 把本文件
//    直接加进每个 exe 的源文件列表（见 Engine/20_cmake/AgilitySdk.cmake 的
//    tooiyume_use_agility_sdk）。
// 2. 必须带 `__declspec(dllexport)`，且名字与类型一字不差：
//    - `const UINT D3D12SDKVersion`
//    - `const char* D3D12SDKPath`
//    加载器是按导出名在 PE 导出表里查的，写错名字 = 静默降级到系统运行时。
// 3. `D3D12SDKVersion` 必须**恰好等于**所分发 D3D12Core.dll 的版本号。
//    版本号对不上时，D3D12 会在 CreateDevice 阶段直接失败
//    （DXGI_ERROR_SDK_COMPONENT_MISSING 一类），而不是悄悄回退。
// 4. `D3D12SDKPath` 是**相对于 exe 所在目录**的路径，且必须以反斜杠结尾。
//    本项目约定 exe 同目录下的 `D3D12\` 子目录（见部署逻辑）。
//
// 【改动本文件时的自检】
//   - 升级 Agility SDK 版本 → 三处必须同时一致：
//       ① 本文件 TY_AGILITY_SDK_VERSION
//       ② ThirdParty/vcpkg-overlays/directx12-agility/vcpkg.json 的 version
//       ③ vcpkg_installed/x64-windows/include/d3d12.h 的 D3D12_SDK_VERSION 宏
//     任一不一致都会出现"能编不能跑"或"能跑但拿不到新接口"。
//   - 若想改成从子目录读（例如 D3D12\620\），改 TY_AGILITY_SDK_PATH 即可，
//     但必须同步改 AgilitySdk.cmake 里的部署目标路径。
// =============================================================================

#include <windows.h>

// 版本号必须与所分发的 D3D12Core.dll 版本一致：1.620.2 -> 620。
// 与 vcpkg_installed/x64-windows/include/d3d12.h 的 D3D12_SDK_VERSION 同值。
//
// 这里刻意用 #define 而不是 `static const UINT kAgilitySdkVersion = 620;`：
// 理由见下方"静态初始化顺序"说明 —— 导出符号必须能被编译器折叠成
// .rdata 里的常量，d3d12.dll 的 DllMain 在 CRT 动态初始化之前就会读它。
// 用宏是零风险写法（字面量直接落到导出数据里，不经过任何变量）。
#define TY_AGILITY_SDK_VERSION 620u

// 相对 exe 所在目录；末尾反斜杠是加载器的要求，不可省。
#define TY_AGILITY_SDK_PATH ".\\D3D12\\"

// -----------------------------------------------------------------------------
// 【踩坑记录 2026-11-21：用中间变量传递 D3D12SDKPath 会让进程在启动阶段崩溃】
//
// 曾经的写法（错误）：
//     static const char* kAgilitySdkPath = ".\\D3D12\\";
//     extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = kAgilitySdkPath; }
//
// 崩溃现象：Demo 一启动就以 0xC0000142（STATUS_DLL_INIT_FAILED）退出，
// 且**日志一行都不写**（连 `Selected GPU` 都没有），因为进程在大
// wWinMain 之前的加载器阶段就死了。用 cdb 挂上去看到真实栈：
//     ntdll!LdrpInitializeProcess
//       -> d3d12!CModule::DllMain
//         -> d3d12!CModule::SetSDKVersion
//           -> d3d12!std::string::assign   <- 读 NullPtr，AV c0000005
//
// 根因（C++ 静态初始化顺序，DX12 不变量）：
//   - `D3D12SDKVersion` 是 `const UINT` + 字面量初始化 → **静态初始化**，
//     值躺在 .rdata 里，加载器一上来就能读到 620。
//   - 而 `= kAgilitySdkPath` 这种「用另一个变量初始化」的写法，编译器认为
//     需要读取内存，归类为**动态初始化**，由 CRT 在 main 之前才执行。
//   - d3d12.dll 的 DllMain 在**更早**的加载器初始化阶段运行，此时
//     D3D12SDKPath 还是零初始化的 nullptr。SetSDKVersion 拿到 nullptr 就直接
//     崩在了 std::string::assign 上。
//   - 结果：版本号读到了（想切 Agility）、路径却是空指针 → 崩。
//
// 修法：直接用字符串字面量初始化导出符号，保证它是静态初始化（常量折叠进 .rdata）。
// 这也是微软官方示例一律写成 `= ".\\D3D12\\"` 字面量的原因 —— 不是风格偏好，
// 而是**必须**，否则这个特性会以最难排查的方式失效。
// -----------------------------------------------------------------------------

// 这两个是 PE 导出符号，必须全局作用域 + extern "C"（不参与 C++ 改名修饰）。
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = TY_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = TY_AGILITY_SDK_PATH;
}
