# 自动拷贝运行期依赖 DLL 到目标目录
# 传入变量：
#   - exe_path:  可执行文件的绝对路径
#   - dest_dir:  目标目录（可执行文件所在目录）
#   - search_dirs: CMake list（分号分隔）的额外搜索目录列表

if(NOT EXISTS "${exe_path}")
    message(FATAL_ERROR "exe_path not found: ${exe_path}")
endif()
if(NOT IS_DIRECTORY "${dest_dir}")
    message(FATAL_ERROR "dest_dir not a directory: ${dest_dir}")
endif()

# 构造依赖解析的搜索目录列表
set(_search_dirs "")
if(search_dirs)
    list(APPEND _search_dirs ${search_dirs})
endif()
if(DEFINED MINGW_RUNTIME_DIR AND MINGW_RUNTIME_DIR AND EXISTS "${MINGW_RUNTIME_DIR}")
    list(APPEND _search_dirs "${MINGW_RUNTIME_DIR}")
endif()
if(DEFINED ENV{MSYSTEM_PREFIX} AND EXISTS "$ENV{MSYSTEM_PREFIX}/bin")
    list(APPEND _search_dirs "$ENV{MSYSTEM_PREFIX}/bin")
endif()
list(REMOVE_DUPLICATES _search_dirs)
message(STATUS "Runtime search dirs: ${_search_dirs}")

# 过滤掉系统目录（避免复制系统 DLL）
set(_pre_exclude
    ".*/[Ww]indows/[^/]+/System32/.*"
    ".*/[Ww]indows/System32/.*"
)

set(_copied 0)

# 优先使用 CMake 3.14+ 的自动依赖解析
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.14")
    set(_deps "")
    set(_unresolved "")
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${exe_path}"
        DIRECTORIES ${_search_dirs}
        RESOLVED_DEPENDENCIES_VAR _deps
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        POST_EXCLUDE_REGEXES ${_pre_exclude}
    )

    foreach(dll IN LISTS _deps)
        if(EXISTS "${dll}")
            file(COPY "${dll}" DESTINATION "${dest_dir}")
            math(EXPR _copied "${_copied}+1")
        endif()
    endforeach()

    if(_unresolved)
        message(WARNING "Unresolved deps (may be fine if they are system DLLs): ${_unresolved}")
    endif()
    if(_copied EQUAL 0)
        message(WARNING "No runtime DLLs copied. Check toolchain match and search dirs.")
    endif()
else()
    # Fallback: use ntldd on MSYS2/MinGW
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
        set(ENV{PATH} "${_old_path}")
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

message(STATUS "Copied ${_copied} runtime DLL(s) to ${dest_dir}")

# 还原 PATH
set(ENV{PATH} "${_old_path}")