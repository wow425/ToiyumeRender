# ============================================================================
# Demos/ForwardRenderDemo — 前向渲染管线模块 + 独立启动 exe
# ============================================================================
add_library(ForwardRenderDemoLib STATIC
    ForwardRenderDemoModule.h
    ForwardRenderDemoModule.cpp
)

target_include_directories(ForwardRenderDemoLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(ForwardRenderDemoLib PUBLIC DemoCommonLib)

add_executable(ForwardRenderDemo WIN32 Main.cpp)

target_link_libraries(ForwardRenderDemo PRIVATE ForwardRenderDemoLib)

tooiyume_set_output_dirs(ForwardRenderDemo "Demos/ForwardRenderDemo")
tooiyume_deploy_runtime_dlls(ForwardRenderDemo)
tooiyume_use_agility_sdk(ForwardRenderDemo)
