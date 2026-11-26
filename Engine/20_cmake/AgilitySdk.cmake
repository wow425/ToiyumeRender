# =============================================================================
# Agility SDK 接入（D3D12Core.dll 随应用分发）—— vcpkg 版本
# =============================================================================
# 用法：在**每个 Demo 的 CMakeLists.txt 里**、target 定义之后调用
#     tooiyume_use_agility_sdk(<target>)
#
# 历史：本文件早期版本从仓库内 ThirdParty/AgilitySDK/ 直接取 DLL 与头
# （手工拷入的 NuGet 解包产物）。现在改为完全由 vcpkg 管理：
#   - 版本声明在 vcpkg.json 的 dependency "directx12-agility"
#   - 版本钉住由 ThirdParty/vcpkg-overlays/directx12-agility/ 这个 overlay port 完成
#   - DLL / 头 / find_package 配置全部来自 vcpkg_installed/x64-windows/
# 好处：换机器不再需要手工拷二进制；版本、SHA512、许可证都有据可查。
#
# 为什么不集中在一个地方对4个 target 循环：
#   和 DeployRuntimeDlls.cmake 同样的理由 —— add_custom_command(TARGET ...) 与
#   target_sources 都要求 target 在**同一个 CMakeLists 目录**里创建，
#   根目录循环会报 "TARGET 'xxx' was not created in this directory"。
#
# ---------------------------------------------------------------------------
# 为什么不能只"链接"而不改 include 路径（本项目实测踩到的关键点）
# ---------------------------------------------------------------------------
# Agility SDK 的包自带一整套 d3d12.h / d3d12sdklayers.h / d3d12shader.h。
# 但**光把 D3D12Core.dll 放到 exe 旁边是不够的**：
#   - 运行时核心版本由 D3D12SDKVersion/Path 两个导出符号决定（见
#     AgilitySdkOptIn.cpp）；
#   - 而**编译期**用的头文件若仍是 Windows SDK 里那份旧的，就会缺新接口，
#     C++ 侧根本写不出调用新特性的代码。
# 本项目改造前实测：`cl /showIncludes` 显示 <d3d12.h> 实际解析到
#   C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\um\d3d12.h
# 其 D3D12_SDK_VERSION = 616，比 Agility 的 620 旧。
# 注意：vcpkg 的 directx-headers 包把头放在 include/directx/ 子目录，
# 而项目源码写的是 `#include <d3d12.h>`（无前缀），因此**从未真正用上它**。
# 所以本 overlay port 把自己 nupkg 里的 620 头装进了 include/ 根，
# 由 directx12-agility-config.cmake 以 INTERFACE 方式导出。
#
# ---------------------------------------------------------------------------
# 部署布局（对 exe 目录而言）
# ---------------------------------------------------------------------------
#   out/<Config>/Demos/<App>/
#       <App>.exe
#       D3D12/
#           D3D12Core.dll          <- 必须在这里，路径由 D3D12SDKPath 指定
#           d3d12SDKLayers.dll     <- Debug 层需与核心配套，版本必须一致
#
# 为什么 d3d12SDKLayers.dll 也要一起部署：
#   Debug 配置下 EnableDebugLayer() 会加载 SDK Layers。若 exe 目录里只有
#   新版 D3D12Core.dll、而 SDK Layers 仍走系统那份旧版，两者版本不匹配时
#   调试层会拒绝加载（或行为异常）。同版本配套是唯一安全做法。
#
# ---------------------------------------------------------------------------
# 升级流程（改版本时按顺序做，漏一步就静默失效）
# ---------------------------------------------------------------------------
#   1. 改 ThirdParty/vcpkg-overlays/directx12-agility/vcpkg.json 的 "version"
#   2. 改 portfile.cmake 的 SHA512（对应 nupkg 的哈希）
#   3. 改 AgilitySdkOptIn.cpp 的 kAgilitySdkVersion（必须 == D3D12Core.dll 版本）
#   4. 删 out/build/<Config> 重配（头变了，CMake 不会自动感知）
#   5. 跑 L4 验证，确认日志里 HighestShaderModel 达到预期
# =============================================================================

# vcpkg 的 directx12-agility-config.cmake 会创建：
#   Microsoft::DirectX12-Core     —— D3D12Core.dll 的 IMPORTED 目标
#   Microsoft::DirectX12-Layers   —— d3d12SDKLayers.dll 的 IMPORTED 目标
#   Microsoft::DirectX12-Agility  —— INTERFACE 目标，连带两者 + 620 头路径
# 并在该 config 里额外设置了 AGILITY_SDK_DIR / AGILITY_SDK_CORE_DLL /
# AGILITY_SDK_LAYERS_DLL / AGILITY_SDK_INCLUDE_DIR 四个变量（本仓库 overlay 版物）。
find_package(directx12-agility CONFIG REQUIRED)

