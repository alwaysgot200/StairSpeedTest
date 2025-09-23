# StairSpeedtest

**Proxy performance batch tester based on Shadowsocks(R), V2Ray and Trojan**
[![Build Status](https://github.com/alwaysgot200/StairSpeedTest/actions/workflows/build.yml/badge.svg)](https://github.com/alwaysgot200/StairSpeedTest/actions/workflows/build.yml)
[![GitHub tag (latest SemVer)](https://img.shields.io/github/tag/alwaysgot200/StairSpeedTest.svg)](https://github.com/alwaysgot200/StairSpeedTest/tags)
[![GitHub release](https://img.shields.io/github/release/alwaysgot200/StairSpeedTest.svg)](https://github.com/alwaysgot200/StairSpeedTest/releases)
[![GitHub license](https://img.shields.io/github/license/alwaysgot200/StairSpeedTest.svg)](https://github.com/alwaysgot200/StairSpeedTest/blob/master/LICENSE)

## Intro

I reconstruct it according to original [StairSpeedtest](https://github.com/tindy2013/stairspeedtest-reborn).

- Add VLESS support.
- Add Multi-Threads support
- Fix some bugs

## Installation

### Prebuilt release

Go to [Release Page](https://github.com/alwaysgot200/StairSpeedTest/releases).

### Build

In general, you need the following build dependencies:

- curl
- openssl
- PNGwriter
- libpng
- freetype
- zlib
- yaml-cpp
- libevent
- pcre2
- rapidjson

On non-Windows platforms, you also need to have the following clients installed to 'tools/clients/':

- shadowsocks-libev
- shadowsocksr-libev ('ss-local' installed as the name 'ssr-local')
- v2ray 4.45.2
- trojan

After installing all these dependencies, you can use CMake to configure and build:

```bash
cmake .
make -j
```

## Usage

- Run "stairspeedtest" for CLI speedtest, run "webgui" for Web GUI speedtest.
- Results for subscribe link tests will be saved to a log file in "results" folder.
- The result will be exported into a PNG file with the result log.
- You can customize some settings by editing "pref.ini".

## Compatibility

Tested platforms:

- Windows 10 1903 x64, Windows Server 2008 R2 x64, Windows 7 SP1 x64
- Ubuntu 18.10
- Debian 6.3
- CentOS 7.6
- MacOS 10.13.6 High Sierra, 10.14.6 Mojave, 10.15 Catalina
- Android 8.0, 9.0 (with Termux)
- iOS/iPadOS 13 (with iSH Shell) **Bad performance, only for testing purpose**
- Raspberry Pi 4B with Raspbian (armv7l)

Supported proxy types:

| Proxy  |       Client       |                                                      Config Parser                                                       |
| :----: | :----------------: | :----------------------------------------------------------------------------------------------------------------------: |
|  SSR   | ShadowsocksR-libev |                                    ShadowsocksR\| Quantumult(X) \| SSTap \| Netch GSF                                    |
|   SS   | Shadowsocks-libev  | Shadowsocks\| ShadowsocksD \| Shadowsocks Android \| SSTap \| Clash \| Surge 2 \| Surge 3+ \| Quantumult(X) \| Netch GSF |
| VMESS  |       V2Ray        |                                 V2RayN\| Quantumult(X) \| Clash \| Surge 4 \| Netch GSF                                  |
| VLESS  |       V2Ray        |                                                                                                                          |
| TROJAN |    Trojan-Core     |                                 Trojan\| Quantumult(X) \| Surge 4 \| Clash \| Netch GSF                                  |
| SOCKS5 |         -          |                                    Telegram\| SSTap \| Clash \| Surge 2+ \| Netch GSF                                    |

## Known Bugs

- Nothing yet

## TODO

- 对于 ss/trojan/socks 增加并发支持
- 增加对 hy2 的支持，引用 singbox
- 处理 getinvalidnode, 剔除无效节点算法
- 增加对 netflex/gemini 探测分类

---

## windows 使用说明

一、最快上手
打开 cmd，切到 软件 目录```

- 运行程序，按提示在控制台粘贴你的订阅链接（或单节点链接），回车开始测速
  .\stairspeedtest.exe

- 多个订阅/链接可以用 | 分隔，一次性测试多个组
  测完后到 ./results 查看导出的图片结果，到 ./logs 查看日志
- 也可以：把订阅 URL 或者节点文件 直接作为参数传入（省去交互输入）
  注意要将 webserver_mode = false 否则将无法进行批量处理

  .\stairspeedtest.exe /u "https://example.com/subscription.txt"

  StairSpeedtest.exe /u "e:\_cpp_work\StairSpeedTest\res\sample.txt"

- 也可以指定自定义分组名（导出图片会用到）

  .\stairspeedtest.exe /u "https://example.com/subscription.txt" /g "我的分组"

二、内置 Web 界面（推荐，零依赖）

- 方式 A：双击运行脚本
- 直接双击 `webserver.bat`，它会打开浏览器 [http://127.0.0.1:10870](http://127.0.0.1:10870/) 并启动后端
- 方式 B：命令行启动后端
  .\stairspeedtest.exe /web

说明：

- 静态页面在 `webui`，打开后按页面提示粘贴订阅或上传配置，再点开始测速即可
- 监听地址和端口可在 `pref.ini` 的 [webserver] 段改 listen_address / listen_port（默认 127.0.0.1:10870）

三、输入支持的内容类型

- 直接在控制台/网页里粘贴即可（也可用 /u 传参）- 单节点链接：vmess://、vmess1://、ss://、ssr://、trojan://、socks://、https://t.me/socks 、tg://socks
- 订阅链接：http:// 或 https://（也支持 surge:///install-config）
- 本地文件路径：程序会读取并解析（配置文件或历史 log）
- 多个链接/订阅：用 | 分隔，例如 "link1|link2|link3"

四、结果输出位置

- 图片：results\YYYYMMDD-HHMMSS.png（或分组多图）
- 日志：logs\YYYYMMDD-HHMMSS.log
- 说明：单链接默认也会导出图片（由 `pref.ini` 中 export.single_test_force_export 控制，默认 true）

六、常用配置（在 `pref.ini`）
[advanced]
speedtest_mode=all|speedonly|pingonly
test_upload=true/false（是否测上传）
test_nat_type=true/false（是否测 NAT 类型）
thread_count=4（并发线程）
[export]
export_with_maxspeed=true/false（是否导出 MaxSpeed）
export_sort_method=none|speed|rspeed|ping|rping（排序）
export_color_style=original|rainbow|custom（颜色方案）
[rules]
test_file_urls=URL|标签（可配置多个下载测试文件）
rules=...（根据 ISP/域名等规则挑选测试 URL）

七、关于外部客户端与运行环境

- 如果你测的是 vmess/ss/ssr/trojan 等协议，程序会尝试启动对应客户端（从 `tools\clients` 查找）：- v2ray.exe、ss-local.exe、ssr-local.exe、trojan.exe
- 不存在时会提示未找到，相关协议将无法自动起客户端（socks 链接不受影响）
- 程序会使用本地 SOCKS 端口（默认 32768 ，自动避让占用）
- 如果在 CMD/PowerShell 下运行遇到“缺少 DLL”之类的启动错误：
- 方案 1：在 MSYS2 MINGW64 终端运行 exe
- 方案 2：把需要的 DLL（如 libnghttp2、libidn2、libunistring、zstd、zlib、brotli 等）放到 根 目录，或把 MINGW64\bin 加到 PATH

八、一些示例命令

交互运行（运行后粘贴订阅）
.\stairspeedtest.exe
指定订阅 URL 并设置分组
.\stairspeedtest.exe /u "https://example.com/sub.txt" /g "我的分组"
启动内置 WebServer
.\stairspeedtest.exe /web
一键 WebServer（脚本）
.\webserver.bat
旧版 WebGUI（脚本）
.\webgui.bat
