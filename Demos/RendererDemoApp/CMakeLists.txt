# ==========================================
# RendererDemoApp —— R3 统一入口
# ==========================================
# 使用 WIN32 标志隐藏控制台窗口（与其它三个 Demo 一致）
add_executable(RendererDemoApp WIN32 RendererDemoApp.cpp)

target_include_directories(
        RendererDemoApp
        PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}
)

target_include_directories(
        RendererDemoApp
        PUBLIC
        ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache
)

# TooiyumeEngine 已经 PUBLIC link 了 imgui，这里不需要重复声明依赖。
target_link_libraries(RendererDemoApp PRIVATE TooiyumeEngine)

# ==========================================

# ==========================================
# 运行时 DLL（PIX / DirectXTex / DirectXMesh / dxcompiler / dxil）
# ==========================================
# 统一部署：函数定义见 Engine/20_cmake/DeployRuntimeDlls.cmake，
# 按 Debug/Release 自动选择 DLL 变体，并复制到本 target 输出目录。
tooiyume_deploy_runtime_dlls(RendererDemoApp)
