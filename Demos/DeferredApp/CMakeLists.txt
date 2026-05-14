# ==========================================
# 编译可执行文件
# ==========================================
add_executable(DeferredApp WIN32 
DeferredApp.cpp
DeferredRenderer.cpp
)

# 链接引擎库
target_link_libraries(DeferredApp PRIVATE TooiyumeEngine)

# ==========================================
# 拷贝运行时的 DLL 依赖 (POST_BUILD)
# ==========================================


function(copy_runtime_dll TARGET_NAME DLL_PATH)

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLL_PATH}"
        $<TARGET_FILE_DIR:${TARGET_NAME}>
    )

endfunction()

# 1. 拷贝 PIX 运行库
copy_runtime_dll(DeferredApp
    "${CMAKE_SOURCE_DIR}/ThirdParty/PIX/bin/WinPixEventRuntime.dll"
)
# 2. 拷贝 DXC 编译器动态库 (基于您提供的 vcpkg_installed 路径)
copy_runtime_dll(DeferredApp
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxcompiler.dll"
)

# 3. 拷贝 DXIL 库 (DX12 编译 Shader 并签名的必需依赖，通常与 dxcompiler.dll 在同一目录下)
copy_runtime_dll(DeferredApp
     "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxil.dll"
)

