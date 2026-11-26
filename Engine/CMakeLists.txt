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

# 产物落 out/<Config>/Engine/（镜像源码层次，见 Engine/20_cmake/OutputLayout.cmake）
tooiyume_set_output_dirs(TooiyumeEngine "Engine")

# ==========================================
# Shader 编译
# ==========================================
# R0：01_Default 是 ForwardRenderer 真正使用的 HLSL 组，但此前从未进入 CMake。
# 后果：仓库里提交的 Engine/Shader/01_Default/DefaultVS.h / DefaultPS.h 是
# 某次手工导出的旧字节码，改了 HLSL 不会重编译，Forward 永远跑旧的。
# 这里把两者纳入正规编译链，产物落到
# ${TOOIYUME_SHADER_OUT_DIR}/01_Default/（即 out/<Config>/Engine/Shader/01_Default/）。
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

# [WP-2C / 契约 5] 全管线唯一共享 TonemapPass 着色器三件套（1/3：SM 6.4 回退路径）
compile_shader(
    TonemapPS
    Shader/04_PostProcess/TonemapPS.hlsl
    MainPS
    ps_6_4
)

# [WP-3A / 契约 7.1] SM 6.6 Bindless 版 Tonemap 着色器三件套（1/3：-HV 2021 -enable-16bit-types）
compile_shader_bindless(
    TonemapBindlessPS
    Shader/04_PostProcess/TonemapBindlessPS.hlsl
    MainPS
    ps_6_6
)

# [WP-3C] 延迟管线 ShadowMap / CSM + SM 6.6 Bindless Lighting + SSAO + TAA 着色器三件套（1/3）
compile_shader(
    ShadowDepthVS
    Shader/03_Shadow/ShadowDepthVS.hlsl
    MainVS
    vs_6_4
)

compile_shader(
    ShadowDepthAlphaTestPS
    Shader/03_Shadow/ShadowDepthAlphaTestPS.hlsl
    MainPS
    ps_6_4
)

compile_shader_bindless(
    LightingBindlessPS
    Shader/02_Deferred/LightingBindlessPS.hlsl
    MainPS
    ps_6_6
)

compile_shader_bindless(
    SSAOComputeCS
    Shader/04_PostProcess/SSAOComputeCS.hlsl
    MainCS
    cs_6_6
)

compile_shader_bindless(
    SSAOBlurCS
    Shader/04_PostProcess/SSAOBlurCS.hlsl
    MainCS
    cs_6_6
)

compile_shader_bindless(
    TAABindlessCS
    Shader/04_PostProcess/TAABindlessCS.hlsl
    MainCS
    cs_6_6
)

# ==========================================
# R5 / D1：DXR 光追库（DXIL Library）
# ==========================================
# 与上面 12 个 compile_shader 的区别：lib profile **不带 -E**，
# 一份文件里的 RayGen / Miss / ClosestHit / AnyHit 全部编进同一个 DXIL 库，
# 再由 State Object 的 DXIL_LIBRARY 子对象按导出名挑选。
# 产物同样是 ${TOOIYUME_SHADER_OUT_DIR}/07_RayTracing/RayTracing.{cso,h}
# （即 out/<Config>/Engine/Shader/07_RayTracing/）。
compile_raytracing_library(
    RayTracing
    Shader/07_RayTracing/RayTracing.hlsl
)

# [契约 4] DXR 1.1 Inline RayQuery 主表面几何提取着色器三件套（1/3：SM 6.6 Bindless）
compile_shader_bindless(
    PrimaryRayCS
    Shader/07_RayTracing/PrimaryRayCS.hlsl
    MainCS
    cs_6_6
)

# ==========================================
# 链接Shader
# ==========================================
# [GFL2 / M1] 少前2 AK-12 角色 Bindless 着色器 (SM 6.6 + UE5 Row-Major + Reverse-Z)
compile_shader_bindless(
    CharacterOpaqueVS
    Shader/08_Character/CharacterOpaqueVS.hlsl
    MainVS
    vs_6_6
)

compile_shader_bindless(
    CharacterBaseColorPS
    Shader/08_Character/CharacterBaseColorPS.hlsl
    MainPS
    ps_6_6
)

compile_shader_bindless(
    CharacterBodyPS
    Shader/08_Character/CharacterBodyPS.hlsl
    MainPS
    ps_6_6
)

compile_shader_bindless(
    GFL2ShadowDepthVS
    Shader/08_Character/GFL2ShadowDepthVS.hlsl
    MainVS
    vs_6_6
)

compile_shader_bindless(
    GFL2DebugViewPS
    Shader/08_Character/GFL2DebugViewPS.hlsl
    MainPS
    ps_6_6
)

