# 自动拷贝运行期依赖 DLL 到目标目录
# 传入变量：
#   - exe_path:           可执行文件的绝对路径
#   - dest_dir:           目标目录（可执行文件所在目录）
#   - search_dirs:        CMake list（分号分隔）的额外搜索目录列表
#   - extra_copy_files:   可选，CMake list（分号分隔）的“必须复制”的 DLL 绝对路径（兜底）

if(NOT EXISTS "${exe_path}")
    message(FATAL_ERROR "exe_path not found: ${exe_path}")
endif()
if(NOT IS_DIRECTORY "${dest_dir}")
    message(FATAL_ERROR "dest_dir not a directory: ${dest_dir}")
endif()

# search_dirs 由顶层通过 -Dsearch_dirs=... 传入
if(NOT search_dirs)
    message(WARNING "No search_dirs provided. Skipping DLL copy.")
    return()
endif()

# 清洗：去掉列表项首尾可能混入的双引号，并规范为前斜杠路径
set(_sd_clean "")
foreach(_d IN LISTS search_dirs)
    string(REGEX REPLACE "^\"|\"$" "" _d "${_d}")
    file(TO_CMAKE_PATH "${_d}" _d_norm)
    list(APPEND _sd_clean "${_d_norm}")
endforeach()
set(search_dirs "${_sd_clean}")

# 同样清洗 extra_copy_files（如果传入）
if(extra_copy_files)
    set(_ef_clean "")
    foreach(_f IN LISTS extra_copy_files)
        string(REGEX REPLACE "^\"|\"$" "" _f "${_f}")
        file(TO_CMAKE_PATH "${_f}" _f_norm)
        list(APPEND _ef_clean "${_f_norm}")
    endforeach()
    set(extra_copy_files "${_ef_clean}")
endif()

# 打印搜索目录，便于调试（此处将看到无引号、已规范化的路径）
message(STATUS "--- copy_runtime_deps.cmake ---")
message(STATUS "Searching for runtime dependencies in the following directories:")
foreach(dir IN LISTS search_dirs)
    message(STATUS "  -> ${dir}")
endforeach()
if(extra_copy_files)
    message(STATUS "Extra force-copy files (if exist): ${extra_copy_files}")
endif()
message(STATUS "-------------------------------")

# 过滤掉系统目录（增强反斜杠匹配）
set(_pre_exclude
    "^[A-Za-z]:[\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/].*"
)

set(_copied 0)

# 优先使用 CMake 3.14+ 的自动依赖解析
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.14")
    set(_deps "")
    set(_unresolved "")
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${exe_path}"
        DIRECTORIES ${search_dirs}
        RESOLVED_DEPENDENCIES_VAR _deps
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        POST_EXCLUDE_REGEXES ${_pre_exclude}
    )

    if(_deps)
        message(STATUS "Resolved dependencies:")
        foreach(dll IN LISTS _deps)
            message(STATUS "  + ${dll}")
            # 兜底再过滤一次 Windows 目录（大小写无关）
            if("${dll}" MATCHES "^[A-Za-z]:[\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/].*")
                continue()
            endif()
            if(EXISTS "${dll}")
                file(COPY "${dll}" DESTINATION "${dest_dir}")
                math(EXPR _copied "${_copied}+1")
            endif()
        endforeach()
    else()
        message(WARNING "No resolved dependencies from GET_RUNTIME_DEPENDENCIES.")
    endif()

    if(_unresolved)
        message(WARNING "Unresolved deps (may be fine if they are system DLLs): ${_unresolved}")
    endif()

else()
    # 兼容旧版 CMake：使用 ntldd（需 MSYS2/MinGW 环境提供）
    find_program(NTLDD ntldd)
    if(NOT NTLDD)
        message(WARNING "CMake < 3.14 and 'ntldd' not found. Skip copying runtime DLLs.")
        return()
    endif()

    execute_process(
        COMMAND "${NTLDD}" -f "${exe_path}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
        WORKING_DIRECTORY "${dest_dir}"
    )
    if(NOT _rc EQUAL 0)
        message(WARNING "ntldd failed (code=${_rc}): ${_err}")
        return()
    endif()

    string(REPLACE "\r\n" "\n" _out "${_out}")
    string(REGEX MATCHALL "([A-Za-z]:[^\\n]*\\.[Dd][Ll][Ll])" _dlls_win "${_out}")

    foreach(_p IN LISTS _dlls_win)
        set(_skip 0)
        foreach(_ex IN LISTS _pre_exclude)
            if(_p MATCHES "${_ex}")
                set(_skip 1)
            endif()
        endforeach()
        if(_skip)
            continue()
        endif()

        file(TO_CMAKE_PATH "${_p}" _dll)
        if(EXISTS "${_dll}")
            file(COPY "${_dll}" DESTINATION "${dest_dir}")
            math(EXPR _copied "${_copied}+1")
        endif()
    endforeach()
endif()

# 兜底：强制复制指定文件（存在就复制），避免解析遗漏
if(extra_copy_files)
    foreach(_f IN LISTS extra_copy_files)
        if(EXISTS "${_f}")
            get_filename_component(_bn "${_f}" NAME)
            if(NOT EXISTS "${dest_dir}/${_bn}")
                file(COPY "${_f}" DESTINATION "${dest_dir}")
                math(EXPR _copied "${_copied}+1")
                message(STATUS "Force-copied: ${_f}")
            endif()
        endif()
    endforeach()
endif()

message(STATUS "Copied ${_copied} runtime DLL(s) to ${dest_dir}")