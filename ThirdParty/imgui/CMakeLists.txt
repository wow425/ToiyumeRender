# ==========================================
# Dear ImGui (1.91.9b) —— 本地三方静态库
# ==========================================
# 为什么自己写 CMakeLists 而不是 find_package：
# 仓库里已经把 ImGui 源码整个拷进 ThirdParty/imgui（无 vcpkg 依赖、离线可构建），
# 所以这里只负责把源码编成一个静态库目标 `imgui`，供 Engine / Demos 直接 link。
#
# 为什么目标名固定为 `imgui`：
# 上层（Demo / Engine）统一 `target_link_libraries(<target> PRIVATE imgui)`，
# 目标名一变所有消费点都要跟着改，没有收益。
#
# 注意：imconfig.h 用仓库现成的那份，不要在这里覆盖或额外塞配置宏，
# 否则同一份源码在不同构建里行为不一致，出问题极难定位。
# ==========================================

add_library(imgui STATIC
    # ---- 核心 ----
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    # ---- Demo 窗口（ShowDemoWindow）----
    # 现在没人用，但一起编进静态库，链接时未被引用的目标文件不会进最终 exe，
    # 不会增加二进制体积；以后要开 ImGui Demo 窗口不用再改 CMake + 重新配置。
    imgui_demo.cpp
    # ---- 平台 / 图形后端 ----
    # 只编 Win32 + D3D12 两条：本项目是 DX12 渲染器，其余后端（Vulkan/GL/SDL…）
    # 既无依赖也不该出现在构建里。
    backends/imgui_impl_win32.cpp
    backends/imgui_impl_dx12.cpp
)

# 头文件搜索路径设成 PUBLIC：
# 消费方只 include "imgui.h" / "imgui_impl_dx12.h"，不该再关心 ThirdParty 的目录层级。
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# 依赖库同样 PUBLIC：
# TooiyumeEngine / Demos 是静态库 + 可执行文件的组合，静态库不传递链接依赖时，
# 最终 exe 会缺符号（典型报错 LNK2019: D3D12CreateDevice / CreateDXGIFactory）。
#   - d3d12 / dxgi : imgui_impl_dx12 用 D3D12 类型与资源屏障
#   - user32 / gdi32 / imm32 : imgui_impl_win32 的窗口、GDI 位图、输入法
#   - dwmapi : imgui_impl_win32 里 #pragma comment(lib,"dwmapi") 只对 MSVC 自动生效，
#              显式写上是为了不依赖编译器扩展，MinGW/其他工具链也能过
target_link_libraries(imgui PUBLIC
    d3d12
    dxgi
    user32
    gdi32
    imm32
    dwmapi
)

# 关闭 ImGui 的废弃接口（IMGUI_DISABLE_OBSOLETE_FUNCTIONS）：
#   - 新接入的项目没有任何历史包袱，用新 API 即可；
#   - 关掉废弃层能防止有人把已废弃的旧 API 写进新代码（能编过 = 没人会发现）。
# 用 PUBLIC 是为了让所有翻译单元看到同一份 imgui.h 视图，
# 一旦出现「库内编了 A 分支、消费方编了 B 分支」就是 ODR 违规。
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# 三方代码不参与本项目的警告治理：ImGui 源码自身在 /W4 下会有大量警告，
# 一旦以后有人开 /WX（警告即错误），构建会被三方代码卡死。
# 这里降低到 /W0（仅 MSVC 有该选项），并保留 /utf-8 跟随根目录全局设置。
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

# 输出目录统一：静态库不产生运行时产物，这里只保证 .lib 落在和
# TooiyumeEngine 相同的输出根下，避免多配置生成器下路径漂移。
set_target_properties(imgui PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
