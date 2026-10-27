
# ==========================================
# DX12 Shader Compiler Utilities
# ==========================================

find_program(DXC_EXECUTABLE
    NAMES dxc
    PATHS
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/tools/directx-dxc"
    REQUIRED
)

# ==========================================
# compile_shader(
#     TARGET_NAME
#     SHADER_FILE
#     ENTRY
#     PROFILE
# )
#
# Example:
# compile_shader(
#     StaticMeshVS
#     Shader/Deferred/StaticMeshVS.hlsl
#     MainVS
#     vs_6_4
# )
# ==========================================

function(compile_shader TARGET_NAME SHADER_FILE ENTRY PROFILE)

    get_filename_component(FILE_WE ${SHADER_FILE} NAME_WE)

    set(SHADER_SOURCE "${CMAKE_SOURCE_DIR}/Engine/${SHADER_FILE}")

    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/ShaderCache")

    file(MAKE_DIRECTORY ${OUTPUT_DIR})

    # ==========================================
    # Output files
    # ==========================================

    set(CSO_FILE "${OUTPUT_DIR}/${FILE_WE}.cso")
    set(HEADER_FILE "${OUTPUT_DIR}/${FILE_WE}.h")

    # ==========================================
    # Debug / Release Flags
    # ==========================================

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")

        set(DXC_FLAGS
            -Zi
            -Qembed_debug
            -Od
        )

    else()

        set(DXC_FLAGS
            -O3
        )

    endif()

    # ==========================================
    # 依赖：HLSL 本体 + 它 include 到的所有 .hlsli/.hlsl
    # ==========================================
    # 原实现只 DEPENDS ${SHADER_SOURCE}，因此修改 GBufferPS.hlsl 里 include 的
    # DeferredCommon.hlsli 不会触发重编译 —— 公共 Shader 改了等于没改。
    # 这里递归解析 #include 并收集全部依赖。解析范围限定在 Engine/Shader 下，
    # 避免把系统头或第三方头也卷进来。
    include(${CMAKE_SOURCE_DIR}/Engine/20_cmake/ShaderDeps.cmake)
    shader_collect_dependencies(
        "${SHADER_SOURCE}"
        "${CMAKE_SOURCE_DIR}/Engine/Shader"
        SHADER_DEPENDS
    )

    # ==========================================
    # Compile HLSL -> DXIL
    # ==========================================

    add_custom_command(
        OUTPUT ${CSO_FILE}

        COMMAND ${DXC_EXECUTABLE}

        -E ${ENTRY}
        -T ${PROFILE}

        -I "${CMAKE_SOURCE_DIR}/Engine/Shader"

        ${DXC_FLAGS}

        -Fo ${CSO_FILE}

        ${SHADER_SOURCE}

        DEPENDS ${SHADER_DEPENDS}

        COMMENT "Compiling Shader: ${SHADER_FILE}"

        VERBATIM
    )

    # ==========================================
    # Convert CSO -> Header
    # ==========================================

add_custom_command(
    OUTPUT ${HEADER_FILE}

    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CSO_FILE}
        -DOUTPUT_FILE=${HEADER_FILE}
        -P ${CMAKE_SOURCE_DIR}/Engine/20_cmake/BinToHeader.cmake

    DEPENDS ${CSO_FILE}

    COMMENT "Generating Header: ${HEADER_FILE}"

    VERBATIM
)

    # ==========================================
    # Custom Target
    # ==========================================

    add_custom_target(
        ${TARGET_NAME}_Shader
        DEPENDS ${HEADER_FILE}
    )

    # ==========================================
    # Export variable
    # ==========================================

    set(${TARGET_NAME}_HEADER
        ${HEADER_FILE}
        PARENT_SCOPE
    )
    # 同时导出「生成的头」与「源 HLSL + 其 include 依赖」这份完整列表，
    # 供上层用 OBJECT_DEPENDS 挂到真正 include 它的 .cpp 上。
    set(${TARGET_NAME}_HEADER_FILES
        ${HEADER_FILE}
        PARENT_SCOPE
    )

endfunction()

