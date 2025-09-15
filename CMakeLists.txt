cmake_minimum_required(VERSION 3.5)
project(stairspeedtest LANGUAGES CXX)

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
add_library(stairspeed_core STATIC ${SRC_ALL})
target_include_directories(stairspeed_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include"
)

# 主程序，仅包含 main.cpp，并只链接核心库
add_executable(stairspeedtest "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(stairspeedtest PRIVATE stairspeed_core)

# 根据开关为主程序附加可选模块
if(BUILD_PERF_TOOLS)
    target_sources(stairspeedtest PRIVATE ${OPTIONAL_SOURCES})
    target_compile_definitions(stairspeedtest PRIVATE ENABLE_PERF_TOOLS=1)
else()
    target_compile_definitions(stairspeedtest PRIVATE ENABLE_PERF_TOOLS=0)
endif()

# 将运行时资源复制到可执行文件所在目录（例如 build 输出目录）
add_custom_command(TARGET stairspeedtest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/base/pref.ini"
            "$<TARGET_FILE_DIR:stairspeedtest>/pref.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/base/config.json"
            "$<TARGET_FILE_DIR:stairspeedtest>/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/base/webui"
            "$<TARGET_FILE_DIR:stairspeedtest>/webui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/base/tools"
            "$<TARGET_FILE_DIR:stairspeedtest>/tools"
)

# 依赖收集（全部挂到 stairspeed_core，避免全局污染）
find_package(PkgConfig REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(stairspeed_core PUBLIC Threads::Threads)

# libevent（使用 pkg-config 辅助定位）
pkg_check_modules(LIBEVENT libevent REQUIRED)
find_path(LIBEVENT_INCLUDE_DIR NAMES event.h PATHS ${LIBEVENT_INCLUDE_DIRS})
find_library(LIBEVENT_LIBRARY NAMES event PATHS ${LIBEVENT_LIBRARY_DIRS})
if(LIBEVENT_INCLUDE_DIR)
    target_include_directories(stairspeed_core PUBLIC ${LIBEVENT_INCLUDE_DIR})
endif()
if(LIBEVENT_LIBRARY)
    target_link_libraries(stairspeed_core PUBLIC ${LIBEVENT_LIBRARY})
endif()

# CURL
find_package(CURL 7.54.0 REQUIRED)
target_link_libraries(stairspeed_core PUBLIC CURL::libcurl)
# 如果你确定使用静态 libcurl 再启用下一行；否则不要定义
# target_compile_definitions(stairspeed_core PUBLIC CURL_STATICLIB)

# OpenSSL
find_package(OpenSSL 1.1.0 REQUIRED)
target_include_directories(stairspeed_core PUBLIC ${OPENSSL_INCLUDE_DIR})
target_link_libraries(stairspeed_core PUBLIC ${OPENSSL_LIBRARIES})

# Rapidjson（header-only）
find_package(Rapidjson REQUIRED)
target_include_directories(stairspeed_core PUBLIC ${RAPIDJSON_INCLUDE_DIRS})

# libpng
find_package(PNG REQUIRED)
target_include_directories(stairspeed_core PUBLIC ${PNG_INCLUDE_DIRS})
target_link_libraries(stairspeed_core PUBLIC ${PNG_LIBRARIES})
if(PNG_DEFINITIONS)
    target_compile_definitions(stairspeed_core PUBLIC ${PNG_DEFINITIONS})
endif()

# Freetype
find_package(Freetype REQUIRED)
target_include_directories(stairspeed_core PUBLIC ${FREETYPE_INCLUDE_DIR_freetype2})
target_link_libraries(stairspeed_core PUBLIC ${FREETYPE_LIBRARIES})

# PNGwriter（使用导入目标）
set(PNGwriter_DIR "${CMAKE_SOURCE_DIR}/pngwriter/install/lib/cmake/PNGwriter")
find_package(PNGwriter 0.7.0 REQUIRED)
target_link_libraries(stairspeed_core PUBLIC PNGwriter::PNGwriter)

# yaml-cpp
pkg_check_modules(YAML_CPP yaml-cpp REQUIRED)
find_library(YAML_CPP_LIBRARY NAMES yaml-cpp yaml-cppd PATHS ${YAML_CPP_LIBRARY_DIRS})
if(YAML_CPP_INCLUDE_DIRS)
    target_include_directories(stairspeed_core PUBLIC ${YAML_CPP_INCLUDE_DIRS})
endif()
if(YAML_CPP_LIBRARY)
    target_link_libraries(stairspeed_core PUBLIC ${YAML_CPP_LIBRARY})
endif()
target_compile_definitions(stairspeed_core PUBLIC YAML_CPP_STATIC_DEFINE)

# PCRE2 或 std::regex（二选一）
if(USING_STD_REGEX STREQUAL "ON")
    target_compile_definitions(stairspeed_core PUBLIC USE_STD_REGEX)
else()
    find_package(PCRE2 REQUIRED)
    target_include_directories(stairspeed_core PUBLIC ${PCRE2_INCLUDE_DIRS})
    target_link_libraries(stairspeed_core PUBLIC ${PCRE2_LIBRARY})
    target_compile_definitions(stairspeed_core PUBLIC PCRE2_STATIC)
endif()

# Windows 专有：套接字库与运行时 DLL 复制
if(WIN32)
    target_link_libraries(stairspeed_core PUBLIC wsock32 ws2_32)

    # 查找 MinGW-w64 根目录（用于复制运行时 DLL）
    if(DEFINED ENV{MINGW64_ROOT} AND EXISTS "$ENV{MINGW64_ROOT}/bin")
        set(MINGW64_ROOT "$ENV{MINGW64_ROOT}" CACHE PATH "MinGW-w64 root directory from ENV" FORCE)
    elseif(DEFINED MINGW64_ROOT AND EXISTS "${MINGW64_ROOT}/bin")
        # 已设置 MINGW64_ROOT
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

    if(DEFINED ENV{MSYSTEM_PREFIX} AND EXISTS "$ENV{MSYSTEM_PREFIX}/bin")
        list(APPEND RUNTIME_DLL_SEARCH_DIRS "$ENV{MSYSTEM_PREFIX}/bin")
    endif()

    if(RUNTIME_DLL_SEARCH_DIRS)
        list(REMOVE_DUPLICATES RUNTIME_DLL_SEARCH_DIRS)
        message(STATUS "RUNTIME_DLL_SEARCH_DIRS='${RUNTIME_DLL_SEARCH_DIRS}'")

        add_custom_command(TARGET stairspeedtest POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -Dexe_path=$<TARGET_FILE:stairspeedtest>
                -Ddest_dir=$<TARGET_FILE_DIR:stairspeedtest>
                -Dsearch_dirs="${RUNTIME_DLL_SEARCH_DIRS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_deps.cmake"
            VERBATIM
        )
    else()
        message(WARNING "No runtime DLL search directories found. Skipping DLL copy command.")
    endif()

else()
    include(GNUInstallDirs)
    install(TARGETS stairspeedtest DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()