# 版本号以 Agility 头里的宏为唯一真源，避免 CMake/C++ 两处手抄不一致。
# 注意：这里解析的是**头文件文本**，因为 CMake 无法在配置期求值 C 宏。
set(TOOIYUME_AGILITY_SDK_VERSION "")
if(EXISTS "${AGILITY_SDK_INCLUDE_DIR}/d3d12.h")
    file(STRINGS "${AGILITY_SDK_INCLUDE_DIR}/d3d12.h" _agility_ver_line
         REGEX "^#define[ \t]+D3D12_SDK_VERSION[ \t]+")
    if(_agility_ver_line)
        string(REGEX MATCH "D3D12_SDK_VERSION[ \t]+\\(?[ \t]*([0-9]+)" _m "${_agility_ver_line}")
        set(TOOIYUME_AGILITY_SDK_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

# 供日志/调试层显示"当前期望的 SDK 版本"，运行时可用 GetModuleHandle 校验。
add_compile_definitions(TOOIYUME_EXPECTED_AGILITY_SDK_VERSION=${TOOIYUME_AGILITY_SDK_VERSION})

function(tooiyume_agility_include target)
    # 只注入头文件搜索路径，不碰源文件与部署。
    # 给 TooiyumeEngine 静态库用：Engine 内部（PCH.h、各个 Renderer）也
    # 直接 include <d3d12.h>，若它与 Demo 看到的 d3d12.h 不是同一份，
    # 结构体/枚举定义会不一致（ODR 违规），Debug 下表现为诡异的内存错乱。
    # 因此必须让 Engine 与所有 Demo 都用同一套 Agility 头。
    if(NOT TARGET ${target})
        message(WARNING "tooiyume_agility_include: target ${target} 不存在，跳过。")
        return()
    endif()
    target_include_directories(${target} SYSTEM BEFORE PUBLIC
        "${AGILITY_SDK_INCLUDE_DIR}")
endfunction()

function(tooiyume_use_agility_sdk target)
    if(NOT TARGET ${target})
        message(WARNING "tooiyume_use_agility_sdk: target ${target} 不存在，跳过。")
        return()
    endif()

    # ---- 1) 把 opt-in 的 TU 直接编进 exe ----
    # 为什么用 target_sources 而不是链接一个静态库：见 AgilitySdkOptIn.cpp
    # 顶部"DX12 不变量 1" —— 符号必须在最终 exe 里，静态库里的孤岛 .obj
    # 可能被链接器丢弃。
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/Engine/20_cmake/AgilitySdkOptIn.cpp")

    # ---- 2) 头文件优先解析到 Agility 版本 ----
    # Microsoft::DirectX12-Agility 已带 INTERFACE_INCLUDE_DIRECTORIES，
    # 这里再显式加一次是幂等的兜底：万一消费方没链接该 target 也不会静默拿旧头。
    target_include_directories(${target} SYSTEM BEFORE PRIVATE
        "${AGILITY_SDK_INCLUDE_DIR}")

    # ---- 3) 把运行时 DLL 部署到 exe 同目录的 D3D12\ ----
    # 必须与 AgilitySdkOptIn.cpp 里的 kAgilitySdkPath（".\\D3D12\\"）保持一致。
    #
    # 为什么不像上游 usage 那样用 $<TARGET_PROPERTY:Microsoft::DirectX12-Core,
    # IMPORTED_LOCATION_RELEASE>：
    #   那些属性只在**生成期**求值，写进 add_custom_command 里语法可行，
    #   但一旦某天 target 名或属性名变了，报错信息极难读（只是空路径拷失败）。
    #   直接用 config 导出的真实路径变量，配置期就能校验存在性、早报错。
    set(_dll_pairs
        "${AGILITY_SDK_CORE_DLL}|D3D12Core.dll"
        "${AGILITY_SDK_LAYERS_DLL}|d3d12SDKLayers.dll")
    foreach(_pair ${_dll_pairs})
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _src)
        list(GET _parts 1 _dll)
        if(EXISTS "${_src}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/D3D12"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_src}" "$<TARGET_FILE_DIR:${target}>/D3D12/${_dll}"
                COMMENT "部署 Agility SDK ${_dll} -> ${target}/D3D12")
        else()
            message(WARNING
                "缺少 Agility SDK 运行时：${_src}\n"
                "${target} 能构建，但 D3D12 会回退到系统运行时（SM 6.6 不可用）。")
        endif()
    endforeach()

    if(TOOIYUME_AGILITY_SDK_VERSION)
        message(STATUS "[agility] ${target}: SDK version ${TOOIYUME_AGILITY_SDK_VERSION}")
    else()
        message(WARNING
            "[agility] 无法解析 D3D12_SDK_VERSION，请检查 vcpkg 的 "
            "directx12-agility 是否安装完整。")
    endif()
endfunction()
