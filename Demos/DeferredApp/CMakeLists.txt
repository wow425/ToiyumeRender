# ==========================================
# 编译可执行文件
# ==========================================
# 使用 WIN32 标志隐藏 Windows 黑框控制台
add_executable(DeferredApp WIN32 DeferredApp.cpp)

# ==========================================
# Include 路径
# ==========================================

target_include_directories(
        DeferredApp
        PUBLIC
        # Engine根目录
        ${CMAKE_SOURCE_DIR}/Engine
        # 如果需要访问项目根目录
        ${CMAKE_SOURCE_DIR}
)

# 说明：这里原有一条指向 ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache 的
# include，那是 CLion 时代的遗留路径（目录早已不存在），已删除。
# 生成头的正确位置由 TooiyumeEngine 的 PUBLIC include
# ${TOOIYUME_SHADER_OUT_DIR}（= out/<Config>/Engine/Shader）提供。

# 产物落 out/<Config>/Demos/DeferredApp/（镜像源码层次）
tooiyume_set_output_dirs(DeferredApp "Demos/DeferredApp")

# 链接引擎库
target_link_libraries(DeferredApp PRIVATE TooiyumeEngine)

# ==========================================

# ==========================================
# 运行时 DLL（PIX / DirectXTex / DirectXMesh / dxcompiler / dxil）
# ==========================================
# 统一部署：函数定义见 Engine/20_cmake/DeployRuntimeDlls.cmake，
# 按 Debug/Release 自动选择 DLL 变体，并复制到本 target 输出目录。
tooiyume_deploy_runtime_dlls(DeferredApp)
