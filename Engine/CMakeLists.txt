# ==========================================
# 收集 Engine 下所有的源码文件
# ==========================================
# 递归获取所有 .cpp 和 .h 文件（方便起见使用 GLOB_RECURSE）
file(GLOB_RECURSE ENGINE_SOURCES
    "*.cpp"
    "*.h"
    "*.hpp"
)

# ==========================================
# Engine 静态库
# ==========================================
add_library(TooiyumeEngine STATIC ${ENGINE_SOURCES}
        04_Renderer/TemporalEffects.cpp
        04_Renderer/TemporalEffects.h)

# ==========================================
# Shader 编译
# ==========================================
# R0：01_Default 是 ForwardRenderer 真正使用的 HLSL 组，但此前从未进入 CMake。
# 后果：仓库里提交的 Engine/Shader/01_Default/DefaultVS.h / DefaultPS.h 是
# 某次手工导出的旧字节码，改了 HLSL 不会重编译，Forward 永远跑旧的。
# 这里把两者纳入正规编译链，产物落到 ${CMAKE_BINARY_DIR}/ShaderCache。
compile_shader(
    DefaultVS
    Shader/01_Default/DefaultVS.hlsl
    MainVS
    vs_6_4
)

compile_shader(
    DefaultPS
    Shader/01_Default/DefaultPS.hlsl
    MainPS
    ps_6_4
)

compile_shader(
    StaticMeshVS
    Shader/02_Deferred/StaticMeshVS.hlsl
    MainVS
    vs_6_4
)

compile_shader(
    GBufferPS
    Shader/02_Deferred/GBufferPS.hlsl
    MainPS
    ps_6_4
)

compile_shader(
    FullScreenVS
    Shader/02_Deferred/FullScreenVS.hlsl
    MainVS
    vs_6_4
)

compile_shader(
    LightingPS
    Shader/02_Deferred/LightingPS.hlsl
    MainPS
    ps_6_4
)

compile_shader(
    SkyboxVS
    Shader/06_Environment/SkyboxVS.hlsl
    MainVS
    vs_6_4
)

compile_shader(
    SkyboxPS
    Shader/06_Environment/SkyboxPS.hlsl
    MainPS
    ps_6_4
)

compile_shader(
    EquirectangularToCubemapCS
   Shader/06_Environment/EquirectangularToCubemapCS.hlsl
   MainCS
   cs_6_4
)

compile_shader(
   IrradianceConvolutionCS
   Shader/06_Environment/IrradianceConvolutionCS.hlsl
   MainCS
   cs_6_4
)

compile_shader(
   SpecularPrefilterCS
   Shader/06_Environment/SpecularPrefilterCS.hlsl
   MainCS
   cs_6_4
)

compile_shader(
   BRDFLUTCS
    Shader/06_Environment/BRDFLUTCS.hlsl
    MainCS
   cs_6_4
)

# ==========================================
# 链接Shader
# ==========================================
add_dependencies(TooiyumeEngine
    DefaultVS_Shader
    DefaultPS_Shader
    StaticMeshVS_Shader
    GBufferPS_Shader
   FullScreenVS_Shader
   LightingPS_Shader
   SkyboxVS_Shader
   SkyboxPS_Shader
   EquirectangularToCubemapCS_Shader
  IrradianceConvolutionCS_Shader
  SpecularPrefilterCS_Shader
  BRDFLUTCS_Shader
)

# R0：ShaderCache 必须是 PUBLIC include，且排在源码目录之前。
# 原因：仓库里残留着旧版 `Engine/Shader/01_Default/DefaultVS.h`（预编译字节码头）
# 和构建目录生成的同名头。若源码目录优先，编译器会静默选中旧字节码，
# 导致「改了 HLSL 却不生效」。把 ShaderCache 放在最前面可以强制优先命中生成头。
target_include_directories(TooiyumeEngine PUBLIC
    "${CMAKE_BINARY_DIR}/ShaderCache"
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==========================================================
# 生成头 → 源文件 的编译依赖
# ----------------------------------------------------------
# 把每个生成头挂到**真正 include 它的 .cpp** 上，
# 否则改 HLSL 只会更新 .cso/.h，不会触发 .cpp 重编，
# exe 里保留的仍是旧字节码数组（实测过的真实故障）。
# ==========================================================
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Renderer/Deferred/DeferredRenderer.cpp"
    "${CMAKE_BINARY_DIR}/ShaderCache/StaticMeshVS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/GBufferPS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/FullScreenVS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/LightingPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Renderer/Forward/ForwardRenderer.cpp"
    "${CMAKE_BINARY_DIR}/ShaderCache/DefaultVS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/DefaultPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/EnvironmentLightingManager.cpp"
    "${CMAKE_BINARY_DIR}/ShaderCache/SkyboxVS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/SkyboxPS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/EquirectangularToCubemapCS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/IrradianceConvolutionCS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/SpecularPrefilterCS.h"
    "${CMAKE_BINARY_DIR}/ShaderCache/BRDFLUTCS.h"
)
# ==========================================
# PCH (Precompiled Header)
# ==========================================
target_precompile_headers(TooiyumeEngine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/00_Core/PCH.h"
)

