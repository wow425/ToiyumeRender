# ==========================================
# 第三方运行时 DLL 统一部署（R4.1）
# ==========================================
# 用法：在**每个 Demo 自己的 CMakeLists.txt 里**、target 定义之后调用：
#     tooiyume_deploy_runtime_dlls(<target>)
#
# 为什么必须是"函数 + 各目录自调用"，而不是在根 CMakeLists 里循环：
#   CMake 的 `add_custom_command(TARGET ...)` 要求该 target **在同一个
#   CMakeLists 目录**中创建，否则报
#   "TARGET 'xxx' was not created in this directory"。
#   根 CMakeLists 与 Demos/*/ 是不同目录，因此不能集中添加。
#
# 为什么不能只靠 vcpkg toolchain 的 VCPKG_APPLOCAL_DEPS：
#   根 CMakeLists 现在支持「不设 VCPKG_ROOT、只用仓库内 vcpkg_installed」
#   的路径（Visual Studio 打开项目时默认走这条）。这条路径不加载 toolchain，
#   APPLOCAL_DEPS 不会运行，DirectXTex.dll / DirectXMesh.dll 就没人拷，
#   exe 启动直接报缺 DLL。
#
# DLL 变体选择：
#   Debug 配置优先用 vcpkg 的 debug/bin（与调试版 CRT 匹配）；
#   Release 用 bin；debug/bin 缺失时回退 bin，保证总能跑起来。

set(_TOOI_DLL_ROOT "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")

function(tooiyume_deploy_runtime_dlls target)
    if(NOT TARGET ${target})
        message(WARNING "tooiyume_deploy_runtime_dlls: target ${target} 不存在，跳过。")
        return()
    endif()

    set(_root "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")

    # 按配置选择 DLL 变体。CMAKE_BUILD_TYPE 对单配置生成器（Ninja）可靠。
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND EXISTS "${_root}/debug/bin")
        set(_dll_dir "${_root}/debug/bin")
    else()
        set(_dll_dir "${_root}/bin")
    endif()

    set(_dlls DirectXTex.dll DirectXMesh.dll dxcompiler.dll dxil.dll)

    foreach(_dll ${_dlls})
        if(EXISTS "${_dll_dir}/${_dll}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll_dir}/${_dll}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "部署 ${_dll} -> ${target}")
        else()
            message(WARNING
                "缺少第三方运行时 DLL：${_dll_dir}/${_dll}\n"
                "${target} 能构建，但启动会因缺 DLL 失败。")
        endif()
    endforeach()

    set(_pix "${CMAKE_SOURCE_DIR}/ThirdParty/PIX/bin/WinPixEventRuntime.dll")
    if(EXISTS "${_pix}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_pix}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "部署 WinPixEventRuntime.dll -> ${target}")
    else()
        message(WARNING "缺少 PIX 运行库：${_pix}")
    endif()
endfunction()
