#!/usr/bin/env bash
set -euo pipefail

# 将 /d/... 或反斜杠路径规范为 Windows 正斜杠 (D:/...)
to_win_fwd() {
  local p="$1"
  p="${p//\\//}"  # 反斜杠转正斜杠
  if [[ "$p" =~ ^/([a-zA-Z])/(.*)$ ]]; then
    local d="${BASH_REMATCH[1]}"
    local rest="${BASH_REMATCH[2]}"
    printf "%s:/%s" "${d^^}" "$rest"
    return 0
  fi
  printf "%s" "$p"
}

# 项目根目录（POSIX）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
# 可执行文件名提取为变量，避免硬编码
EXE_BASENAME="stairspeedtest"
EXE_NAME="${EXE_BASENAME}.exe"

# MSYS2 根目录（优先环境变量，回退到 Scoop 默认）
MSYS2_ROOT_RAW="${MSYS2_ROOT:-D:/Programs/scoop/apps/msys2/current}"
MSYS2_ROOT="$(to_win_fwd "$MSYS2_ROOT_RAW")"

MINGW_PREFIX="${MSYS2_ROOT}/mingw64"
CC_PATH="${MINGW_PREFIX}/bin/gcc.exe"
CXX_PATH="${MINGW_PREFIX}/bin/g++.exe"
MAKE_PATH="${MINGW_PREFIX}/bin/mingw32-make.exe"
GENERATOR="MinGW Makefiles"

configure() {
  echo "[configure] 清理 CMake 缓存并配置 Debug 构建..."
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"
  export MSYS2_ARG_CONV_EXCL="CMAKE_PREFIX_PATH;CMAKE_C_COMPILER;CMAKE_CXX_COMPILER;CMAKE_MAKE_PROGRAM;MINGW64_ROOT;RUNTIME_DLL_EXTRA_DIRS"

  # 配置阶段 PATH：不含 usr/bin
  export PATH="${MSYS2_ROOT}/cmakeproxy:${MINGW_PREFIX}/bin:${MSYS2_ROOT}/bin:${PATH}"

  mkdir -p "${BUILD_DIR}"
  rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles"

  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -G "${GENERATOR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="${MINGW_PREFIX}" \
    -DMINGW64_ROOT="${MINGW_PREFIX}" \
    -DENABLE_RUNTIME_DLL_COPY=ON \
    -DRUNTIME_DLL_EXTRA_DIRS="${MSYS2_ROOT}/bin;${MSYS2_ROOT}/usr/bin" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_COMPILER="${CC_PATH}" \
    -DCMAKE_CXX_COMPILER="${CXX_PATH}" \
    -DCMAKE_MAKE_PROGRAM="${MAKE_PATH}" \
    -DCMAKE_SH=CMAKE_SH-NOTFOUND
}

build() {
  echo "[build] 编译项目（Debug）..."
 
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"
  export MSYS2_ARG_CONV_EXCL="CMAKE_PREFIX_PATH;CMAKE_C_COMPILER;CMAKE_CXX_COMPILER;CMAKE_MAKE_PROGRAM"

  # 构建阶段 PATH：加入 usr/bin
  export PATH="${MSYS2_ROOT}/cmakeproxy:${MINGW_PREFIX}/bin:${MSYS2_ROOT}/usr/bin:${MSYS2_ROOT}/bin:${PATH}"

  cmake --build "${BUILD_DIR}" --config Debug -j
  local exe="${BUILD_DIR}/bin/${EXE_NAME}"
  if [ -f "${exe}" ]; then
    echo "[build] 生成可执行文件：${exe}"
  fi
}

run_cli() {
  echo "[run] 运行 CLI 可执行文件..."
  #unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH PKG_CONFIG_PATH || true
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"
  # 禁用 MSYS2 参数路径转换，避免将 "/u" 与 URL 转换成路径
  export MSYS2_ARG_CONV_EXCL="*"
  export PATH="${MSYS2_ROOT}/cmakeproxy:${MINGW_PREFIX}/bin:${MSYS2_ROOT}/usr/bin:${MSYS2_ROOT}/bin:${PATH}"

  local exe="${BUILD_DIR}/bin/${EXE_NAME}"
  if [ ! -f "${exe}" ]; then
    echo "[run] 未找到可执行文件，正在编译..."
    build
  fi
  # 与 VSCode launch.json 的控制台模式一致：传入 /u <URL>
  local cli_args=("/u" "https://gitee.com/amessboy/DeployData/raw/main/result2")
  (cd "${BUILD_DIR}/bin" && ./${EXE_NAME} "${cli_args[@]}")
}

run_web() {
  echo "[run-web] 编译后的 Web 模式启动..."
  #unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH PKG_CONFIG_PATH || true
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"
  # 禁用 MSYS2 参数路径转换，避免将 "/web" 当作路径进行转换
  export MSYS2_ARG_CONV_EXCL="*"
  export PATH="${MSYS2_ROOT}/cmakeproxy:${MINGW_PREFIX}/bin:${MSYS2_ROOT}/usr/bin:${MSYS2_ROOT}/bin:${PATH}"

  local exe="${BUILD_DIR}/bin/${EXE_NAME}"
  if [ ! -f "${exe}" ]; then
    echo "[run-web] 未找到可执行文件，正在编译..."
    build
  fi

  # 按 VSCode 的 web 调试配置：cwd 指向 build/bin 并传入 /web
  local web_cwd="${BUILD_DIR}/bin"
  local url="http://127.0.0.1:10870"
  echo "[run-web] 工作目录：${web_cwd}，静态资源：${web_cwd}/webui"
  (cd "${web_cwd}" && ./${EXE_NAME} /web) &
  local server_pid=$!

  # 打开默认浏览器访问 Web 页面
  if command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoProfile -Command "Start-Process '${url}'" >/dev/null 2>&1 || true
  elif command -v cmd.exe >/dev/null 2>&1; then
    cmd.exe /c start "" "${url}" >/dev/null 2>&1 || true
  else
    echo "[run-web] 请手动打开：${url}"
  fi

  # 等待 Web 服务器结束（Ctrl+C 结束）
  wait "${server_pid}"
}

clean_and_build() {
  echo "[clean+build] 清理并编译（Debug）..."
  rm -rf "${BUILD_DIR}"
  configure
  build
}

build_and_run_cli() {
  build
  run_cli
}

build_and_run_web() {
  build
  run_web
}

show_menu() {
  echo "========== StairSpeedTest Debug 构建 =========="
  echo "MSYS2_ROOT = ${MSYS2_ROOT}"
  echo "1) 清理并编译"
  echo "2) 编译并以命令行方式运行"
  echo "3) 编译并以 Web 方式运行"
  echo "=============================================="
  read -r -p "请输入数字 [1-3]: " choice
  case "${choice}" in
    1) clean_and_build ;;
    2) build_and_run_cli ;;
    3) build_and_run_web ;;
    *) echo "无效选择"; exit 1 ;;
  esac
}

main() {
  if [ $# -ge 1 ]; then
    case "$1" in
      1) clean_and_build ;;
      2) build_and_run_cli ;;
      3) build_and_run_web ;;
      *) show_menu ;;
    esac
  else
    show_menu
  fi
}

main "$@"