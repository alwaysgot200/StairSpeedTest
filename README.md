# StairSpeedTest

批量测试多种代理协议性能的工具，支持命令行与内置 Web 界面两种运行方式，能够读取订阅链接或本地配置，自动完成测速并导出结果图片与日志。

> 原项目基础：根据 [StairSpeedtest](https://github.com/tindy2013/stairspeedtest-reborn) 重构与增强，新增 VLESS、并发等能力并修复多处问题。

---

## 功能与优势

- 多协议支持：SSR、SS、VMESS、VLESS、Trojan、SOCKS5
- 支持订阅批量测速：可一次性测试多个订阅或节点文件
- 并发测速：多线程加速，缩短整体测试时间
- 内置 Web GUI：零依赖网页界面，贴入订阅即可启动测速
- 结果导出：将测试结果导出为 PNG 图片，并生成详细日志
- 灵活输入：控制台交互、URL 参数、文件路径，支持多输入用 `|` 分隔
- 可配置：通过 `pref.ini` 调整监听地址/端口、并发线程数、导出策略、排序、配色等

## 平台支持

已在以下环境验证：

- Windows 10/11（含 MSYS2/MINGW64 环境）
- Ubuntu、Debian、CentOS 等 Linux 发行版
- macOS 10.13+（Intel）
- Android（Termux）、iOS/iPadOS（iSH，性能较差，仅测试）
- Raspberry Pi（armv7l）

> 注：在 Windows 上，若直接运行 `exe` 提示缺少 DLL，建议在 MSYS2 MINGW64 终端运行或将依赖 DLL 放到程序根目录，或把 `MINGW64\bin` 加到 `PATH`。

---

## 构建指南

### Windows（推荐）

前置条件：已安装 [MSYS2](https://www.msys2.org/) 并具备 `MINGW64` 工具链。

- 方式 A：一键脚本

  - 在 MSYS2 MINGW64 终端进入项目根目录，执行：
    ```bash
    bash build.sh 1   # 清理并编译（Debug）
    ```
  - 可选：
    ```bash
    bash build.sh 2   # 编译并以命令行模式运行（/u URL）
    bash build.sh 3   # 编译并以 Web 方式运行（/web）
    ```

- 方式 B：手动 CMake 构建（示例）
  ```bash
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
  cmake --build build --config Debug -j
  ```

> 说明：构建后可执行文件位于 `build/bin/`；构建脚本会将 `base/`（含 `webui/`）复制到该目录，Web 静态资源路径为 `build/bin/webui/`。

### Linux/macOS

确保依赖安装（curl、openssl、PNGwriter、libpng、freetype、zlib、yaml-cpp、libevent、pcre2、rapidjson 等），随后：

```bash
cmake -S . -B build
cmake --build build -j
```

---

## 运行方法

### 命令行模式（CLI）

- 交互运行（启动后在控制台粘贴订阅/链接）
  ```bash
  ./stairspeedtest.exe
  ```
- 直接传入订阅或本地文件（推荐）
  ```bash
  ./stairspeedtest.exe /u "https://example.com/subscription.txt"
  ./stairspeedtest.exe /u "e:\_cpp_work\StairSpeedTest\res\sample.txt"
  ```
- 指定分组名（导出图片的分组标题）
  ```bash
  ./stairspeedtest.exe /u "https://example.com/subscription.txt" /g "我的分组"
  ```

> VSCode 调试：仓库内 `.vscode/launch.json` 已配置 CLI 运行示例，`build.sh` 的 `run_cli` 也与其保持一致。

### Web 界面模式（推荐）

- 启动内置 WebServer：
  ```bash
  ./stairspeedtest.exe /web
  ```
- 或使用脚本：
  - Windows：双击 `base/webserver.bat`
  - 其他平台：`bash base/webserver.sh`
- 打开浏览器访问：`http://127.0.0.1:10870`

> 监听地址与端口在 `pref.ini` 的 `[webserver]` 段可配置（默认 `listen_address = 127.0.0.1`，`listen_port = 10870`）。Web 静态页面位于 `webui/`。

### 一键运行（脚本）

- `bash build.sh 2`：编译并以命令行模式运行（按 VSCode 配置传入 `/u <URL>`）
- `bash build.sh 3`：编译并以 Web 方式运行（工作目录为 `build/bin`，自动打开浏览器）

---

## 测试节点使用方法

支持在控制台或网页直接粘贴以下内容（也可使用 `/u` 传参）：

- 单节点链接：`vmess://`、`vmess1://`、`ss://`、`ssr://`、`trojan://`、`socks://`、`https://t.me/socks`、`tg://socks`
- 订阅链接：`http://` 或 `https://`（支持 `surge:///install-config`）
- 本地文件路径：指向订阅或节点配置文件，程序会读取并解析
- 多个链接/订阅：使用 `|` 分隔，例如：`"link1|link2|link3"`

结果输出位置：

- 图片：`results/YYYYMMDD-HHMMSS.png`（分组测试生成多图）
- 日志：`logs/YYYYMMDD-HHMMSS.log`
- 单链接也会导出图片（由 `pref.ini` 中 `export.single_test_force_export` 控制，默认 `true`）

---

## 常见问题与注意事项

- 依赖缺失（Windows）：如提示缺少 `libnghttp2`、`libidn2`、`libunistring`、`zstd`、`zlib`、`brotli` 等 DLL，可在 MSYS2 MINGW64 终端运行可执行文件，或将相关 DLL 放至程序根目录，或将 `MINGW64\bin` 添加到 `PATH`。
- 外部客户端：测试 `vmess/ss/ssr/trojan` 时，程序会尝试从 `tools/clients/` 启动对应客户端（`v2ray.exe`、`ss-local.exe`、`ssr-local.exe`、`trojan.exe`）。缺失时会提示，不影响 `socks`。
- 本地 SOCKS 端口：默认 `32768`，遇占用会自动避让。
- Web 资源路径：构建后位于 `build/bin/webui/`；若手动运行，请保证工作目录指向含 `webui/` 的路径。
- VSCode 集成：`tasks.json` 和 `launch.json` 已与脚本适配，`build.sh` 的 `run_cli`/`run_web` 使用相同参数与工作目录。
- 避免环境包含路径干扰标准头的 include_next（stdlib.h）
  unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH PKG_CONFIG_PATH || true

---

## 未完成的工作（TODO）

- 为 `ss/trojan/socks` 增加更完善的并发支持
- 增加对 `hy2` 的支持（计划引入 `singbox`）
- 增加对 `netflex/gemini` 探测并分类输出结果
- Web 界面进一步优化与交互增强

---

## 许可证

本项目使用 MIT 许可证，详见 [LICENSE](./LICENSE)。
