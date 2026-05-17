cmake_minimum_required(VERSION 3.20)
project(TooiyumeRender VERSION 0.1.0 LANGUAGES CXX C)

# ==========================================
# 全局编译设置：C++20 与 UTF-8
# ==========================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 强制 MSVC 编译器使用 UTF-8 编码，并开启 Unicode 字符集支持
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(UNICODE _UNICODE) # <--- 新增这一行
endif()

# ==========================================
# Vcpkg 依赖管理
# ==========================================
# 注意：确保在配置 CMake 时传入了 -DCMAKE_TOOLCHAIN_FILE=[vcpkg_root]/scripts/buildsystems/vcpkg.cmake
find_package(directxtex CONFIG REQUIRED)
find_package(directxmesh CONFIG REQUIRED)
find_package(DirectX-Headers CONFIG REQUIRED)
# directx-dxc 通常会被 vcpkg 下载到环境路径中，稍后可以在 Engine 中直接链接 dxcompiler

# ==========================================
# 添加子目录
# ==========================================
add_subdirectory(Engine)
add_subdirectory(Demos/ForwardApp)
add_subdirectory(Demos/DeferredApp)
