# ============================================================================
# Demos/DeferredRenderDemo — 延迟渲染管线模块 + 独立启动 exe
# ============================================================================
add_library(DeferredRenderDemoLib STATIC
    DeferredRenderDemoModule.h
    DeferredRenderDemoModule.cpp
)

target_include_directories(DeferredRenderDemoLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(DeferredRenderDemoLib PUBLIC DemoWorkstationLib)

add_executable(DeferredRenderDemo WIN32
    DeferredRenderDemoModule.h
    DeferredRenderDemoModule.cpp
)

target_compile_definitions(DeferredRenderDemo PRIVATE TOOIYUME_DEMO_STANDALONE=1)
target_link_libraries(DeferredRenderDemo PRIVATE DemoWorkstationLib)

tooiyume_set_output_dirs(DeferredRenderDemo "Demos/DeferredRenderDemo")
tooiyume_deploy_runtime_dlls(DeferredRenderDemo)
tooiyume_use_agility_sdk(DeferredRenderDemo)
