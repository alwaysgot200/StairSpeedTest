cmake_minimum_required(VERSION 3.5)
project(stairspeedtest LANGUAGES CXX)
SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
INCLUDE_DIRECTORIES("${CMAKE_SOURCE_DIR}/include/")
# Add system include directories
# include_directories(SYSTEM "/d/Programs/scoop/apps/msys2/2025-08-30/mingw64/include")
# include_directories(SYSTEM "/d/Programs/scoop/apps/msys2/2025-08-30/mingw64/x86_64-w64-mingw32/include")

# Fix for GCC bug #70129 - prevents stdlib.h: No such file or directory
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
IF(NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
ENDIF()
SET(CMAKE_CXX_STANDARD 17)
ADD_DEFINITIONS(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result)

OPTION(USING_STD_REGEX "Use std::regex from C++ library instead of PCRE2." OFF)

INCLUDE(CheckCXXSourceCompiles)
CHECK_CXX_SOURCE_COMPILES(
"
#include<string>
void main(){std::to_string(0);}
" HAVE_TO_STRING)

INCLUDE(CheckSymbolExists)
CHECK_SYMBOL_EXISTS(MSG_NOSIGNAL "sys/socket.h" HAVE_MSG_NOSIGNAL)

IF(APPLE)
    ADD_DEFINITIONS(-D_MACOS)
ENDIF()

IF(HAVE_TO_STRING)
    ADD_DEFINITIONS(-DHAVE_TO_STRING)
ENDIF()

IF(HAVE_MSG_NOSIGNAL)
	ADD_DEFINITIONS(-DHAVE_MSG_NOSIGNAL)
ENDIF()

# 指定 MinGW 运行时 DLL 所在目录（为你当前机器的 Scoop MSYS2 默认路径，可按需修改或在配置时用 -D 覆盖）
set(MINGW_RUNTIME_DIR "D:/Programs/scoop/apps/msys2/current/mingw64/bin" CACHE PATH "MinGW runtime bin directory (e.g. D:/Programs/scoop/apps/msys2/current/mingw64/bin)")

ADD_EXECUTABLE(stairspeedtest 
	src/confbuild.cpp
	src/geoip.cpp
	src/logger.cpp
	src/main.cpp
	src/md5.cpp
	src/misc.cpp
	src/multithread_test.cpp
	src/ntt.cpp
	src/printmsg.cpp
	src/processes.cpp
	src/renderer.cpp
	src/rulematch.cpp
	src/socket.cpp
	src/speedtestutil.cpp
	src/tcping.cpp
	src/webget.cpp
	src/webgui_wrapper.cpp
	src/webserver_libevent.cpp)
# 将运行时资源复制到可执行文件所在目录（例如 build.debug）
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
INCLUDE_DIRECTORIES(src)
LINK_DIRECTORIES(${CMAKE_SOURCE_DIR})

FIND_PACKAGE(PkgConfig REQUIRED)

SET(THREADS_PREFER_PTHREAD_FLAG ON)
FIND_PACKAGE(Threads REQUIRED)
TARGET_LINK_LIBRARIES(stairspeedtest ${CMAKE_THREAD_LIBS_INIT})

PKG_CHECK_MODULES(LIBEVENT libevent REQUIRED)
FIND_PATH(LIBEVENT_INCLUDE_DIR NAMES event.h PATHS ${LIBEVENT_INCLUDE_DIRS})
FIND_LIBRARY(LIBEVENT_LIBRARY NAMES event PATHS ${LIBEVENT_LIBRARY_DIRS})
LINK_DIRECTORIES(${LIBEVENT_LIBRARY_DIRS})
INCLUDE_DIRECTORIES(${LIBEVENT_INCLUDE_DIR})
TARGET_LINK_LIBRARIES(stairspeedtest ${LIBEVENT_LIBRARY})

FIND_PACKAGE(CURL 7.54.0 REQUIRED)
LINK_DIRECTORIES(${CURL_LIBRARY_DIRS})
INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(stairspeedtest CURL::libcurl)
ADD_DEFINITIONS(-DCURL_STATICLIB)

FIND_PACKAGE(OpenSSL 1.1.0 REQUIRED)
INCLUDE_DIRECTORIES(${OPENSSL_INCLUDE_DIR})
TARGET_LINK_LIBRARIES(stairspeedtest ${OPENSSL_LIBRARIES})

FIND_PACKAGE(Rapidjson REQUIRED)
INCLUDE_DIRECTORIES(${RAPIDJSON_INCLUDE_DIRS})

FIND_PACKAGE(PNG REQUIRED)
INCLUDE_DIRECTORIES(${PNG_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(stairspeedtest ${PNG_LIBRARIES})
ADD_DEFINITIONS(${PNG_DEFINITIONS})

FIND_PACKAGE(Freetype REQUIRED)
INCLUDE_DIRECTORIES(${FREETYPE_INCLUDE_DIR_freetype2})
TARGET_LINK_LIBRARIES(stairspeedtest ${FREETYPE_LIBRARIES})

# Add this line before the FIND_PACKAGE(PNGwriter) call
set(PNGwriter_DIR "C:/Users/HP/Downloads/stairspeedtest-reborn-master/pngwriter/install/lib/cmake/PNGwriter")
FIND_PACKAGE(PNGwriter 0.7.0 REQUIRED)
INCLUDE_DIRECTORIES(${PNGwriter_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(stairspeedtest PNGwriter::PNGwriter)

PKG_CHECK_MODULES(YAML_CPP yaml-cpp REQUIRED)
FIND_LIBRARY(YAML_CPP_LIBRARY NAMES yaml-cpp yaml-cppd PATHS ${YAML_CPP_LIBRARY_DIRS})
LINK_DIRECTORIES(${YAML_CPP_LIBRARY_DIRS})
INCLUDE_DIRECTORIES(${YAML_CPP_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(stairspeedtest ${YAML_CPP_LIBRARY})
ADD_DEFINITIONS(-DYAML_CPP_STATIC_DEFINE)

IF(USING_STD_REGEX STREQUAL "ON")
    ADD_DEFINITIONS(-DUSE_STD_REGEX)
ELSE()
    FIND_PACKAGE(PCRE2 REQUIRED)
    INCLUDE_DIRECTORIES(${PCRE2_INCLUDE_DIRS})
    TARGET_LINK_LIBRARIES(stairspeedtest ${PCRE2_LIBRARY})
    ADD_DEFINITIONS(-DPCRE2_STATIC)
ENDIF()

IF(WIN32)
    TARGET_LINK_LIBRARIES(stairspeedtest wsock32 ws2_32)

    # 自动收集并拷贝运行期依赖 DLL（构建后执行）
    # 若未通过 -DMINGW_RUNTIME_DIR 指定，尝试从环境或 CMAKE_PREFIX_PATH 推断（作为兜底）
    if(NOT DEFINED MINGW_RUNTIME_DIR)
        if(DEFINED ENV{MSYSTEM_PREFIX} AND EXISTS "$ENV{MSYSTEM_PREFIX}/bin")
            set(MINGW_RUNTIME_DIR "$ENV{MSYSTEM_PREFIX}/bin" CACHE PATH "Path to MinGW runtime bin" FORCE)
        elseif(CMAKE_PREFIX_PATH AND EXISTS "${CMAKE_PREFIX_PATH}/bin")
            set(MINGW_RUNTIME_DIR "${CMAKE_PREFIX_PATH}/bin" CACHE PATH "Path to MinGW runtime bin" FORCE)
        else()
            set(MINGW_RUNTIME_DIR "" CACHE PATH "Path to MinGW runtime bin")
        endif()
    endif()
    message(STATUS "MINGW_RUNTIME_DIR='${MINGW_RUNTIME_DIR}'")

    # 构造运行期 DLL 搜索目录列表（优先使用 MINGW_RUNTIME_DIR）
    set(RUNTIME_DLL_SEARCH_DIRS "")
    if(MINGW_RUNTIME_DIR)
        list(APPEND RUNTIME_DLL_SEARCH_DIRS "${MINGW_RUNTIME_DIR}")
    endif()
    if(DEFINED ENV{MSYSTEM_PREFIX})
        list(APPEND RUNTIME_DLL_SEARCH_DIRS "$ENV{MSYSTEM_PREFIX}/bin")
    endif()
    list(APPEND RUNTIME_DLL_SEARCH_DIRS "/mingw64/bin")
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
ELSE()
    INCLUDE(GNUInstallDirs)
    INSTALL(TARGETS stairspeedtest DESTINATION ${CMAKE_INSTALL_BINDIR})
ENDIF()
