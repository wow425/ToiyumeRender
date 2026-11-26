# ============================================================================
# Demos/Common — 共享 DemoModule 契约与独立运行外壳
# ============================================================================
add_library(DemoCommonLib STATIC
    IDemoModule.h
    DemoSceneHelpers.h
    DemoSceneHelpers.cpp
    DemoStandaloneApp.h
)

target_include_directories(DemoCommonLib PUBLIC
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(DemoCommonLib PUBLIC TooiyumeEngine)
