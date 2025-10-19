#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
# 允许通过环境变量覆盖生成器，默认使用 MinGW Makefiles
GENERATOR="${CMAKE_GENERATOR:-MinGW Makefiles}"

red() { printf "\033[31m%s\033[0m\n" "$*"; }
green() { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }

# 自动解析 MSYS2 根目录，优先使用 scoop 的 current
resolve_msys2_root_auto() {
  if [[ -n "${MSYS2_ROOT:-}" ]]; then
    local root="${MSYS2_ROOT//\\//}"
    # 如果是日期版本，且存在 current，则切到 current
    local maybe_current
    maybe_current="$(echo "$root" | sed -E 's#/([0-9]{4}-[0-9]{2}-[0-9]{2})$#/current#')"
    if [[ "$maybe_current" != "$root" && -d "$maybe_current" ]]; then
      MSYS2_ROOT="$maybe_current"
    else
      MSYS2_ROOT="$root"
    fi
    return 0
  fi
  if command -v where.exe >/dev/null 2>&1; then
    local gxx
    gxx="$(where.exe g++.exe 2>/dev/null | grep -i -E "\\\\msys2\\\\current\\\\mingw64\\\\bin\\\\g\+\+\.exe|\\\\msys64\\\\mingw64\\\\bin\\\\g\+\+\.exe" | head -n 1)"
    if [[ -n "$gxx" ]]; then
      local gxx_unix="${gxx//\\//}"
      local bin_dir
      bin_dir="$(dirname "$gxx_unix")"                # .../mingw64/bin
      local mingw_dir
      mingw_dir="$(dirname "$bin_dir")"                # .../mingw64
      local msys2_root
      msys2_root="$(dirname "$mingw_dir")"             # .../msys2/current 或 .../msys64
      MSYS2_ROOT="$msys2_root"
      return 0
    fi
  fi
  return 1
}

ensure_msys2_root() {
  # 优先自动解析；失败则报错并提示
  if ! resolve_msys2_root_auto; then
    red "无法自动解析 MSYS2_ROOT。请在系统环境中设置，例如："
    echo "  setx MSYS2_ROOT \"D:/Programs/scoop/apps/msys2/current\""
    exit 1
  fi
  # 统一斜杠
  MSYS2_ROOT_UNIX="${MSYS2_ROOT//\\//}"
}



configure() {
  ensure_msys2_root
  # 与 VSCode Configure 阶段一致：去掉 usr/bin，避免 CMake 发现 sh.exe 选择 MSYS Makefiles
  export PATH="${MSYS2_ROOT_UNIX}/cmakeproxy:${MSYS2_ROOT_UNIX}/mingw64/bin:${MSYS2_ROOT_UNIX}/bin:${PATH}"
  export TERM="xterm-256color"
  export CLICOLOR="1"
  export CLICOLOR_FORCE="1"
  export GCC_COLORS="error=01;31:warning=01;35:note=01;36:locus=00;34:caret=01;32:quote=01"
  # 与 VSCode 调试保持一致，尽量使用 MinGW64 环境
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"
  # 避免外部环境干扰包含路径（VSCode 任务未设置这些变量）
  unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH PKG_CONFIG_PATH || true
  # 禁止 MSYS2 自动转换以下 CMake 参数路径，确保保持 Windows 格式
  export MSYS2_ARG_CONV_EXCL="CMAKE_PREFIX_PATH;CMAKE_C_COMPILER;CMAKE_CXX_COMPILER;CMAKE_MAKE_PROGRAM"

  # 统一清理旧的 CMake 动态文件，避免遗留路径/状态干扰
  mkdir -p "${BUILD_DIR}"
  rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles"

  # 仅在使用 MinGW Makefiles 时禁用 CMAKE_SH，避免 cmake 误用 sh
  local sh_flag=()
  if [[ "${GENERATOR}" == "MinGW Makefiles" ]]; then
    sh_flag=(-DCMAKE_SH=CMAKE_SH-NOTFOUND)
  fi

  # 使用正斜杠形式的 Windows 路径，避免 \d 这样的非法前缀
  local MINGW_PREFIX_WIN_FWD="${MSYS2_ROOT_UNIX}/mingw64"
  local CC_PATH="${MINGW_PREFIX_WIN_FWD}/bin/gcc.exe"
  local CXX_PATH="${MINGW_PREFIX_WIN_FWD}/bin/g++.exe"
  local MAKE_PATH="${MINGW_PREFIX_WIN_FWD}/bin/mingw32-make.exe"

  yellow "配置 CMake (Debug, ${GENERATOR})…"
  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -G "${GENERATOR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="${MINGW_PREFIX_WIN_FWD}" \
    -DCMAKE_C_COMPILER="${CC_PATH}" \
    -DCMAKE_CXX_COMPILER="${CXX_PATH}" \
    -DCMAKE_MAKE_PROGRAM="${MAKE_PATH}" \
    -DENABLE_RUNTIME_DLL_COPY=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_COLOR_MAKEFILE=ON \
    -DCMAKE_C_FLAGS="-fdiagnostics-color=always" \
    -DCMAKE_CXX_FLAGS="-fdiagnostics-color=always" \
    -DCMAKE_COLOR_DIAGNOSTICS=ON \
    "${sh_flag[@]}"
}

