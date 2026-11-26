# ============================================================================
# Demos/RayTracingRenderDemo — DXR 光追管线模块 + 独立启动 exe
# ============================================================================
add_library(RayTracingRenderDemoLib STATIC
    RayTracingRenderDemoModule.h
    RayTracingRenderDemoModule.cpp
)

target_include_directories(RayTracingRenderDemoLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(RayTracingRenderDemoLib PUBLIC DemoWorkstationLib)

add_executable(RayTracingRenderDemo WIN32
    RayTracingRenderDemoModule.h
    RayTracingRenderDemoModule.cpp
)

target_compile_definitions(RayTracingRenderDemo PRIVATE TOOIYUME_DEMO_STANDALONE=1)
target_link_libraries(RayTracingRenderDemo PRIVATE DemoWorkstationLib)

tooiyume_set_output_dirs(RayTracingRenderDemo "Demos/RayTracingRenderDemo")
tooiyume_deploy_runtime_dlls(RayTracingRenderDemo)
tooiyume_use_agility_sdk(RayTracingRenderDemo)
