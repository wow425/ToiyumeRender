cmake_minimum_required(VERSION 3.20)
project(TooiyumeRender VERSION 0.1.0 LANGUAGES CXX C)

# ==========================================
# Shader编译
# ==========================================
include(${CMAKE_SOURCE_DIR}/Engine/20_cmake/ShaderCompiler.cmake)

# ==========================================
# 全局编译设置：C++20 与 UTF-8
# ==========================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 强制编译器使用 UTF-8 编码，并开启 Unicode 字符集支持
    add_compile_options(/utf-8)
    add_compile_definitions(UNICODE _UNICODE) # <--- 新增这一行

# ==========================================
# 构建类型：单配置生成器（Ninja）必须显式给出
# ==========================================
# 之前 CMakePresets 里写死了 CMAKE_BUILD_TYPE，直接用 cmake -S . -B dir 时
# 会得到空 build type，进而导致 Debug/Release 分支（shader flags、RELEASE 宏）
# 全部落空。这里补一个明确默认值，并限制为已知取值。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug/Release)" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

# ==========================================
# 第三方依赖定位（R4.1：解除对 vcpkg toolchain 的硬依赖）
# ==========================================
# 背景：原实现要求先设 VCPKG_ROOT 环境变量、再走 vcpkg 的 toolchain 文件。
# 这在 Visual Studio 里是致命的：VS 的 CMake 集成不读 shell 的 set 命令，
# 于是"在 VS 里打开项目就无法配置"。而且 toolchain 会把清单重新解析一遍，
# 把依赖装到 <binaryDir>/vcpkg_installed，与本仓库根目录已有的
# vcpkg_installed/x64-windows 脱节。
#
# 现在改为双路径，优先级从高到低：
#   A. 若调用方显式给了 CMAKE_TOOLCHAIN_FILE（或设了 VCPKG_ROOT）：
#      尊重它，走标准 vcpkg manifest 流程（命令行/CI 用）。
#   B. 否则：直接把仓库内的 vcpkg_installed/x64-windows 加进
#      CMAKE_PREFIX_PATH。包的 *-config.cmake 已经用相对路径（_IMPORT_PREFIX）
#      定位自身，因此不需要 toolchain 也能正常工作。
#
# 重要：B 路径**不会**自动安装缺失的包，缺包时给出可操作提示而不是让
# find_package 抛出十几行 "not found"。已验证：完全无环境变量时，
# 仅靠 CMAKE_PREFIX_PATH 即可解析 directxtex / directxmesh / DirectX-Headers。
set(TOOIYUME_VCPKG_INSTALLED_DIR
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows"
    CACHE PATH "仓库内置的 vcpkg 安装目录（B 路径使用）")

# 判定仓库内置依赖是否**真的可用**（存在 directxtex 的 config 文件）：
# 只判目录存在是不够的 —— 空目录、或只有部分包都会让 find_package 失败。
set(_TOOI_REPO_DEPS_OK FALSE)
if(EXISTS "${TOOIYUME_VCPKG_INSTALLED_DIR}/share/directxtex")
    set(_TOOI_REPO_DEPS_OK TRUE)
endif()

if(_TOOI_REPO_DEPS_OK)
    # ── B 路径优先（2026-10-27 修正优先级）──────────────────────────────
    # 原来 B 路径排在 VCPKG_ROOT 之后，导致「Visual Studio 打开文件夹」必然失败：
    # VS 的 CMake 集成**总会**把 VCPKG_ROOT 注入成它自带的
    #   F:\VS\Community\VC\vcpkg
    # 而那个 vcpkg 的 installed/ 是空的（它只是个未初始化的壳），
    # 于是走到 A 路径、加载一个空 toolchain，find_package(directxtex) 直接报
    #   Could not find a package configuration file provided by "directxtex"。
    #
    # 现在区分两种 toolchain：
    #   - 用户/脚本显式指定（如 scripts/build.bat 传的 F:/VS/rep/vcpkg）→ 尊重它
    #   - VS 自动注入的空壳 vcpkg（路径含 /VC/vcpkg）→ 忽略并清除
    if(DEFINED CMAKE_TOOLCHAIN_FILE
       AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL ""
       AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\][Vv][Cc][/\\\\]vcpkg[/\\\\]")
        message(STATUS "[deps] 尊重调用方显式指定的 toolchain: ${CMAKE_TOOLCHAIN_FILE}")
    else()
        if(DEFINED CACHE{CMAKE_TOOLCHAIN_FILE})
            get_property(_tc CACHE CMAKE_TOOLCHAIN_FILE PROPERTY VALUE)
            message(STATUS "[deps] 忽略 VS 注入的空壳 vcpkg toolchain: ${_tc}")
            unset(CMAKE_TOOLCHAIN_FILE CACHE)
        endif()
        list(APPEND CMAKE_PREFIX_PATH "${TOOIYUME_VCPKG_INSTALLED_DIR}")
        message(STATUS "[deps] 使用仓库内依赖目录（优先于 VCPKG_ROOT）: ${TOOIYUME_VCPKG_INSTALLED_DIR}")
    endif()
elseif(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
    message(STATUS "[deps] 使用调用方指定的 toolchain 文件: ${CMAKE_TOOLCHAIN_FILE}")
elseif(DEFINED VCPKG_ROOT AND NOT VCPKG_ROOT STREQUAL "")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file" FORCE)
    message(STATUS "[deps] 由 VCPKG_ROOT 推导 toolchain: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(FATAL_ERROR
        "找不到第三方依赖，且没有可用的 vcpkg：\n"
        "  仓库内置目录不存在或无内容：${TOOIYUME_VCPKG_INSTALLED_DIR}\n"
        "  VCPKG_ROOT 也未设置。\n"
        "请任选其一：\n"
        "  1) 把 vcpkg 的 x64-windows 安装结果放到该目录（推荐，无需联网）\n"
        "  2) 设 VCPKG_ROOT 让 CMake 走标准 vcpkg manifest 流程\n"
        "  3) cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

find_package(directxtex CONFIG REQUIRED)
find_package(directxmesh CONFIG REQUIRED)
find_package(DirectX-Headers CONFIG REQUIRED)

# R4.1：运行时 DLL 部署函数。定义在 Engine/20_cmake 下，供各 Demo 在
# 自己的 CMakeLists 里调用（add_custom_command(TARGET) 要求同目录）。
include(${CMAKE_SOURCE_DIR}/Engine/20_cmake/DeployRuntimeDlls.cmake)
# directx-dxc 通常会被 vcpkg 下载到环境路径中，稍后可以在 Engine 中直接链接 dxcompiler

# ==========================================
# 添加子目录
# ==========================================
# 三方库先于 Engine / Demos 加入：
#   - Demos（真正 link imgui 的消费方）在最后，顺序天然满足；
#   - 放在 Engine 之前是为了让 Engine 里任何以后想 include 到 imgui 的目标
#     在配置阶段就能看到 `imgui` 这个目标名（target 是全局可见的，
#     但先定义先可用，避免「先 link 后定义」这种隐式依赖链）。
add_subdirectory(ThirdParty/imgui)

add_subdirectory(Engine)
add_subdirectory(Demos/ForwardApp)
add_subdirectory(Demos/DeferredApp)
add_subdirectory(Demos/RayTracingApp)
# R3：统一入口 Demo（RendererRegistry 运行时切换 + ImGui）
add_subdirectory(Demos/RendererDemoApp)
