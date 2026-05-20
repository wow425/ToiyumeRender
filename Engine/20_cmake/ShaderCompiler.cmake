
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
#     vs_6_6
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

        DEPENDS ${SHADER_SOURCE}

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

endfunction()