# ==========================================================
# compile_raytracing_library(
#     TARGET_NAME
#     SHADER_FILE
#     [PROFILE lib_6_4]
# )
# ----------------------------------------------------------
# R5 / D1：DXIL Library 编译（DXR State Object 专用）。
#
# 与 compile_shader 的两点关键差异：
#   1) **不传 -E**：lib profile 的语义是「把文件里所有入口点都编进库」，
#      由 State Object 的 DXIL_LIBRARY 子对象按导出名挑选。
#      传了 -E 就退化成单个 entry，RayGen/Miss/ClosestHit 无法共存于一份库。
#   2) 输出文件名固定为 <NAME_WE>.cso/.h，与普通 shader 同一套 BinToHeader 流程，
#      因此 C++ 侧照样 #include "RayTracing.h" 拿到字节码数组。
#
# 注意：lib_6_4 要求 DXC 与设备都支持 SM 6.4；本项目 DXC 来自 vcpkg 的
# directx-dxc，设备侧由 D3D12_RAYTRACING_TIER_1_0 保证（DXR 1.0 即要求 SM 6.3+）。
# 这里默认 6_4 是为了能用 float16/最新内置，实际字节码仍向后兼容 Tier 1.0。
# ==========================================================
function(compile_raytracing_library TARGET_NAME SHADER_FILE)

    set(options)
    set(oneValueArgs PROFILE)
    cmake_parse_arguments(ART "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT ART_PROFILE)
        set(ART_PROFILE "lib_6_4")
    endif()

    get_filename_component(FILE_WE ${SHADER_FILE} NAME_WE)

    set(SHADER_SOURCE "${CMAKE_SOURCE_DIR}/Engine/${SHADER_FILE}")
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/ShaderCache")

    file(MAKE_DIRECTORY ${OUTPUT_DIR})

    set(CSO_FILE "${OUTPUT_DIR}/${FILE_WE}.cso")
    set(HEADER_FILE "${OUTPUT_DIR}/${FILE_WE}.h")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(DXC_FLAGS -Zi -Qembed_debug -Od)
    else()
        set(DXC_FLAGS -O3)
    endif()

    include(${CMAKE_SOURCE_DIR}/Engine/20_cmake/ShaderDeps.cmake)
    shader_collect_dependencies(
        "${SHADER_SOURCE}"
        "${CMAKE_SOURCE_DIR}/Engine/Shader"
        SHADER_DEPENDS
    )

    # 关键：这里**没有 -E**。lib profile 会把所有 [shader("...")] 入口导出。
    add_custom_command(
        OUTPUT ${CSO_FILE}

        COMMAND ${DXC_EXECUTABLE}
        -T ${ART_PROFILE}
        -I "${CMAKE_SOURCE_DIR}/Engine/Shader"
        ${DXC_FLAGS}
        -Fo ${CSO_FILE}
        ${SHADER_SOURCE}

        DEPENDS ${SHADER_DEPENDS}

        COMMENT "Compiling Raytracing Library: ${SHADER_FILE} (${ART_PROFILE})"

        VERBATIM
    )

    add_custom_command(
        OUTPUT ${HEADER_FILE}

        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${CSO_FILE}
            -DOUTPUT_FILE=${HEADER_FILE}
            -P ${CMAKE_SOURCE_DIR}/Engine/20_cmake/BinToHeader.cmake

        DEPENDS ${CSO_FILE}

        COMMENT "Generating Header: ${HEADER_FILE}"

        VERBATIM
    )

    add_custom_target(
        ${TARGET_NAME}_Shader
        DEPENDS ${HEADER_FILE}
    )

    set(${TARGET_NAME}_HEADER
        ${HEADER_FILE}
        PARENT_SCOPE
    )
    set(${TARGET_NAME}_HEADER_FILES
        ${HEADER_FILE}
        PARENT_SCOPE
    )

endfunction()

# ==========================================================
# shader_attach_header_dependency(<source_file> <header> [<header>...])
# ----------------------------------------------------------
# 把一个「构建期生成的头文件」变成指定 .cpp 的**编译依赖**。
#
# 为什么必须显式做这件事：
#   生成头（*.h）不是 target 的源文件，CMake/ninja 默认不知道
#   “DeferredRenderer.cpp 编译时会 #include "LightingPS.h"”。
#   只靠 add_custom_target + add_dependencies，只能保证「编译前头文件已存在」，
#   但**头文件内容变化不会让 .cpp 重新编译**。
#
#   实测症状：改了 LightingPS.hlsl，cso 和 .h 的时间戳都更新了，
#   但 exe 里的字节码仍是旧的 —— 因为 DeferredRenderer.cpp 没被重编，
#   它 include 进去的是上一次编译时的旧数组。
#
#   用 OBJECT_DEPENDS 把生成头挂到源文件上，头变了就会触发重编。
# ==========================================================
function(shader_attach_header_dependency SOURCE_FILE)
    if(NOT EXISTS "${SOURCE_FILE}")
        # 源文件可能还没生成（极少数情况），跳过而不是报错
        return()
    endif()
    set_property(SOURCE "${SOURCE_FILE}" APPEND PROPERTY OBJECT_DEPENDS "${ARGN}")
endfunction()
