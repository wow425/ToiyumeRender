# ============================================================================
# Demos/CMakeLists.txt — 统一编排工作站底座 -> 4 个管线 Demo -> 管理器
# ============================================================================
add_library(DemoWorkstationLib STATIC
    RendererDemoApp/DemoWorkstation.h
    RendererDemoApp/DemoWorkstation.cpp
)

target_include_directories(DemoWorkstationLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(DemoWorkstationLib PUBLIC TooiyumeEngine)

add_subdirectory(DeferredRenderDemo)
add_subdirectory(ForwardRenderDemo)
add_subdirectory(RayTracingRenderDemo)
add_subdirectory(GFL2RenderDemo)
add_subdirectory(RendererDemoApp)
