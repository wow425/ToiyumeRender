# ==========================================
# 编译可执行文件
# ==========================================
# 使用 WIN32 标志隐藏 Windows 黑框控制台
add_executable(RayTracingApp WIN32 RayTracingApp.cpp)

# ==========================================
# Include 路径
# ==========================================

target_include_directories(
        RayTracingApp
        PUBLIC
        # Engine根目录
        ${CMAKE_SOURCE_DIR}/Engine
        # 如果需要访问项目根目录
        ${CMAKE_SOURCE_DIR}
)

target_include_directories(
        RayTracingApp
        PUBLIC
        ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache
)

# 链接引擎库
target_link_libraries(RayTracingApp PRIVATE TooiyumeEngine)

# ==========================================

# ==========================================
# 运行时 DLL（PIX / DirectXTex / DirectXMesh / dxcompiler / dxil）
# ==========================================
# 统一部署：函数定义见 Engine/20_cmake/DeployRuntimeDlls.cmake，
# 按 Debug/Release 自动选择 DLL 变体，并复制到本 target 输出目录。
tooiyume_deploy_runtime_dlls(RayTracingApp)
