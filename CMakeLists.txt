cmake_minimum_required(VERSION 3.5)
project(stairspeedtest LANGUAGES CXX)

# 使用工程名派生目标名，避免硬编码
set(APP_TARGET "${PROJECT_NAME}")
set(CORE_TARGET "${PROJECT_NAME}_core")

# 模块路径（保留）
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

# Fix for GCC bug #70129 - prevents stdlib.h: No such file or directory
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)
add_definitions(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result)

option(USING_STD_REGEX "Use std::regex from C++ library instead of PCRE2." OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
"
#include<string>
void main(){std::to_string(0);}
" HAVE_TO_STRING)

include(CheckSymbolExists)
check_symbol_exists(MSG_NOSIGNAL "sys/socket.h" HAVE_MSG_NOSIGNAL)

if(APPLE)
    add_definitions(-D_MACOS)
endif()

if(HAVE_TO_STRING)
    add_definitions(-DHAVE_TO_STRING)
endif()

if(HAVE_MSG_NOSIGNAL)
    add_definitions(-DHAVE_MSG_NOSIGNAL)
endif()

# 可选模块开关（默认关闭）
option(BUILD_PERF_TOOLS "Build performance/benchmark modules (perf_test, multithread_test)" OFF)

# 运行时 DLL 拷贝总开关（仅 Windows 生效；默认关闭）
option(ENABLE_RUNTIME_DLL_COPY "Copy runtime DLLs to target directory after build (Windows only)" OFF)

# 自动收集 src 下所有 .cpp（对 CMake<3.12 做兼容处理）
if(CMAKE_VERSION VERSION_LESS 3.12)
    file(GLOB SRC_ALL "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
else()
    file(GLOB SRC_ALL CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
endif()

# 定义“可选源码”，默认不参与核心库编译
set(OPTIONAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/perf_test.cpp"
)

# 从核心库源码中排除 main.cpp 和可选源码
list(REMOVE_ITEM SRC_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    ${OPTIONAL_SOURCES}
)

# 核心静态库，承载绝大部分功能源码与第三方依赖
add_library(${CORE_TARGET} STATIC ${SRC_ALL})
target_include_directories(${CORE_TARGET} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
)

# 主程序，仅包含 main.cpp，并只链接核心库
add_executable(${APP_TARGET} "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(${APP_TARGET} PRIVATE ${CORE_TARGET})

# 统一输出目录：可执行到 build/bin，静态库到 build/lib
set(OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set_target_properties(${APP_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}"
)
set_target_properties(${CORE_TARGET} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 确保输出目录存在（在配置阶段创建，避免 ar: No such file or directory）
file(MAKE_DIRECTORY "${OUTPUT_BIN_DIR}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 根据开关为主程序附加可选模块
if(BUILD_PERF_TOOLS)
    target_sources(${APP_TARGET} PRIVATE ${OPTIONAL_SOURCES})
    target_compile_definitions(${APP_TARGET} PRIVATE ENABLE_PERF_TOOLS=1)
else()
    target_compile_definitions(${APP_TARGET} PRIVATE ENABLE_PERF_TOOLS=0)
endif()

# Breadcrumb logging: default off in Release, on in Debug. Can override via ENABLE_BREADCRUMB_LOGS.
option(ENABLE_BREADCRUMB_LOGS "Enable breadcrumb debug logging (file IO)" ON)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(ENABLE_BREADCRUMB_LOGS OFF)
endif()

# Apply compile definitions for single-config generators
if(ENABLE_BREADCRUMB_LOGS)
    target_compile_definitions(${APP_TARGET} PRIVATE BREADCRUMB_ENABLED=1)
    target_compile_definitions(${CORE_TARGET} PRIVATE BREADCRUMB_ENABLED=1)
else()
    target_compile_definitions(${APP_TARGET} PRIVATE BREADCRUMB_ENABLED=0)
    target_compile_definitions(${CORE_TARGET} PRIVATE BREADCRUMB_ENABLED=0)
endif()

# Also handle multi-config generators (MSVC/Visual Studio)
target_compile_definitions(${APP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:BREADCRUMB_ENABLED=1>
    $<$<CONFIG:RelWithDebInfo>:BREADCRUMB_ENABLED=1>
    $<$<CONFIG:Release>:BREADCRUMB_ENABLED=0>
    $<$<CONFIG:MinSizeRel>:BREADCRUMB_ENABLED=0>
)
target_compile_definitions(${CORE_TARGET} PRIVATE
    $<$<CONFIG:Debug>:BREADCRUMB_ENABLED=1>
    $<$<CONFIG:RelWithDebInfo>:BREADCRUMB_ENABLED=1>
    $<$<CONFIG:Release>:BREADCRUMB_ENABLED=0>
    $<$<CONFIG:MinSizeRel>:BREADCRUMB_ENABLED=0>
)

# 将运行时资源复制到可执行文件所在目录（例如 build 输出目录）
add_custom_command(TARGET ${APP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}"
    # 一步复制 base 下的所有内容到 OUTPUT_BIN_DIR（不会把“base”这一层带过去）
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/base"
            "${OUTPUT_BIN_DIR}"
)

# 依赖收集（全部挂到核心库目标，避免全局污染）
find_package(PkgConfig REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${CORE_TARGET} PUBLIC Threads::Threads)

# libevent（使用 pkg-config 辅助定位）
pkg_check_modules(LIBEVENT libevent REQUIRED)
find_path(LIBEVENT_INCLUDE_DIR NAMES event.h PATHS ${LIBEVENT_INCLUDE_DIRS})
find_library(LIBEVENT_LIBRARY NAMES event PATHS ${LIBEVENT_LIBRARY_DIRS})
if(LIBEVENT_INCLUDE_DIR)
    target_include_directories(${CORE_TARGET} PUBLIC ${LIBEVENT_INCLUDE_DIR})
endif()
if(LIBEVENT_LIBRARY)
    target_link_libraries(${CORE_TARGET} PUBLIC ${LIBEVENT_LIBRARY})
endif()

# CURL
find_package(CURL 7.54.0 REQUIRED)
target_link_libraries(${CORE_TARGET} PUBLIC CURL::libcurl)
# 如果你确定使用静态 libcurl 再启用下一行；否则不要定义
# target_compile_definitions(${CORE_TARGET} PUBLIC CURL_STATICLIB)

# OpenSSL
find_package(OpenSSL 1.1.0 REQUIRED)
target_include_directories(${CORE_TARGET} PUBLIC ${OPENSSL_INCLUDE_DIR})
target_link_libraries(${CORE_TARGET} PUBLIC ${OPENSSL_LIBRARIES})

# Rapidjson（header-only）
find_package(Rapidjson REQUIRED)
target_include_directories(${CORE_TARGET} PUBLIC ${RAPIDJSON_INCLUDE_DIRS})

# libpng
find_package(PNG REQUIRED)
target_include_directories(${CORE_TARGET} PUBLIC ${PNG_INCLUDE_DIRS})
target_link_libraries(${CORE_TARGET} PUBLIC ${PNG_LIBRARIES})
if(PNG_DEFINITIONS)
    target_compile_definitions(${CORE_TARGET} PUBLIC ${PNG_DEFINITIONS})
endif()

# Freetype
find_package(Freetype REQUIRED)
target_include_directories(${CORE_TARGET} PUBLIC ${FREETYPE_INCLUDE_DIR_freetype2})
target_link_libraries(${CORE_TARGET} PUBLIC ${FREETYPE_LIBRARIES})

# PNGwriter（使用导入目标）
set(PNGwriter_DIR "${CMAKE_SOURCE_DIR}/pngwriter/install/lib/cmake/PNGwriter")
find_package(PNGwriter 0.7.0 REQUIRED)
target_link_libraries(${CORE_TARGET} PUBLIC PNGwriter::PNGwriter)

# yaml-cpp
pkg_check_modules(YAML_CPP yaml-cpp REQUIRED)
find_library(YAML_CPP_LIBRARY NAMES yaml-cpp yaml-cppd PATHS ${YAML_CPP_LIBRARY_DIRS})
if(YAML_CPP_INCLUDE_DIRS)
    target_include_directories(${CORE_TARGET} PUBLIC ${YAML_CPP_INCLUDE_DIRS})
endif()
if(YAML_CPP_LIBRARY)
    target_link_libraries(${CORE_TARGET} PUBLIC ${YAML_CPP_LIBRARY})
endif()
target_compile_definitions(${CORE_TARGET} PUBLIC YAML_CPP_STATIC_DEFINE)

# PCRE2 或 std::regex（二选一）
if(USING_STD_REGEX STREQUAL "ON")
    target_compile_definitions(${CORE_TARGET} PUBLIC USE_STD_REGEX)
else()
    find_package(PCRE2 REQUIRED)
    target_include_directories(${CORE_TARGET} PUBLIC ${PCRE2_INCLUDE_DIRS})
    target_link_libraries(${CORE_TARGET} PUBLIC ${PCRE2_LIBRARY})
    target_compile_definitions(${CORE_TARGET} PUBLIC PCRE2_STATIC)
endif()

# Windows 专有：套接字库与运行时 DLL 复制
if(WIN32)
    target_link_libraries(${CORE_TARGET} PUBLIC wsock32 ws2_32)

    # 仅当开启时才进行“依赖分析 + DLL 拷贝”
    if(ENABLE_RUNTIME_DLL_COPY)
        # 查找 MinGW-w64 根目录（用于复制运行时 DLL）
        if(DEFINED ENV{MINGW64_ROOT} AND EXISTS "$ENV{MINGW64_ROOT}/bin")
            set(MINGW64_ROOT "$ENV{MINGW64_ROOT}" CACHE PATH "MinGW-w64 root directory from ENV" FORCE)
        elseif(DEFINED MINGW64_ROOT AND EXISTS "${MINGW64_ROOT}/bin")
         elseif(CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
        set(MINGW64_ROOT "${CMAKE_PREFIX_PATH}" CACHE PATH "MinGW-w64 root directory from CMAKE_PREFIX_PATH" FORCE)
        elseif(DEFINED ENV{MSYSTEM_PREFIX} AND EXISTS "$ENV{MSYSTEM_PREFIX}/bin")
        set(MINGW64_ROOT "$ENV{MSYSTEM_PREFIX}" CACHE PATH "MinGW-w64 root directory from ENV{MSYSTEM_PREFIX}" FORCE)
  
        else()
            set(MINGW64_ROOT "" CACHE PATH "MinGW-w64 root directory")
        endif()

        if(MINGW64_ROOT)
            file(TO_CMAKE_PATH "${MINGW64_ROOT}" MINGW64_ROOT)
            message(STATUS "Found MinGW-w64 root at: ${MINGW64_ROOT}")
            set(MINGW_RUNTIME_DIR "${MINGW64_ROOT}/bin")
        else()
            message(WARNING "MinGW-w64 root not found. DLLs may not be copied correctly. Please set MINGW64_ROOT environment variable or CMake variable.")
            set(MINGW_RUNTIME_DIR "")
        endif()

        # 构造运行时 DLL 搜索目录
        set(RUNTIME_DLL_SEARCH_DIRS "")
        if(MINGW_RUNTIME_DIR AND EXISTS "${MINGW_RUNTIME_DIR}")
            list(APPEND RUNTIME_DLL_SEARCH_DIRS "${MINGW_RUNTIME_DIR}")
        endif()       

        # 统一规范化与扩展搜索目录，增强健壮性
        if(RUNTIME_DLL_SEARCH_DIRS)
            set(_normalized_runtime_dirs "")
            foreach(_d IN LISTS RUNTIME_DLL_SEARCH_DIRS)
                if(EXISTS "${_d}")
                    file(TO_CMAKE_PATH "${_d}" _d_norm)
                    list(APPEND _normalized_runtime_dirs "${_d_norm}")
                endif()
            endforeach()
            set(RUNTIME_DLL_SEARCH_DIRS "${_normalized_runtime_dirs}")

            # 允许外部通过 -DRUNTIME_DLL_EXTRA_DIRS="dir1;dir2" 注入更多搜索目录
            set(RUNTIME_DLL_EXTRA_DIRS "" CACHE STRING "Extra directories to search for runtime DLLs")
            if(RUNTIME_DLL_EXTRA_DIRS)
                list(APPEND RUNTIME_DLL_SEARCH_DIRS ${RUNTIME_DLL_EXTRA_DIRS})
            endif()

            list(REMOVE_DUPLICATES RUNTIME_DLL_SEARCH_DIRS)
            message(STATUS "RUNTIME_DLL_SEARCH_DIRS='${RUNTIME_DLL_SEARCH_DIRS}'")

            # 可选的兜底强制复制
            set(RUNTIME_DLL_FORCE_COPY "" CACHE STRING "Absolute paths of DLLs to force-copy (semicolon-separated). Leave empty to disable.")

            add_custom_command(TARGET ${APP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -Dexe_path=$<TARGET_FILE:${APP_TARGET}>
                    -Ddest_dir=${OUTPUT_BIN_DIR}
                    -Dsearch_dirs=${RUNTIME_DLL_SEARCH_DIRS}
                    -Dextra_copy_files=${RUNTIME_DLL_FORCE_COPY}
                    -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_deps.cmake"
                VERBATIM
            )
        else()
            message(WARNING "No runtime DLL search directories found. Skipping DLL copy command.")
        endif()
    endif() # ENABLE_RUNTIME_DLL_COPY
endif()

# 新增：清理目标（清理构建产物与复制的资源：build/bin 和 build/lib）
add_custom_target(clean_outputs
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${OUTPUT_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_BINARY_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
    COMMENT "Clean: removed and recreated ${OUTPUT_BIN_DIR} and ${CMAKE_BINARY_DIR}/lib"
)