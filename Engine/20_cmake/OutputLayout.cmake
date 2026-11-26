# ==========================================================
# 产物输出布局（统一约定）
# ----------------------------------------------------------
# 目标：所有**可交付产物**（exe / lib / dll / cso / 生成头）统一落在
#   <repo>/out/<Config>/...
# 并按**原文件的源码层次**镜像，而不是散落在各 target 的默认目录里。
#
# 为什么要把「构建树」和「产物树」分开：
#   - out/build/<Config>/  是 CMake 自己的地盘（CMakeCache、ninja 文件、
#     CMakeFiles/ 中间对象）。这些东西位置由 -B 决定，不进版本库、也不需要人读。
#   - out/<Config>/        是人的地盘：exe 在哪、lib 在哪、生成的头在哪，
#     一眼能按源码目录结构找到，方便调试和排查。
#
# 镜像规则（相对路径 = 源码里的相对路径）：
#   Engine/CMakeLists.txt          -> out/<Config>/Engine/TooiyumeEngine.lib
#   Demos/DeferredApp/CMakeLists   -> out/<Config>/Demos/DeferredApp/DeferredApp.exe (+ 运行期 DLL)
#   Engine/Shader/02_Deferred/GBufferPS.hlsl
#                                  -> out/<Config>/Engine/Shader/02_Deferred/GBufferPS.cso
#                                     out/<Config>/Engine/Shader/02_Deferred/GBufferPS.h
#
# 注意：本文件只定义**变量与函数**，不修改任何 target；
# 具体 target 由各处 CMakeLists 显式调用 tooiyume_set_output_dirs()。
# ==========================================================

if(NOT DEFINED TOOIYUME_OUT_ROOT)
    set(TOOIYUME_OUT_ROOT "${CMAKE_SOURCE_DIR}/out/build"
        CACHE PATH "所有构建产物的根目录")
endif()

# 单配置生成器（Ninja）下 CMAKE_BUILD_TYPE 在配置期就是确定的；
# 多配置生成器（VS）下 CMAKE_BUILD_TYPE 为空，此时输出目录用生成器表达式
# （见函数里的 $<CONFIG> 分支），这里只给缓存/诊断用的展示值。
if(CMAKE_BUILD_TYPE)
    set(TOOIYUME_CONFIG_FOR_SHADER "${CMAKE_BUILD_TYPE}")
else()
    set(TOOIYUME_CONFIG_FOR_SHADER "Debug")
endif()

set(TOOIYUME_OUT_DIR "${TOOIYUME_OUT_ROOT}/${TOOIYUME_CONFIG_FOR_SHADER}"
    CACHE PATH "当前配置的产物目录（out/<Config>）")

# 生成的头 / cso 的根。注意它必须与源码里的 Engine/Shader/<子目录> 镜像，
# 因此 C++ 侧的 include 要写成 #include "02_Deferred/GBufferPS.h"。
set(TOOIYUME_SHADER_OUT_DIR "${TOOIYUME_OUT_DIR}/Engine/Shader"
    CACHE PATH "着色器产物根目录（按 HLSL 子目录镜像）")

message(STATUS "[layout] 产物根目录: ${TOOIYUME_OUT_DIR}")
message(STATUS "[layout] 着色器产物: ${TOOIYUME_SHADER_OUT_DIR}")

# ==========================================================
# tooiyume_set_output_dirs(<target> <相对目录>)
# ----------------------------------------------------------
# 把 target 的 exe/lib/dll/pdb 全部指到 ${TOOIYUME_OUT_DIR}/<相对目录>。
#
# 为什么同时设置 <TYPE>_OUTPUT_DIRECTORY 与 <TYPE>_OUTPUT_DIRECTORY_<CONFIG>：
#   单配置生成器（Ninja）只读前者，多配置生成器（VS）优先读后者。
#   两边都设，两种生成器得到一致结果，不依赖调用方记得区分。
#
# 为什么不设 OBJECT 输出目录：
#   .obj 属于构建树的中间文件，留在 out/build/<Config>/CMakeFiles 下由
#   CMake 自己管理；挪出来会让增量构建和 ninja 依赖图更容易出错。
# ==========================================================
function(tooiyume_set_output_dirs target relative_dir)
    if(NOT TARGET ${target})
        message(WARNING "tooiyume_set_output_dirs: target ${target} 不存在，跳过。")
        return()
    endif()

    set(_dir "${TOOIYUME_OUT_DIR}/${relative_dir}")

    foreach(_kind ARCHIVE LIBRARY RUNTIME)
        set_target_properties(${target} PROPERTIES
            ${_kind}_OUTPUT_DIRECTORY "${_dir}"
        )
        foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_target_properties(${target} PROPERTIES
                ${_kind}_OUTPUT_DIRECTORY_${_cfg} "${_dir}"
            )
        endforeach()
    endforeach()

    # 仅重定向链接器 PDB（与 exe/lib 同目录方便定位崩溃栈）；
    # 绝不重定向 COMPILE_PDB_OUTPUT_DIRECTORY（vc140.pdb 属于编译器中间产物，
    # 必须与 .obj / cmake_pch.cxx.pch 一同留在 CMakeFiles/<target>.dir 下，
    # 否则清理或切换 VS/CLI 构建时极易触发 MSVC C2859 预编译头 PDB 不匹配错误）。
    set_target_properties(${target} PROPERTIES
        PDB_OUTPUT_DIRECTORY "${_dir}"
    )
    foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES
            PDB_OUTPUT_DIRECTORY_${_cfg} "${_dir}"
        )
    endforeach()
endfunction()
