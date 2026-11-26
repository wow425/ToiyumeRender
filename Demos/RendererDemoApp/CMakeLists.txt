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

# 说明：这里原有一条指向 ${CMAKE_SOURCE_DIR}/cmake-build-debug/ShaderCache 的
# include，那是 CLion 时代的遗留路径（目录早已不存在），已删除。
# 生成头的正确位置由 TooiyumeEngine 的 PUBLIC include
# ${TOOIYUME_SHADER_OUT_DIR}（= out/<Config>/Engine/Shader）提供。

# 产物落 out/<Config>/Demos/RendererDemoApp/（镜像源码层次）
tooiyume_set_output_dirs(RendererDemoApp "Demos/RendererDemoApp")

# RendererDemoApp 作为纯主控管理器，聚合 4 个独立 Demo 模块库
target_link_libraries(RendererDemoApp PRIVATE
    DeferredRenderDemoLib
    ForwardRenderDemoLib
    RayTracingRenderDemoLib
    GFL2RenderDemoLib
)

# ==========================================

# ==========================================
# 运行时 DLL（PIX / DirectXTex / DirectXMesh / dxcompiler / dxil）
# ==========================================
# 统一部署：函数定义见 Engine/20_cmake/DeployRuntimeDlls.cmake，
# 按 Debug/Release 自动选择 DLL 变体，并复制到本 target 输出目录。
tooiyume_deploy_runtime_dlls(RendererDemoApp)

# ==========================================
# Agility SDK（D3D12Core.dll 随应用分发）
# ==========================================
# 接入前实测：HighestShaderModel=0x65（SM 6.5，系统运行时限制，非显卡限制）
# 接入后实测：HighestShaderModel=0x66（SM 6.6，bindless 所需）
# 详见 Engine/20_cmake/AgilitySdkOptIn.cpp 与
# docs/AI/worklogs/TR-20261121-01_wp-0a-baseline.md §2.2
tooiyume_use_agility_sdk(RendererDemoApp)
