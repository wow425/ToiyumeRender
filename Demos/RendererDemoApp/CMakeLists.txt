# ============================================================================
# Demos/RendererDemoApp — 图形研究工作站统一管理入口
# ============================================================================
add_executable(RendererDemoApp WIN32 RendererDemoApp.cpp)

target_include_directories(RendererDemoApp PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(RendererDemoApp PRIVATE
    DeferredRenderDemoLib
    ForwardRenderDemoLib
    RayTracingRenderDemoLib
    GFL2RenderDemoLib
)

tooiyume_set_output_dirs(RendererDemoApp "Demos/RendererDemoApp")
tooiyume_deploy_runtime_dlls(RendererDemoApp)
tooiyume_use_agility_sdk(RendererDemoApp)