# [GFL2 / M4-M6] 面发眼 / 描边 / 半透明 / 整备室环境 / 后处理 Bindless 着色器
compile_shader_bindless(
    CharacterFacePS
    Shader/08_Character/CharacterFacePS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    CharacterHairPS
    Shader/08_Character/CharacterHairPS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    CharacterBangsShadowVS
    Shader/08_Character/CharacterBangsShadowVS.hlsl
    MainVS
    vs_6_6
)
compile_shader_bindless(
    CharacterEyePS
    Shader/08_Character/CharacterEyePS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    CharacterOutlineVS
    Shader/08_Character/CharacterOutlineVS.hlsl
    MainVS
    vs_6_6
)
compile_shader_bindless(
    CharacterOutlinePS
    Shader/08_Character/CharacterOutlinePS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    CharacterTransparentPS
    Shader/08_Character/CharacterTransparentPS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    EnvScenePS
    Shader/08_Character/EnvScenePS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    EnvSkyboxPS
    Shader/08_Character/EnvSkyboxPS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    FloorPlanarReflectionPS
    Shader/08_Character/FloorPlanarReflectionPS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    GFL2SSAOCS
    Shader/08_Character/GFL2SSAOCS.hlsl
    MainCS
    cs_6_6
)
compile_shader_bindless(
    GFL2SSAOCompositePS
    Shader/08_Character/GFL2SSAOCompositePS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    GFL2TAACS
    Shader/08_Character/GFL2TAACS.hlsl
    MainCS
    cs_6_6
)
compile_shader_bindless(
    GFL2BloomPS
    Shader/08_Character/GFL2BloomPS.hlsl
    MainPS
    ps_6_6
)
compile_shader_bindless(
    GFL2UberPostLUTPS
    Shader/08_Character/GFL2UberPostLUTPS.hlsl
    MainPS
    ps_6_6
)

add_dependencies(TooiyumeEngine
    CharacterOpaqueVS_Shader
    CharacterBaseColorPS_Shader
    CharacterBodyPS_Shader
    GFL2ShadowDepthVS_Shader
    GFL2DebugViewPS_Shader
    CharacterFacePS_Shader
    CharacterHairPS_Shader
    CharacterBangsShadowVS_Shader
    CharacterEyePS_Shader
    CharacterOutlineVS_Shader
    CharacterOutlinePS_Shader
    CharacterTransparentPS_Shader
    EnvScenePS_Shader
    EnvSkyboxPS_Shader
    FloorPlanarReflectionPS_Shader
    GFL2SSAOCS_Shader
    GFL2SSAOCompositePS_Shader
    GFL2TAACS_Shader
    GFL2BloomPS_Shader
    GFL2UberPostLUTPS_Shader
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
  TonemapPS_Shader
  TonemapBindlessPS_Shader
  ShadowDepthVS_Shader
  ShadowDepthAlphaTestPS_Shader
  LightingBindlessPS_Shader
  SSAOComputeCS_Shader
  SSAOBlurCS_Shader
  TAABindlessCS_Shader
  RayTracing_Shader
  PrimaryRayCS_Shader
)

# R0：生成头目录必须是 PUBLIC include，且排在源码目录之前。
# 原因：仓库里残留着旧版 `Engine/Shader/01_Default/DefaultVS.h`（预编译字节码头）
# 和构建目录生成的同名头。若源码目录优先，编译器会静默选中旧字节码，
# 导致「改了 HLSL 却不生效」。把生成头根目录放在最前面可以强制优先命中生成头。
#
# 注意路径变了：生成头不再平铺在 <build>/ShaderCache，而是按 HLSL 子目录
# 镜像到 out/<Config>/Engine/Shader/<子目录>/，因此 C++ 侧的 include
# 必须带子目录前缀，例如 #include "02_Deferred/GBufferPS.h"。
target_include_directories(TooiyumeEngine PUBLIC
    "${TOOIYUME_SHADER_OUT_DIR}"
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==========================================================
# Agility SDK 头文件（PUBLIC，让所有消费方看到同一份 d3d12.h）
# ----------------------------------------------------------
# 为什么 Engine 也要注入：
#   Engine 内部（00_Core/PCH.h:30 及其下游）直接写 `#include <d3d12.h>`。
#   若只有 Demo 看到 Agility 的新头、Engine 仍吃 Windows SDK 的旧头，
#   同一个结构体在两边会有不同定义 → ODR 违规 → Debug 下内存错乱。
#   必须整棵树用同一份头。
# 为什么 BEFORE + SYSTEM：
#   BEFORE 保证优先于 Windows SDK 的 um\ 目录；
#   SYSTEM 让新版头里的第三方宏/警告不污染 /W4 构建。
# ==========================================================
tooiyume_agility_include(TooiyumeEngine)

# ==========================================================
# 生成头 → 源文件 的编译依赖
# ----------------------------------------------------------
# 把每个生成头挂到**真正 include 它的 .cpp** 上，
# 否则改 HLSL 只会更新 .cso/.h，不会触发 .cpp 重编，
# exe 里保留的仍是旧字节码数组（实测过的真实故障）。
# ==========================================================
# [WP-2B] 各着色器字节码头已按单 TU 唯一所有权收口到对应 Pass 实现文件
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/GBufferPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/StaticMeshVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/GBufferPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/LightingPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/FullScreenVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/LightingPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/LightingBindlessPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/ShadowPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/03_Shadow/ShadowDepthVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/03_Shadow/ShadowDepthAlphaTestPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/SSAOPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/04_PostProcess/SSAOComputeCS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/04_PostProcess/SSAOBlurCS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/TAAPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/04_PostProcess/TAABindlessCS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/SkyboxPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/SkyboxVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/SkyboxPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/ForwardMainPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/01_Default/DefaultVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/01_Default/DefaultPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/EnvironmentLightingManager.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/EquirectangularToCubemapCS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/IrradianceConvolutionCS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/SpecularPrefilterCS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/06_Environment/BRDFLUTCS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/TonemapPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/FullScreenVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/04_PostProcess/TonemapPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/04_PostProcess/TonemapBindlessPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/RayTracingMainPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/07_RayTracing/RayTracing.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/PrimaryRayPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/07_RayTracing/PrimaryRayCS.h"
)
# ==========================================
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Character/CharacterOpaquePass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOpaqueVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterBodyPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Common/GFL2ShadowPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2ShadowDepthVS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Common/GFL2DebugOverlayPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/FullScreenVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2DebugViewPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Character/CharacterFaceHairEyePass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOpaqueVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterBangsShadowVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterFacePS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterHairPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterEyePS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Character/CharacterOutlinePass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOutlineVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOutlinePS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Character/CharacterTransparentPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOpaqueVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterTransparentPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/Common/GFL2EnvScenePass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterOpaqueVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/EnvScenePS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/EnvSkyboxPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/CharacterBodyPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/FloorPlanarReflectionPS.h"
)
shader_attach_header_dependency(
    "${CMAKE_CURRENT_SOURCE_DIR}/04_Renderer/Passes/PostProcess/GFL2PostProcessPass.cpp"
    "${TOOIYUME_SHADER_OUT_DIR}/02_Deferred/FullScreenVS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2SSAOCS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2SSAOCompositePS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2TAACS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2BloomPS.h"
    "${TOOIYUME_SHADER_OUT_DIR}/08_Character/GFL2UberPostLUTPS.h"
)
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
    TOOIYUME_SOURCE_ROOT_DIR=L"${CMAKE_SOURCE_DIR}"
)


