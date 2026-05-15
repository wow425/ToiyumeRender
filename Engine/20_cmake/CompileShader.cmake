

function(compile_shader SHADER_PATH TARGET_PROFILE ENTRY_POINT DEFINES)
    # 获取文件名 (例如 BasePass_VS)
    get_filename_component(SHADER_NAME ${SHADER_PATH} NAME_WLE)
    
    # 设定 CSO 的输出路径 (注意：放在 BINARY_DIR 即 out/build/ 下，不污染源码目录)
    set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/CompiledShaders")
    set(OUTPUT_PATH "${OUTPUT_DIR}/${SHADER_NAME}.cso")
    
    # 确保输出目录存在
    file(MAKE_DIRECTORY ${OUTPUT_DIR})

    # 转换宏定义
    set(DXC_DEFINES "")
    foreach(DEF ${DEFINES})
        list(APPEND DXC_DEFINES "-D${DEF}")
    endforeach()

    # 注册自定义构建命令
    add_custom_command(
        OUTPUT ${OUTPUT_PATH}
        COMMAND dxcompiler.exe
            -T ${TARGET_PROFILE}
            -E ${ENTRY_POINT}
            ${DXC_DEFINES}
            -Fo ${OUTPUT_PATH}
            ${SHADER_PATH}
        DEPENDS ${SHADER_PATH}
        COMMENT "Compiling Shader: ${SHADER_NAME} (${TARGET_PROFILE})"
    )
    
    # 把生成的 .cso 绑定到当前的 Target（引擎静态库），这样编译引擎时才会触发 DXC
    target_sources(TooiyumeEngine PRIVATE ${OUTPUT_PATH})
endfunction()