build() {
  ensure_msys2_root
  # 与 VSCode Build 阶段一致：加入 usr/bin
  export PATH="${MSYS2_ROOT_UNIX}/cmakeproxy:${MSYS2_ROOT_UNIX}/mingw64/bin:${MSYS2_ROOT_UNIX}/usr/bin:${MSYS2_ROOT_UNIX}/bin:${PATH}"
  export TERM="xterm-256color"
  export CLICOLOR="1"
  export CLICOLOR_FORCE="1"
  export GCC_COLORS="error=01;31:warning=01;35:note=01;36:locus=00;34:caret=01;32:quote=01"
  export MSYSTEM="MINGW64"
  export MSYS2_PATH_TYPE="inherit"

  yellow "开始编译…"
  cmake --build "${BUILD_DIR}" -j
  green "编译完成：${BIN_DIR}/stairspeedtest.exe"
}

run_cli() {
  ensure_msys2_root
  export PATH="${MSYS2_ROOT_UNIX}/cmakeproxy:${MSYS2_ROOT_UNIX}/mingw64/bin:${MSYS2_ROOT_UNIX}/usr/bin:${MSYS2_ROOT_UNIX}/bin:${PATH}"
  yellow "以命令行方式运行…"
  pushd "${BIN_DIR}" >/dev/null
  ./stairspeedtest.exe /u "https://gitee.com/amessboy/DeployData/raw/main/result2"
  popd >/dev/null
}

run_web() {
  ensure_msys2_root
  export PATH="${MSYS2_ROOT_UNIX}/cmakeproxy:${MSYS2_ROOT_UNIX}/mingw64/bin:${MSYS2_ROOT_UNIX}/usr/bin:${MSYS2_ROOT_UNIX}/bin:${PATH}"
  yellow "以 Web 方式运行…"
  # 先尝试打开浏览器
  if command -v explorer.exe >/dev/null 2>&1; then
    explorer.exe "http://127.0.0.1:10870" >/dev/null 2>&1 || true
  fi
  pushd "${BIN_DIR}" >/dev/null
  ./stairspeedtest.exe /web
  popd >/dev/null
}

main() {
  local choice="${1:-}"
  if [[ -z "${choice}" ]]; then
    echo "请选择操作："
    echo "1) 编译（Configure + Build）"
    echo "2) 编译并以命令行方式运行"
    echo "3) 编译并以 Web 方式运行"
    read -rp "输入选项 [1-3]: " choice
  else
    echo "选择了选项：${choice}"
  fi

  case "${choice}" in
    1)
      configure
      build
      ;;
    2)
      configure
      build
      run_cli
      ;;
    3)
      configure
      build
      run_web
      ;;
    *)
      red "无效选项：${choice}"
      exit 1
      ;;
  esac
}

main "$@"