# ==========================================
# 第三方库：全部来自 vcpkg manifest（见仓库根 vcpkg.json）
# ==========================================
# 历史：B0 时代 PIX 与 imgui 是手工拷进 ThirdParty/ 的裸副本，
# 靠 set(PIX_DIR ...)/add_subdirectory(ThirdParty/imgui) 引入，
# 并用一大段 foreach 手工 copy DLL。问题：
#   1. 版本无记录（PIX 是 1.0.240308001 还是别的，只能看二进制属性）；
#   2. 没有哈希校验，换机器要人肉拷；
#   3. 手工 copy 漏掉某个 Demo 就表现为「exe 起不来，缺 DLL」。
# 现在统一由 vcpkg 提供：
#   winpixevent          -> Microsoft::WinPixEventRuntime（带 INTERFACE_INCLUDE_DIRECTORIES）
#   imgui[dx12,win32]    -> imgui::imgui
#   directx12-agility    -> Microsoft::DirectX12-Agility（overlay 钉 1.620.2）
# vcpkg 的 *-config.cmake 会自动把 DLL 拷到 $<TARGET_FILE_DIR:...>（applocal），
# 因此**不再需要**任何手工 POST_BUILD 复制逻辑。这也是本次迁移的主要收益。
if(NOT TARGET Microsoft::WinPixEventRuntime)
    message(FATAL_ERROR
        "缺少 winpixevent。请先运行：\n"
        "  F:/VS/rep/vcpkg/vcpkg.exe install --triplet x64-windows "
        "--overlay-ports=ThirdParty/vcpkg-overlays\n"
        "或在 CMake 配置时指定 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

# ==========================================
# 链接所有依赖项
# ==========================================
target_link_libraries(TooiyumeEngine PUBLIC
    imgui::imgui          # R3：Dear ImGui 静态库（vcpkg 版，含 dx12/win32 后端）
    Microsoft::DirectXTex
    Microsoft::DirectXMesh
    Microsoft::DirectX-Headers
    Microsoft::WinPixEventRuntime  # PIX 导入库 + 头（vcpkg 版）
    Microsoft::DirectX12-Agility   # 620 头路径（DLL 由各 Demo 的 tooiyume_use_agility_sdk 部署）
    dxcompiler            # vcpkg 安装的 DXC 库
    d3d12                 # Windows SDK 自带（import lib；运行时换成 Agility 的）
    dxgi
    dxguid
)

# 说明：运行时 DLL 部署**不在这里**做。
# Engine 的 add_subdirectory 早于 Demos，此刻各 Demo target 尚未定义，
# 任何 `if(TARGET ...)` 都会静默失败。统一部署见
# Engine/20_cmake/DeployRuntimeDlls.cmake（由根 CMakeLists 在所有
# add_subdirectory 之后 include）。

