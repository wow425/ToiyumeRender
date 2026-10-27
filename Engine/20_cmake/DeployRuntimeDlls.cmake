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

    # 按配置选择 DLL 变体。
    #
    # 为什么必须用 $<CONFIG> 生成器表达式、而不是 if(CMAKE_BUILD_TYPE ...)：
    #   VS 生成器是**多配置**生成器，CMAKE_BUILD_TYPE 在配置期是一个固定的单值
    #   （本项目 configure 时写的是 Debug），与用户随后选中的 --config 无关。
    #   旧写法 if(CMAKE_BUILD_TYPE STREQUAL "Debug") 在 VS 里恒真 ——
    #   于是 Debug 和 Release 两个配置都把 debug/bin 的调试版 DLL 拷过去，
    #   Release 可执行文件会链接到调试版 CRT 的第三方库（混用 /MTd 与 /MT）。
    #
    # **坑（2026-10-27 踩过，务必别重犯）**：
    #   不能把生成器表达式塞进 if(EXISTS ...) 里做存在性判断！
    #   `set(_dll_dir "$<IF:$<CONFIG:Debug>,...>")` 之后
    #   `if(EXISTS "${_dll_dir}/DirectXTex.dll")` 判断的是**未展开的表达式文本**
    #   （形如 "$<IF:$<CONFIG:Debug>,D:/.../debug/bin,D:/.../bin>/DirectXTex.dll"），
    #   这个路径永远不存在 → 判断恒假 → 四条 copy 命令一条都不会被添加。
    #   症状：configure 期打出一堆 "缺少第三方运行时 DLL：$<IF:...>/dxil.dll"
    #   警告，且**删掉产物 DLL 再 build 报 "ninja: no work to do"、DLL 再也不会回来**。
    #   更隐蔽的是：build.bat 走 vcpkg toolchain 时 APPLOCAL_DEPS 会兜底拷 DLL，
    #   于是命令行构建看不出问题；只有「VS 打开文件夹」模式（不走 toolchain）
    #   才会真的缺 DLL 起不来。
    #
    # 正确写法：存在性用**真实路径**分别判断，只让 copy 命令的**参数**含生成器表达式。
    set(_dbg_dir "${_root}/debug/bin")
    set(_rel_dir "${_root}/bin")

    set(_dlls DirectXTex.dll DirectXMesh.dll dxcompiler.dll dxil.dll)

    foreach(_dll ${_dlls})
        if(EXISTS "${_dbg_dir}/${_dll}" AND EXISTS "${_rel_dir}/${_dll}")
            # 两个变体都在：交给 $<CONFIG> 在构建期选（单配置/多配置生成器都正确）
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${_dbg_dir}/${_dll},${_rel_dir}/${_dll}>"
                    "$<TARGET_FILE_DIR:${target}>"
                COMMENT "部署 ${_dll} (按配置变体) -> ${target}")
        elseif(EXISTS "${_rel_dir}/${_dll}")
            # 只有 Release 变体：固定用它
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_rel_dir}/${_dll}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "部署 ${_dll} (仅 Release 变体可用) -> ${target}")
        elseif(EXISTS "${_dbg_dir}/${_dll}")
            # 只有 Debug 变体：固定用它
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dbg_dir}/${_dll}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "部署 ${_dll} (仅 Debug 变体可用) -> ${target}")
        else()
            message(WARNING
                "缺少第三方运行时 DLL：${_dll}\n"
                "  已查找：${_dbg_dir}/${_dll}\n"
                "          ${_rel_dir}/${_dll}\n"
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
