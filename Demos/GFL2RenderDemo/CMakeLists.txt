# ============================================================================
# Demos/GFL2RenderDemo — 少前2 AK-12 风格化管线模块 + 独立启动 exe
# ============================================================================
add_library(GFL2RenderDemoLib STATIC
    GFL2RenderDemoModule.h
    GFL2RenderDemoModule.cpp
)

target_include_directories(GFL2RenderDemoLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(GFL2RenderDemoLib PUBLIC DemoCommonLib)

add_executable(GFL2RenderDemo WIN32 Main.cpp)

target_link_libraries(GFL2RenderDemo PRIVATE GFL2RenderDemoLib)

tooiyume_set_output_dirs(GFL2RenderDemo "Demos/GFL2RenderDemo")
tooiyume_deploy_runtime_dlls(GFL2RenderDemo)
tooiyume_use_agility_sdk(GFL2RenderDemo)
