# ==========================================
# Shader include 依赖收集
# ==========================================
# 目的：让「修改公共 .hlsli 触发所有引用它的 shader 重编译」成为构建系统的事实，
# 而不是靠人手动 touch HLSL。
#
# 实现方式：递归扫描 #include "..." 与 #include <...>，按相对 shader 文件所在目录
# 和 CMAKE_SOURCE_DIR/Engine/Shader 两个根解析，收集所有真实存在的文件。
# 只做静态文本扫描 —— 这已经覆盖本项目全部 shader（没有宏拼出来的 include）。

# shader_collect_dependencies(<shader_source> <include_root> <out_var>)
function(shader_collect_dependencies SHADER_SOURCE INCLUDE_ROOT OUT_VAR)
    set(_seen "")
    set(_queue "${SHADER_SOURCE}")

    while(_queue)
        list(POP_FRONT _queue _file)
        list(FIND _seen "${_file}" _idx)
        if(NOT _idx EQUAL -1)
            continue()
        endif()

        if(NOT EXISTS "${_file}")
            continue()
        endif()

        list(APPEND _seen "${_file}")

        get_filename_component(_dir "${_file}" DIRECTORY)
        file(STRINGS "${_file}" _inc_lines REGEX "^[ \t]*#[ \t]*include")

        foreach(_line IN LISTS _inc_lines)
            # 抽取引号或尖括号中的路径
            string(REGEX MATCH "\"([^\"]+)\"" _quoted "${_line}")
            if(_quoted)
                string(REGEX REPLACE "^\"|\"$" "" _inc "${_quoted}")
            else()
                string(REGEX MATCH "<([^>]+)>" _angled "${_line}")
                if(_angled)
                    string(REGEX REPLACE "^<|>$" "" _inc "${_angled}")
                else()
                    continue()
                endif()
            endif()

            if(_inc STREQUAL "")
                continue()
            endif()

            # 解析顺序：相对于当前文件所在目录 -> Engine/Shader 根
            if(EXISTS "${_dir}/${_inc}")
                list(APPEND _queue "${_dir}/${_inc}")
            elseif(EXISTS "${INCLUDE_ROOT}/${_inc}")
                list(APPEND _queue "${INCLUDE_ROOT}/${_inc}")
            endif()
        endforeach()
    endwhile()

    set(${OUT_VAR} "${_seen}" PARENT_SCOPE)
endfunction()
