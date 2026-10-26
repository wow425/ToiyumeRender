# ==========================================
# 编译可执行文件
# ==========================================
# 使用 WIN32 标志隐藏 Windows 黑框控制台
add_executable(TooiyumeApp WIN32 Tooiyume.cpp )

# 链接引擎库
target_link_libraries(TooiyumeApp PRIVATE TooiyumeEngine)

# ==========================================

# ==========================================
# 运行时 DLL（PIX / DirectXTex / DirectXMesh / dxcompiler / dxil）
# ==========================================
# 统一部署：函数定义见 Engine/20_cmake/DeployRuntimeDlls.cmake，
# 按 Debug/Release 自动选择 DLL 变体，并复制到本 target 输出目录。
tooiyume_deploy_runtime_dlls(TooiyumeApp)
