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

target_link_libraries(DeferredRenderDemoLib PUBLIC DemoCommonLib)

add_executable(DeferredRenderDemo WIN32 Main.cpp)

target_link_libraries(DeferredRenderDemo PRIVATE DeferredRenderDemoLib)

tooiyume_set_output_dirs(DeferredRenderDemo "Demos/DeferredRenderDemo")
tooiyume_deploy_runtime_dlls(DeferredRenderDemo)
tooiyume_use_agility_sdk(DeferredRenderDemo)