# ==========================================
# R0：RELEASE 宏
# ==========================================
# 源码里有 13 处 `#ifdef RELEASE` / `#ifndef RELEASE` 分支（断言、Debug Layer、
# 资源命名等），但此前从未有任何构建配置真正定义过 RELEASE，
# 因此 Release 构建仍会走 Debug 分支。这里按 build type 补齐。
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    target_compile_definitions(TooiyumeEngine PUBLIC RELEASE=1)
    message(STATUS "TooiyumeRender: RELEASE 宏已启用 (CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})")
else()
    message(STATUS "TooiyumeRender: Debug 构建 (CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})")
endif()

# ==========================================
# R0.2 资源根兜底
# ==========================================
# AssetRoot 的运行时解析顺序是：命令行 -AssetsRoot > TOOIYUME_ASSETS_ROOT 环境变量
# > exe 目录上溯 > 工作目录上溯 > 本编译期默认值。
# 这里注入源码树里的 Assets 绝对路径，仅作为最后兜底，不参与正常路径决策。
target_compile_definitions(TooiyumeEngine PUBLIC
    TOOIYUME_SOURCE_ASSETS_DIR=L"${CMAKE_SOURCE_DIR}/Assets"
)


# ==========================================
# 本地第三方库：PIX (从根目录的 ThirdParty 中引入)
# ==========================================
# R0：PIX DLL 策略说明。
# 原先只把 ThirdParty/PIX/lib 加入链接搜索路径，DLL 能否找到完全取决于
# 开发机 PATH 或手工拷到 exe 目录 —— 换机器就是「exe 起不来，缺 DLL」。
# 现在明确规定：DLL 由本仓库 ThirdParty/PIX/bin 分发，并在构建后用
# 一个 POST_BUILD 步骤复制到每个 Demo 的输出目录，不依赖系统 PATH。
set(PIX_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/PIX")
set(PIX_DLL "${PIX_DIR}/bin/WinPixEventRuntime.dll")
set(PIX_LIB "${PIX_DIR}/lib/WinPixEventRuntime.lib")

if(NOT EXISTS "${PIX_LIB}")
    message(FATAL_ERROR
        "缺少 PIX 导入库：${PIX_LIB}\n"
        "ThirdParty/PIX 应当随仓库一并分发（lib + include + bin）。")
endif()
if(NOT EXISTS "${PIX_DLL}")
    message(WARNING
        "缺少 PIX 运行库：${PIX_DLL}\n"
        "构建仍可进行，但运行时会因缺少 DLL 而启动失败。")
endif()

# 包含 PIX 的头文件
target_include_directories(TooiyumeEngine PUBLIC "${PIX_DIR}/include")

# 链接 PIX 的静态库 (假设您开发的是 64 位程序)
target_link_directories(TooiyumeEngine PUBLIC "${PIX_DIR}/lib")

# 把 DLL 复制到所有消费方（3 个 Demo）的 exe 同目录。
# 用 INTERFACE 属性而不是逐个 add_custom_command，新增 Demo 时自动生效。
# TooiyumeEngine 是静态库，没有自己的输出目录，因此只对可执行目标复制。
foreach(_demo_target TooiyumeApp DeferredApp RayTracingApp RendererDemoApp)
    if(TARGET ${_demo_target})
        add_custom_command(TARGET ${_demo_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PIX_DLL}" "$<TARGET_FILE_DIR:${_demo_target}>"
            COMMENT "复制 PIX 运行库到 ${_demo_target} 输出目录"
        )
    endif()
endforeach()

# ==========================================
# 链接所有依赖项
# ==========================================
target_link_libraries(TooiyumeEngine PUBLIC
    imgui                 # R3：Dear ImGui 静态库（include 目录与 d3d12/dxgi/user32/gdi32/imm32/dwmapi 都是 PUBLIC 依赖）
    Microsoft::DirectXTex
    Microsoft::DirectXMesh
    Microsoft::DirectX-Headers
    WinPixEventRuntime    # PIX 的导入库
    dxcompiler            # vcpkg 安装的 DXC 库
    d3d12                 # Windows SDK 自带
    dxgi
    dxguid
)

# 说明：运行时 DLL 部署**不在这里**做。
# Engine 的 add_subdirectory 早于 Demos，此刻各 Demo target 尚未定义，
# 任何 `if(TARGET ...)` 都会静默失败。统一部署见
# Engine/20_cmake/DeployRuntimeDlls.cmake（由根 CMakeLists 在所有
# add_subdirectory 之后 include）。

