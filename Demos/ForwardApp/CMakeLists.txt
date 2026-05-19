# ==========================================
# 编译可执行文件
# ==========================================
# 使用 WIN32 标志隐藏 Windows 黑框控制台
add_executable(TooiyumeApp WIN32 Tooiyume.cpp )

# 链接引擎库
target_link_libraries(TooiyumeApp PRIVATE TooiyumeEngine)

# ==========================================
# 拷贝运行时的 DLL 依赖 (POST_BUILD)
# ==========================================

# 1. 拷贝 PIX 运行库
add_custom_command(TARGET TooiyumeApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/ThirdParty/PIX/bin/WinPixEventRuntime.dll"
    $<TARGET_FILE_DIR:TooiyumeApp>
    COMMENT "Copying WinPixEventRuntime.dll to output directory..."
)

# 2. 拷贝 DXC 编译器动态库 (基于您提供的 vcpkg_installed 路径)
add_custom_command(TARGET TooiyumeApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxcompiler.dll"
    $<TARGET_FILE_DIR:TooiyumeApp>
    COMMENT "Copying dxcompiler.dll to output directory..."
)

# 3. 拷贝 DXIL 库 (DX12 编译 Shader 并签名的必需依赖，通常与 dxcompiler.dll 在同一目录下)
add_custom_command(TARGET TooiyumeApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin/dxil.dll"
    $<TARGET_FILE_DIR:TooiyumeApp>
    COMMENT "Copying dxil.dll to output directory..."
)
