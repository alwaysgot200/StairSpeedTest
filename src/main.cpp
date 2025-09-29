#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <atomic>
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#endif // _WIN32

#include "ini_reader.h"
#include "logger.h"
#include "misc.h"
#include "multithread_test.h"
#include "nodeinfo.h"
#include "printmsg.h"
#include "printout.h"
#include "processes.h"
#include "rapidjson_extra.h" // 新增：用于解析/生成聚合 v2ray 配置
#include "renderer.h"
#include "rulematch.h"
#include "socket.h"
#include "speedtestutil.h"
#include "version.h"
#include "webget.h"

using namespace std::chrono;

#define MAX_FILE_SIZE 100 * 1024 * 1024

// use for command argument
bool rpcmode = false;
std::string sub_url;
bool pause_on_done = true;

// for use globally
bool multilink = false;
int socksport = 32768;
std::string socksaddr = "127.0.0.1";
std::string custom_group;
std::string pngpath;

// for use of web server
bool webserver_mode = false;
std::string listen_address = "127.0.0.1";
int listen_port = 10870, cur_node_id = -1;
int http_timeout_seconds = 30; // 新增：默认 30 秒

bool ss_libev = true;
bool ssr_libev = true;
std::string def_test_file =
    "https://download.microsoft.com/download/2/0/E/"
    "20E90413-712F-438C-988E-FDAA79A8AC3D/dotnetfx35.exe";
std::string def_upload_target =
    "http://losangeles.speed.googlefiber.net:3004/upload?time=0";
int http_get_max_size_mb = 5;
long http_get_max_size_bytes = 5L * 1024 * 1024;
std::string site_ping_url = "https://www.google.com/";
std::vector<downloadLink> downloadFiles;
std::vector<linkMatchRule> matchRules;
string_array custom_exclude_remarks, custom_include_remarks, dict, trans;
std::vector<nodeInfo> allNodes;
std::vector<color> custom_color_groups;
std::vector<int> custom_color_bounds;
std::string speedtest_mode = "all";
std::string override_conf_port = "";
std::string export_color_style = "rainbow";
int def_thread_count = 4;
bool export_with_maxspeed = false;
bool export_picture = false;
bool export_as_new_style = true;
bool test_site_ping = true;
bool test_upload = false;
bool test_nat_type = true;
bool multilink_export_as_one_image = false;
bool single_test_force_export = false;
bool verbose = false;
std::string export_sort_method = "none";

// 新增：v2ray 分片与并发参数
int v2ray_shard_size = 1024;       // 每片最多节点数
int v2ray_group_concurrency = 256; // 并发度（可改为 16）

int avail_status[5] = {0, 0, 0, 0, 0};
unsigned int node_count = 0;
int curGroupID = 0;

int global_log_level = LOG_LEVEL_ERROR;
bool serve_cache_on_fetch_fail = false, print_debug_info = false;
int parse_worker_count = 0;
int parse_parallel_threshold = 256;

// declarations

int explodeLog(const std::string &log, std::vector<nodeInfo> &nodes);
int tcping(nodeInfo &node);
void getTestFile(nodeInfo &node, const std::string &proxy,
                 const std::vector<downloadLink> &downloadFiles,
                 const std::vector<linkMatchRule> &matchRules,
                 const std::string &defaultTestFile);
void stairspeed_webserver_routine(const std::string &listen_address,
                                  int listen_port);
// 显式 push：来自 webgui_wrapper.cpp 的线程安全通知接口
void webui_notify_node_tested(const nodeInfo &node);
std::string
get_nat_type_thru_socks5(const std::string &server, uint16_t port,
                         const std::string &username = "",
                         const std::string &password = "",
                         const std::string &stun_server = "stun.l.google.com",
                         uint16_t stun_port = 19302);

// forward declarations for aggregated v2ray concurrent testing helpers
static bool extract_first_outbound(const std::string &single_config_json,
                                   rapidjson::Value &out,
                                   rapidjson::Document::AllocatorType &alloc);
static std::string buildAggregatedV2RayConfig(
    const std::vector<std::pair<nodeInfo *, int>> &items);
static std::vector<int> allocateShardPorts(size_t count, int start_hint_port);
// 处理GeoIP检测结果
static void processGeoIPResult(nodeInfo &node, int rpcmode, std::string id) {
  geoIPInfo outbound = node.outboundGeoIP.get();
  if (outbound.organization.size()) {
    writeLog(LOG_TYPE_INFO, "Got outbound ISP: " + outbound.organization +
                                "  Country code: " + outbound.country_code);
    printMsg(SPEEDTEST_MESSAGE_GOTGEOIP, rpcmode, id, outbound.organization,
             outbound.country_code);
  } else {
    printMsg(SPEEDTEST_ERROR_GEOIPERR, rpcmode, id);
  }
}

// 确保异步任务完成（Web模式）
static void ensureAsyncTasksReady(nodeInfo &node) {
  (void)node.inboundGeoIP.get();
  (void)node.outboundGeoIP.get();
  if (test_nat_type) {
    (void)node.natType.get();
  }
}

static int testNodeViaPreparedSocks(nodeInfo &node,
                                    const std::string &testserver, int testport,
                                    const std::string &username,
                                    const std::string &password);
// 新增：分片准备（分配端口 + 生成并写入聚合 config.json）
static bool prepareShardConfigAndPorts(const std::vector<nodeInfo *> &shard,
                                       std::vector<int> &ports);
// 新增：标准流程的并发测试（sleep->就绪->并发测试->web通知）
static void testShardWithConcurrency(std::vector<nodeInfo *> &shard,
                                     const std::vector<int> &ports,
                                     int concurrency);
static void testV2RayShards(std::vector<nodeInfo *> &group, int shard_size,
                            int concurrency);
// 新增：在分片启动失败时，剔除无效节点并返回有效子集
static std::vector<nodeInfo *> getValidNodes(std::vector<nodeInfo *> &shard);

static inline int to_int(const char *s) {
  if (!s)
    return 0;
  try {
    return std::stoi(std::string(s));
  } catch (...) {
    return 0;
  }
}

#ifdef _WIN32
static std::atomic<int> g_current_node_id{-1};
static std::atomic<const char *> g_current_stage{"init"};
static std::mutex g_breadcrumb_mutex;
static std::string g_current_node_key;

void set_breadcrumb(int node_id, const char *stage) {
  g_current_node_id.store(node_id, std::memory_order_relaxed);
  g_current_stage.store(stage, std::memory_order_relaxed);
  // 构造节点键：id|linkType|server|port，如索引无效则使用占位
  std::string key;
  if (node_id >= 0 && node_id < static_cast<int>(allNodes.size())) {
    const nodeInfo &n = allNodes[node_id];
    key = std::to_string(n.id) + "|" + std::to_string(n.linkType) + "|" +
          n.server + "|" + std::to_string(n.port);
  } else {
    key = std::to_string(node_id) + "|?|?|?";
  }
  {
    std::lock_guard<std::mutex> lk(g_breadcrumb_mutex);
    g_current_node_key = key;
  }
  writeLog(LOG_TYPE_INFO,
           "Breadcrumb: node=" + key + " stage=" + std::string(stage));
}

static LONG WINAPI CrashUnhandledFilter(EXCEPTION_POINTERS *ep) {
  std::string msg = "Unhandled exception. Last breadcrumb: node_id=" +
                    std::to_string(g_current_node_id.load()) +
                    " stage=" + std::string(g_current_stage.load());
  writeLog(LOG_TYPE_ERROR, msg);
  return EXCEPTION_EXECUTE_HANDLER;
}

static void installCrashFilter() {
  SetUnhandledExceptionFilter(CrashUnhandledFilter);
}

// 新增：零查找重载版本，直接用传入的 node 构造节点键
void set_breadcrumb(const nodeInfo &node, const char *stage) {
  g_current_node_id.store(node.id, std::memory_order_relaxed);
  g_current_stage.store(stage, std::memory_order_relaxed);

  std::string key = std::to_string(node.id) + "|" +
                    std::to_string(node.linkType) + "|" + node.server + "|" +
                    std::to_string(node.port);
  {
    std::lock_guard<std::mutex> lk(g_breadcrumb_mutex);
    g_current_node_key = key;
  }
  writeLog(LOG_TYPE_INFO,
           "Breadcrumb: node=" + key + " stage=" + std::string(stage));
}
#else
static inline void set_breadcrumb(int, const char *) {}
static inline void installCrashFilter() {}
// 非 Windows：为重载提供空实现，保持链接一致
static inline void set_breadcrumb(const nodeInfo &, const char *) {}
#endif

// original codes

#ifndef _WIN32

int _getch() {
  int ch;
  struct termios tm, tm_old;
  int fd = 0;

  if (tcgetattr(fd, &tm) < 0) {
    return -1;
  }

  tm_old = tm;
  cfmakeraw(&tm);
  if (tcsetattr(fd, TCSANOW, &tm) < 0) {
    return -1;
  }

  ch = std::cin.get();
  if (tcsetattr(fd, TCSANOW, &tm_old) < 0) {
    return -1;
  }
  return ch;
}

void SetConsoleTitle(std::string title) {
  system(std::string("echo \"\\033]0;" + title + "\\007\\c\"").data());
}

#endif // _WIN32

void clearTrans() {
  eraseElements(dict);
  eraseElements(trans);
}

void addTrans(std::string dictval, std::string transval) {
  dict.push_back(dictval);
  trans.push_back(transval);
}

void copyNodes(std::vector<nodeInfo> &source, std::vector<nodeInfo> &dest) {
  for (auto &x : source) {
    dest.push_back(x);
  }
}

void copyNodesWithGroupID(std::vector<nodeInfo> &source,
                          std::vector<nodeInfo> &dest, int groupID) {
  for (auto &x : source) {
    if (x.groupID == groupID)
      dest.push_back(x);
  }
}

void clientCheck() {
#ifdef _WIN32
  std::string v2core_path = "tools\\clients\\v2ray.exe";
  std::string ssr_libev_path = "tools\\clients\\ssr-local.exe";
  std::string ss_libev_path = "tools\\clients\\ss-local.exe";
  std::string trojan_path = "tools\\clients\\trojan.exe";
#else
  std::string v2core_path = "tools/clients/v2ray";
  std::string ssr_libev_path = "tools/clients/ssr-local";
  std::string ss_libev_path = "tools/clients/ss-local";
  std::string trojan_path = "tools/clients/trojan";
#endif // _WIN32

  if (fileExist(v2core_path)) {
    avail_status[SPEEDTEST_MESSAGE_FOUNDVMESS] = 1;
    writeLog(LOG_TYPE_INFO, "Found V2Ray core at path " + v2core_path);
  } else {
    avail_status[SPEEDTEST_MESSAGE_FOUNDVMESS] = 0;
    writeLog(LOG_TYPE_WARN, "V2Ray core not found at path " + v2core_path);
  }
  if (fileExist(v2core_path)) {
    avail_status[SPEEDTEST_MESSAGE_FOUNDVLESS] = 1;
    writeLog(LOG_TYPE_INFO, "Found V2Ray core at path " + v2core_path);
  } else {
    avail_status[SPEEDTEST_MESSAGE_FOUNDVLESS] = 0;
    writeLog(LOG_TYPE_WARN, "V2Ray core not found at path " + v2core_path);
  }
  if (fileExist(ss_libev_path)) {
    avail_status[SPEEDTEST_MESSAGE_FOUNDSS] = 1;
    writeLog(LOG_TYPE_INFO, "Found Shadowsocks-libev at path " + ss_libev_path);
  } else {
    avail_status[SPEEDTEST_MESSAGE_FOUNDSS] = 0;
    writeLog(LOG_TYPE_WARN,
             "Shadowsocks-libev not found at path " + ss_libev_path);
  }
  if (fileExist(ssr_libev_path)) {
    avail_status[SPEEDTEST_MESSAGE_FOUNDSSR] = 1;
    writeLog(LOG_TYPE_INFO,
             "Found ShadowsocksR-libev at path " + ssr_libev_path);
  } else {
    avail_status[SPEEDTEST_MESSAGE_FOUNDSSR] = 0;
    writeLog(LOG_TYPE_WARN,
             "ShadowsocksR-libev not found at path " + ssr_libev_path);
  }
  if (fileExist(trojan_path)) {
    avail_status[SPEEDTEST_MESSAGE_FOUNDTROJAN] = 1;
    writeLog(LOG_TYPE_INFO, "Found Trojan at path " + trojan_path);
  } else {
    avail_status[SPEEDTEST_MESSAGE_FOUNDTROJAN] = 0;
    writeLog(LOG_TYPE_WARN, "Trojan not found at path " + trojan_path);
  }
}

bool runClient(int client) {
#ifdef _WIN32

  // std::string v2core_path = "tools\\clients\\v2ray.exe run -c config.json";
  std::string v2core_path = "tools\\clients\\v2ray.exe -config config.json";
  std::string ssr_libev_path =
      "tools\\clients\\ssr-local.exe -u -c config.json";

  std::string ss_libev_dir = "tools\\clients\\";
  std::string ss_libev_path =
      ss_libev_dir + "ss-local.exe -u -c ..\\..\\config.json";

  std::string ssr_win_dir = "tools\\clients\\";
  std::string ssr_win_path = ssr_win_dir + "shadowsocksr-win.exe";
  std::string ss_win_dir = "tools\\clients\\";
  std::string ss_win_path = ss_win_dir + "shadowsocks-win.exe";

  std::string trojan_path = "tools\\clients\\trojan.exe -c config.json";

  switch (client) {
  case SPEEDTEST_MESSAGE_FOUNDVMESS:
    writeLog(LOG_TYPE_INFO, "Starting up v2ray core...");
    // 先对磁盘配置做预检，失败则不启动
    if (!testV2RayConfigFile("config.json", "")) {
      writeLog(LOG_TYPE_ERROR,
               "v2ray config.json validation failed (-test). Abort start.");
      return false;
    }
    return runProgram(v2core_path, "", false);
  case SPEEDTEST_MESSAGE_FOUNDVLESS:
    writeLog(LOG_TYPE_INFO, "Starting up v2ray core...");
    // 同样进行预检
    if (!testV2RayConfigFile("config.json", "")) {
      writeLog(LOG_TYPE_ERROR,
               "v2ray config.json validation failed (-test). Abort start.");
      return false;
    }
    return runProgram(v2core_path, "", false);
  case SPEEDTEST_MESSAGE_FOUNDSSR:
    if (ssr_libev) {
      writeLog(LOG_TYPE_INFO, "Starting up shadowsocksr-libev...");
      return runProgram(ssr_libev_path, "", false);
    } else {
      writeLog(LOG_TYPE_INFO, "Starting up shadowsocksr-win...");
      fileCopy("config.json", ssr_win_dir + "gui-config.json");
      return runProgram(ssr_win_path, "", false);
    }
  case SPEEDTEST_MESSAGE_FOUNDSS:
    if (ss_libev) {
      writeLog(LOG_TYPE_INFO, "Starting up shadowsocks-libev...");
      return runProgram(ss_libev_path, ss_libev_dir, false);
    } else {
      writeLog(LOG_TYPE_INFO, "Starting up shadowsocks-win...");
      fileCopy("config.json", ss_win_dir + "gui-config.json");
      return runProgram(ss_win_path, ss_win_dir, false);
    }
  case SPEEDTEST_MESSAGE_FOUNDTROJAN:
    writeLog(LOG_TYPE_INFO, "Starting up trojan...");
    return runProgram(trojan_path, "", false);
  default:
    return false;
  }
#else

  // std::string v2core_path = "tools/clients/v2ray  -config config.json";
  std::string v2core_path = "tools/clients/v2ray  -config config.json";
  std::string ssr_libev_path = "tools/clients/ssr-local -u -c config.json";
  std::string trojan_path = "tools/clients/trojan -c config.json";

  std::string ss_libev_dir = "tools/clients/";
  std::string ss_libev_path = "./ss-local -u -c ../../config.json";

  switch (client) {
  case SPEEDTEST_MESSAGE_FOUNDVMESS:
    writeLog(LOG_TYPE_INFO, "Starting up v2ray core...");
    // 先对磁盘配置做预检，失败则不启动
    if (!testV2RayConfigFile("config.json", "")) {
      writeLog(LOG_TYPE_ERROR,
               "v2ray config.json validation failed (-test). Abort start.");
      return false;
    }
    return runProgram(v2core_path, "", false);
  case SPEEDTEST_MESSAGE_FOUNDVLESS:
    writeLog(LOG_TYPE_INFO, "Starting up v2ray core...");
    // 同样进行预检
    if (!testV2RayConfigFile("config.json", "")) {
      writeLog(LOG_TYPE_ERROR,
               "v2ray config.json validation failed (-test). Abort start.");
      return false;
    }
    return runProgram(v2core_path, "", false);
  case SPEEDTEST_MESSAGE_FOUNDSSR:
    writeLog(LOG_TYPE_INFO, "Starting up shadowsocksr-libev...");
    return runProgram(ssr_libev_path, "", false);
  case SPEEDTEST_MESSAGE_FOUNDSS:
    writeLog(LOG_TYPE_INFO, "Starting up shadowsocks-libev...");
    return runProgram(ss_libev_path, ss_libev_dir, false);
  case SPEEDTEST_MESSAGE_FOUNDTROJAN:
    writeLog(LOG_TYPE_INFO, "Starting up trojan...");
    return runProgram(trojan_path, "", false);
  default:
    return false;
  }
#endif // _WIN32
}

static bool waitUntilSocksReady(const std::string &addr, int port,
                                const std::string &username,
                                const std::string &password,
                                int timeout_ms = 1000) {
  using namespace std::chrono;
  auto deadline = steady_clock::now() + milliseconds(timeout_ms);

  while (steady_clock::now() < deadline) {
    SOCKET s = initSocket(getNetworkType(addr), SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
      if (startConnect(s, addr, port) != SOCKET_ERROR) {
        setTimeout(s, 700);
        // Try a full SOCKS5 method negotiation; success indicates inbound is
        // ready
        if (connectSocks5(s, username, password) == 0) {
          closesocket(s);
          return true;
        }
      }
      closesocket(s);
    }
    // wait a bit before retry to avoid busy loop
    sleep(200);
  }
  return false;
}

int killClient(int client) {
#ifdef _WIN32
  std::string v2core_name = "v2ray.exe";
  std::string ss_libev_name = "ss-local.exe";
  std::string ssr_libev_name = "ssr-local.exe";
  std::string ss_win_name = "shadowsocks-win.exe";
  std::string ssr_win_name = "shadowsocksr-win.exe";
  std::string trojan_name = "trojan.exe";

  switch (client) {
  case SPEEDTEST_MESSAGE_FOUNDVMESS:
    writeLog(LOG_TYPE_INFO, "Killing v2ray core...");
    killProgram(v2core_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDVLESS:
    writeLog(LOG_TYPE_INFO, "Killing v2ray core...");
    killProgram(v2core_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSSR:
    if (ssr_libev) {
      writeLog(LOG_TYPE_INFO, "Killing shadowsocksr-libev...");
      killProgram(ssr_libev_name);
    } else {
      writeLog(LOG_TYPE_INFO, "Killing shadowsocksr-win...");
      killProgram(ssr_win_name);
    }
    break;
  case SPEEDTEST_MESSAGE_FOUNDSS:
    if (ss_libev) {
      writeLog(LOG_TYPE_INFO, "Killing shadowsocks-libev...");
      killProgram(ss_libev_name);
    } else {
      writeLog(LOG_TYPE_INFO, "Killing shadowsocks-win...");
      killProgram(ss_win_name);
    }
    break;
  case SPEEDTEST_MESSAGE_FOUNDTROJAN:
    writeLog(LOG_TYPE_INFO, "Killing trojan...");
    killProgram(trojan_name);
    break;
  }
#else
  std::string v2core_name = "v2ray";
  std::string ss_libev_name = "ss-local";
  std::string ssr_libev_name = "ssr-local";
  std::string trojan_name = "trojan";

  switch (client) {
  case SPEEDTEST_MESSAGE_FOUNDVMESS:
    writeLog(LOG_TYPE_INFO, "Killing v2ray core...");
    killProgram(v2core_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDVLESS:
    writeLog(LOG_TYPE_INFO, "Killing v2ray core...");
    killProgram(v2core_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSSR:
    writeLog(LOG_TYPE_INFO, "Killing shadowsocksr-libev...");
    killProgram(ssr_libev_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSS:
    writeLog(LOG_TYPE_INFO, "Killing shadowsocks-libev...");
    killProgram(ss_libev_name);
    break;
  case SPEEDTEST_MESSAGE_FOUNDTROJAN:
    writeLog(LOG_TYPE_INFO, "Killing trojan...");
    killProgram(trojan_name);
    break;
  }
#endif
  return 0;
}

int terminateClient(int client) {
  killByHandle();
#ifdef __APPLE__
  killClient(client);
#endif // __APPLE__
  return 0;
}

void readConf(std::string path) {
  downloadLink link;
  linkMatchRule rule;
  color tmpColor;
  unsigned int i;
  string_array vChild, vArray;
  INIReader ini;
  std::string strTemp;

  // ini.do_utf8_to_gbk = true;
  ini.ParseFile(path);

  ini.EnterSection("common");
  if (ini.ItemPrefixExist("exclude_remark"))
    ini.GetAll("exclude_remark", custom_exclude_remarks);
  if (ini.ItemPrefixExist("include_remark"))
    ini.GetAll("include_remark", custom_include_remarks);

  ini.EnterSection("advanced");
  ini.GetIfExist("speedtest_mode", speedtest_mode);
  ini.GetBoolIfExist("test_site_ping", test_site_ping);
  ini.GetBoolIfExist("test_upload", test_upload);
  ini.GetBoolIfExist("test_nat_type", test_nat_type);
#ifdef _WIN32
  if (ini.ItemExist("preferred_ss_client")) {
    strTemp = ini.Get("preferred_ss_client");
    if (strTemp == "ss-csharp")
      ss_libev = false;
  }
  if (ini.ItemExist("preferred_ssr_client")) {
    strTemp = ini.Get("preferred_ssr_client");
    if (strTemp == "ssr-csharp")
      ssr_libev = false;
  }
#endif // _WIN32
  ini.GetIfExist("override_conf_port", override_conf_port);
  ini.GetIntIfExist("thread_count", def_thread_count);
  ini.GetBoolIfExist("pause_on_done", pause_on_done);

  // read GET limit (MB) and convert to bytes; and custom site ping URL
  ini.GetIntIfExist("http_get_max_size_mb", http_get_max_size_mb);
  if (http_get_max_size_mb <= 0)
    http_get_max_size_mb = 5;
  http_get_max_size_bytes = 1LL * http_get_max_size_mb * 1024 * 1024;
  ini.GetIfExist("site_ping_url", site_ping_url);
  if (site_ping_url.empty())
    site_ping_url = "https://www.google.com/";

  ini.EnterSection("parse");
  ini.GetIntIfExist("parallel_threshold", parse_parallel_threshold);
  ini.GetIntIfExist("worker_count", parse_worker_count);
  // 允许从 INI 覆盖 v2ray 并发与分片参数
  ini.GetIntIfExist("v2ray_group_concurrency", v2ray_group_concurrency);
  ini.GetIntIfExist("v2ray_shard_size", v2ray_shard_size);

  ini.EnterSection("export");
  ini.GetBoolIfExist("export_with_maxspeed", export_with_maxspeed);
  ini.GetBoolIfExist("export_picture", export_picture);
  ini.GetIfExist("export_sort_method", export_sort_method);
  ini.GetBoolIfExist("multilink_export_as_one_image",
                     multilink_export_as_one_image);
  ini.GetBoolIfExist("single_test_force_export", single_test_force_export);
  ini.GetBoolIfExist("export_as_new_style", export_as_new_style);
  ini.GetIfExist("export_color_style", export_color_style);
  if (ini.ItemExist("custom_color_groups")) {
    vChild = split(ini.Get("custom_color_groups"), "|");
    if (vChild.size() >= 2) {
      for (i = 0; i < vChild.size(); i++) {
        vArray = split(vChild[i], ",");
        if (vArray.size() == 3) {
          tmpColor.red = stoi(trim(vArray[0]));
          tmpColor.green = stoi(trim(vArray[1]));
          tmpColor.blue = stoi(trim(vArray[2]));
          custom_color_groups.push_back(tmpColor);
        }
      }
    }
  }
  if (ini.ItemExist("custom_color_bounds")) {
    vChild = split(ini.Get("custom_color_bounds"), "|");
    if (vChild.size() >= 2) {
      for (i = 0; i < vChild.size(); i++) {
        custom_color_bounds.push_back(stoi(vChild[i]));
      }
    }
  }
  ini.GetBoolIfExist("export_as_stairspeed", export_as_stairspeed);

  ini.EnterSection("rules");
  if (ini.ItemPrefixExist("test_file_urls")) {
    eraseElements(vArray);
    ini.GetAll("test_file_urls", vArray);
    for (auto &x : vArray) {
      vChild = split(x, "|");
      if (vChild.size() == 2) {
        link.url = vChild[0];
        link.tag = vChild[1];
        downloadFiles.push_back(link);
      }
    }
  }
  if (ini.ItemPrefixExist("rules")) {
    eraseElements(vArray);
    ini.GetAll("rules", vArray);
    for (auto &x : vArray) {
      vChild = split(x, "|");
      if (vChild.size() >= 3) {
        eraseElements(rule.rules);
        rule.mode = vChild[0];
        for (i = 1; i < vChild.size() - 1; i++) {
          rule.rules.push_back(vChild[i]);
        }
        rule.tag = vChild[vChild.size() - 1];
        matchRules.push_back(rule);
      }
    }
  }
  if (export_color_style == "custom") {
    colorgroup.swap(custom_color_groups);
    bounds.swap(custom_color_bounds);
  }

  ini.EnterSection("webserver");
  ini.GetBoolIfExist("webserver_mode", webserver_mode);
  ini.GetIfExist("listen_address", listen_address);
  ini.GetIntIfExist("listen_port", listen_port);
  ini.GetIntIfExist("http_timeout_seconds",
                    http_timeout_seconds); // 新增：从配置读取
#ifdef _WIN32
  // 安装未捕获异常拦截器，保证后续任何阶段崩溃能写面包屑到日志
  installCrashFilter();
#endif
}

void signalHandler(int signum) {
  std::cerr << "Interrupt signal (" << signum << ") received.\n";

#ifdef __APPLE__
  killClient(SPEEDTEST_MESSAGE_FOUNDSS);
  killClient(SPEEDTEST_MESSAGE_FOUNDSSR);
  killClient(SPEEDTEST_MESSAGE_FOUNDVMESS);
  killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
  killClient(SPEEDTEST_MESSAGE_FOUNDTROJAN);
#endif // __APPLE__
  killByHandle();
  writeLog(LOG_TYPE_INFO, "Received signal. Exit right now.");
  logEOF();

  exit(signum);
}

void chkArg(int argc, char *argv[]) {
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "/rpc"))
      rpcmode = true;
    else if (!strcmp(argv[i], "/web"))
      webserver_mode = true;
    else if (!strcmp(argv[i], "/u") && argc > i + 1)
      sub_url.assign(argv[++i]);
    else if (!strcmp(argv[i], "/g") && argc > i + 1)
      custom_group.assign(argv[++i]);
    else if ((!strcmp(argv[i], "/parse-threads") ||
              !strcmp(argv[i], "--parse-threads")) &&
             argc > i + 1)
      parse_worker_count = to_int(argv[++i]);
    else if ((!strcmp(argv[i], "/parse-threshold") ||
              !strcmp(argv[i], "--parse-threshold")) &&
             argc > i + 1)
      parse_parallel_threshold = to_int(argv[++i]);
  }
}

/*
void exportHTML()
{
    std::string htmpath = replace_all_distinct(resultPath, ".log", ".htm");
    //std::string pngname =
replace_all_distinct(replace_all_distinct(resultpath, ".log", ".png"),
"results\\", "");
    //std::string resultname = replace_all_distinct(resultpath, "results\\",
"");
    //std::string htmname = replace_all_distinct(htmpath, "results\\", "");
    //std::string rendercmd = "..\\tools\\misc\\phantomjs.exe
..\\tools\\misc\\render_alt.js " + htmname + " " + pngname + " " +
export_sort_method; exportResult(htmpath, "tools\\misc\\util.js",
"tools\\misc\\style.css", export_with_maxspeed);
    //runprogram(rendercmd, "results", true);
}
*/

void saveResult(std::vector<nodeInfo> &nodes) {
  INIReader ini;
  std::string data;
  std::string url_lines;          // collect original URLs of valid nodes
  std::string original_url_lines; // collect original share links of valid nodes

  ini.SetCurrentSection("Basic");
  ini.Set("Tester", "StairSpeedtest " VERSION);
  ini.Set("GenerationTime", getTime(3));

  // Only write valid nodes according to rules
  for (nodeInfo &x : nodes) {
    bool valid = false;

    // 新逻辑：只要站点延迟或下载测速任一有结果即视为有效
    bool has_site_ping = false;
    if (test_site_ping) {
      for (int v : x.rawSitePing) {
        if (v > 0) {
          has_site_ping = true;
          break;
        }
      }
    }
    bool has_download =
        (speedtest_mode != "pingonly") && (x.totalRecvBytes > 0);
    valid = has_site_ping || has_download;

    if (!valid)
      continue;

    // 收集原始分享链接（仅在字段非空时）
    if (!x.originalUrl.empty()) {
      original_url_lines += x.originalUrl;
      original_url_lines += "\n";
    }

    ini.SetCurrentSection(x.group + "^" + x.remarks);
    ini.Set("AvgPing", x.avgPing);
    ini.Set("PkLoss", x.pkLoss);
    ini.Set("SitePing", x.sitePing);
    ini.Set("AvgSpeed", x.avgSpeed);
    ini.Set("MaxSpeed", x.maxSpeed);
    ini.Set("ULSpeed", x.ulSpeed);
    ini.SetNumber<unsigned long long>("UsedTraffic", x.totalRecvBytes);
    ini.SetNumber<int>("GroupID", x.groupID);
    ini.SetNumber<int>("ID", x.id);
    ini.SetBool("Online", x.online);
    ini.SetArray("RawPing", ",", x.rawPing);
    ini.SetArray("RawSitePing", ",", x.rawSitePing);
    ini.SetArray("RawSpeed", ",", x.rawSpeed);

    // 新增：序列化出站国家代码
    if (!x.outboundCountryCode.empty()) {
      ini.Set("OutboundCountryCode", x.outboundCountryCode);
    }
  }

  // 新增：导出原始分享链接到 .originalUrl.txt
  if (!original_url_lines.empty()) {
    std::string originalPath = resultPath;
    size_t slash_pos2 = originalPath.find_last_of("\\/");
    size_t dot_pos2 = originalPath.find_last_of('.');
    if (dot_pos2 != std::string::npos &&
        (slash_pos2 == std::string::npos || dot_pos2 > slash_pos2)) {
      originalPath.replace(dot_pos2, std::string::npos, ".originalUrl.txt");
    } else {
      originalPath += ".originalUrl.txt";
    }
    fileWrite(originalPath, original_url_lines, true);
  }

  ini.ToFile(resultPath);
}
void saveResult(const nodeInfo &x) {
  // 函数内静态互斥锁，用于保护磁盘写入
  static std::mutex g_save_result_mutex;
  std::lock_guard<std::mutex> lk(g_save_result_mutex);

  // 只要站点延迟或下载测速任一有结果即视为有效
  bool has_site_ping = false;
  if (test_site_ping) {
    for (int v : x.rawSitePing) {
      if (v > 0) {
        has_site_ping = true;
        break;
      }
    }
  }
  bool has_download = (speedtest_mode != "pingonly") && (x.totalRecvBytes > 0);
  bool valid = has_site_ping || has_download;

  if (valid) {
    std::string content;

    // 如果结果文件尚不存在，先写入 Basic 段（一次性）
    if (!fileExist(resultPath, false)) {
      INIReader hdr;
      hdr.SetCurrentSection("Basic");
      hdr.Set("Tester", "StairSpeedtest " VERSION);
      hdr.Set("GenerationTime", getTime(3));
      content += hdr.ToString();
    }
    // 构造本节点的 section 文本并追加
    INIReader sec;
    // 将 linkType 映射为协议字符串（小写）
    auto protoToStr = [](int t) -> std::string {
      switch (t) {
      case SPEEDTEST_MESSAGE_FOUNDVMESS:
        return "vmess";
      case SPEEDTEST_MESSAGE_FOUNDVLESS:
        return "vless";
      case SPEEDTEST_MESSAGE_FOUNDSS:
        return "ss";
      case SPEEDTEST_MESSAGE_FOUNDSSR:
        return "ssr";
      case SPEEDTEST_MESSAGE_FOUNDTROJAN:
        return "trojan";
      case SPEEDTEST_MESSAGE_FOUNDSOCKS:
        return "socks";
      case SPEEDTEST_MESSAGE_FOUNDSUB:
        return "sub";
      case SPEEDTEST_MESSAGE_FOUNDNETCH:
        return "netch";
      case SPEEDTEST_MESSAGE_FOUNDUPD:
        return "upd";
      case SPEEDTEST_MESSAGE_FOUNDLOCAL:
        return "local";
      default:
        return "unknown";
      }
    };
    std::string section =
        protoToStr(x.linkType) + "|" + x.server + "|" + std::to_string(x.port);
    sec.SetCurrentSection(section);
    sec.Set("Remarks", x.remarks);
    sec.Set("AvgPing", x.avgPing);
    sec.Set("PkLoss", x.pkLoss);
    sec.Set("SitePing", x.sitePing);
    sec.Set("AvgSpeed", x.avgSpeed);
    sec.Set("MaxSpeed", x.maxSpeed);
    sec.Set("ULSpeed", x.ulSpeed);
    sec.SetNumber<unsigned long long>("UsedTraffic", x.totalRecvBytes);
    sec.SetNumber<int>("GroupID", x.groupID);
    sec.SetNumber<int>("ID", x.id);
    sec.SetBool("Online", x.online);
    sec.SetArray("RawPing", ",", x.rawPing);
    sec.SetArray("RawSitePing", ",", x.rawSitePing);
    sec.SetArray("RawSpeed", ",", x.rawSpeed);
    if (!x.outboundCountryCode.empty()) {
      sec.Set("OutboundCountryCode", x.outboundCountryCode);
    }
    content += sec.ToString();

    // 以“追加”方式写入到结果文件
    fileWrite(resultPath, content, false);

    // 追加原始分享链接
    if (!x.originalUrl.empty()) {
      std::string originalPath = resultPath;
      size_t slash_pos2 = originalPath.find_last_of("\\/");
      size_t dot_pos2 = originalPath.find_last_of('.');
      if (dot_pos2 != std::string::npos &&
          (slash_pos2 == std::string::npos || dot_pos2 > slash_pos2)) {
        originalPath.replace(dot_pos2, std::string::npos, ".originalUrl.txt");
      } else {
        originalPath += ".originalUrl.txt";
      }
      fileWrite(originalPath, x.originalUrl + "\n", false);
    }
  }
}
std::string removeEmoji(const std::string &orig_remark) {
  char emoji_id[2] = {(char)-16, (char)-97};
  std::string remark = orig_remark;
  while (true) {
    if (remark[0] == emoji_id[0] && remark[1] == emoji_id[1])
      remark.erase(0, 4);
    else
      break;
  }
  if (remark.empty())
    return orig_remark;
  return remark;
}
// test one node
int singleTest(nodeInfo &node) {
  node.remarks = trim(removeEmoji(node.remarks)); // remove all emojis
  int retVal = 0;
  std::string logdata, testserver, username, password, proxy;
  int testport;
  node.ulTarget = def_upload_target; // for now only use default
  cur_node_id = node.id;
  std::string id = std::to_string(node.id + (rpcmode ? 0 : 1));

  set_breadcrumb(node, "singleTest:begin");

  writeLog(LOG_TYPE_INFO,
           "Received server. Group: " + node.group + " Name: " + node.remarks);
  defer(printMsg(SPEEDTEST_MESSAGE_GOTRESULT, rpcmode, node.avgSpeed,
                 node.maxSpeed, node.ulSpeed, node.pkLoss, node.avgPing,
                 node.sitePing, node.natType.get());) auto start =
      steady_clock::now();
  if (node.proxyStr == "LOG") // import from result
  {
    if (!rpcmode)
      printMsg(SPEEDTEST_MESSAGE_GOTSERVER, rpcmode, id, node.group,
               node.remarks, std::to_string(node_count));
    printMsg(SPEEDTEST_MESSAGE_GOTPING, rpcmode, id, node.avgPing);
    printMsg(SPEEDTEST_MESSAGE_GOTGPING, rpcmode, id, node.sitePing);
    printMsg(SPEEDTEST_MESSAGE_GOTSPEED, rpcmode, id, node.avgSpeed);
    printMsg(SPEEDTEST_MESSAGE_GOTUPD, rpcmode, id, node.ulSpeed);
    writeLog(
        LOG_TYPE_INFO,
        "Average speed: " + node.avgSpeed + "  Max speed: " + node.maxSpeed +
            "  Upload speed: " + node.ulSpeed +
            "  Traffic used in bytes: " + std::to_string(node.totalRecvBytes));
    return SPEEDTEST_ERROR_NONE;
  }
  defer(auto end = steady_clock::now();
        auto lapse = duration_cast<seconds>(end - start);
        node.duration = lapse.count();)

      if (node.linkType == SPEEDTEST_MESSAGE_FOUNDSOCKS) {
    testserver = node.server;
    testport = node.port;
    username = getUrlArg(node.proxyStr, "user");
    password = getUrlArg(node.proxyStr, "pass");
  }
  else {
    testserver = socksaddr;
    testport = socksport;
    writeLog(LOG_TYPE_INFO, "Writing config file...");
    fileWrite("config.json", node.proxyStr,
              true); // make the right config file for client
    if (node.linkType != -1 && avail_status[node.linkType] == 1) {
      bool ok = runClient(node.linkType); // startup client
      if (!ok) {
        writeLog(LOG_TYPE_ERROR, "Client startup failed. Please check "
                                 "config.json and client binary.");
        // 提示一条通用错误信息，随后终止该节点的测试
        printMsg(SPEEDTEST_ERROR_UNDEFINED, rpcmode);
        return SPEEDTEST_ERROR_UNDEFINED;
      }
    }

    // Wait until local SOCKS5 inbound is ready (up to 1 seconds)
    if (!waitUntilSocksReady(testserver, testport, username, password, 1000)) {
      writeLog(LOG_TYPE_WARN,
               "SOCKS inbound seems not ready at " + testserver + ":" +
                   std::to_string(testport) +
                   " after waiting. Will continue and may fail.");
    }
  }
#ifdef __APPLE__
  defer(killClient(node.linkType);)
#endif // __APPLE__
      defer(killByHandle();) proxy =
          buildSocks5ProxyString(testserver, testport, username, password);

  // printMsg(SPEEDTEST_MESSAGE_GOTSERVER, node, rpcmode);
  if (!rpcmode)
    printMsg(SPEEDTEST_MESSAGE_GOTSERVER, rpcmode, id, node.group, node.remarks,
             std::to_string(node_count));
  sleep(200); /// wait for client startup

  // Here begin TCP ping
  printMsg(SPEEDTEST_MESSAGE_STARTPING, rpcmode, id);
  if (speedtest_mode != "speedonly") {
    writeLog(LOG_TYPE_INFO, "Now performing TCP ping...");
    retVal = tcping(node);
    if (retVal == SPEEDTEST_ERROR_NORESOLVE) {
      writeLog(LOG_TYPE_ERROR, "Node address resolve error.");
      printMsg(SPEEDTEST_ERROR_NORESOLVE, rpcmode, id);
      return SPEEDTEST_ERROR_NORESOLVE;
    }
    if (node.pkLoss == "100.00%") {
      writeLog(LOG_TYPE_ERROR, "Cannot connect to this node.");
      printMsg(SPEEDTEST_ERROR_NOCONNECTION, rpcmode, id);
      return SPEEDTEST_ERROR_NOCONNECTION;
    }
    logdata = std::accumulate(
        std::next(std::begin(node.rawPing)), std::end(node.rawPing),
        std::to_string(node.rawPing[0]), [](std::string a, int b) {
          return std::move(a) + " " + std::to_string(b);
        });
    writeLog(LOG_TYPE_RAW, logdata);
    writeLog(LOG_TYPE_INFO,
             "TCP Ping: " + node.avgPing + "  Packet Loss: " + node.pkLoss);
  } else
    node.pkLoss = "0.00%";
  printMsg(SPEEDTEST_MESSAGE_GOTPING, rpcmode, id, node.avgPing, node.pkLoss);

  // ping a real website with proxy engine
  if (test_site_ping) {
    printMsg(SPEEDTEST_MESSAGE_STARTGPING, rpcmode, id);
    set_breadcrumb(node, "singleTest:sitePing");
    writeLog(LOG_TYPE_INFO, "Now performing site ping...");
    // websitePing(node, "https://www.google.com/", testserver, testport,
    // username, password);
    sitePing(node, testserver, testport, username, password, site_ping_url);
    logdata = std::accumulate(
        std::next(std::begin(node.rawSitePing)), std::end(node.rawSitePing),
        std::to_string(node.rawSitePing[0]), [](std::string a, int b) {
          return std::move(a) + " " + std::to_string(b);
        });
    writeLog(LOG_TYPE_RAW, logdata);
    writeLog(LOG_TYPE_INFO, "Site ping: " + node.sitePing);
    printMsg(SPEEDTEST_MESSAGE_GOTGPING, rpcmode, id, node.sitePing);
    // 如果所有时延都失败，就返回
    bool anySuccess = false;
    for (int v : node.rawSitePing) {
      if (v > 0) {
        anySuccess = true;
        break;
      }
    }
    if (!anySuccess) {
      writeLog(LOG_TYPE_ERROR, "Site ping failed or timed out.");
      printMsg(SPEEDTEST_ERROR_NOCONNECTION, rpcmode, id);
      return SPEEDTEST_ERROR_NOCONNECTION;
    }
  }
  // test to download the data from remote site
  printMsg(SPEEDTEST_MESSAGE_STARTSPEED, rpcmode, id);
  // node.total_recv_bytes = 1;
  if (speedtest_mode != "pingonly") {
    set_breadcrumb(node, "singleTest:download");
    getTestFile(node, proxy, downloadFiles, matchRules, def_test_file);
    writeLog(LOG_TYPE_INFO, "Now performing file download speed test...");
    perform_test(node, testserver, testport, username, password,
                 def_thread_count);
    logdata = std::accumulate(
        std::next(std::begin(node.rawSpeed)), std::end(node.rawSpeed),
        std::to_string(node.rawSpeed[0]), [](std::string a, int b) {
          return std::move(a) + " " + std::to_string(b);
        });
    writeLog(LOG_TYPE_RAW, logdata);
    if (node.totalRecvBytes == 0) {
      writeLog(LOG_TYPE_ERROR, "Speedtest returned no speed.");
      printMsg(SPEEDTEST_ERROR_RETEST, rpcmode, id);
      set_breadcrumb(node, "singleTest:download_retest");
      perform_test(node, testserver, testport, username, password,
                   def_thread_count);
      logdata = std::accumulate(
          std::next(std::begin(node.rawSpeed)), std::end(node.rawSpeed),
          std::to_string(node.rawSpeed[0]), [](std::string a, int b) {
            return std::move(a) + " " + std::to_string(b);
          });
      writeLog(LOG_TYPE_RAW, logdata);
      if (node.totalRecvBytes == 0) {
        writeLog(LOG_TYPE_ERROR, "Speedtest returned no speed 2 times.");
        printMsg(SPEEDTEST_ERROR_NOSPEED, rpcmode, id);
        printMsg(SPEEDTEST_MESSAGE_GOTSPEED, rpcmode, id, node.avgSpeed,
                 node.maxSpeed);
        return SPEEDTEST_ERROR_NOSPEED;
      }
    }
  }
  // test upload speed by local proxy
  printMsg(SPEEDTEST_MESSAGE_GOTSPEED, rpcmode, id, node.avgSpeed,
           node.maxSpeed);
  if (test_upload) {
    set_breadcrumb(node, "singleTest:upload");
    writeLog(LOG_TYPE_INFO, "Now performing upload speed test...");
    printMsg(SPEEDTEST_MESSAGE_STARTUPD, rpcmode, id);
    upload_test(node, testserver, testport, username, password);
    printMsg(SPEEDTEST_MESSAGE_GOTUPD, rpcmode, id, node.ulSpeed);
  }

  // Begin to test  NAT type test
  if (test_nat_type) {
    printMsg(SPEEDTEST_MESSAGE_STARTNAT, rpcmode, id);
    node.natType.set(std::async(std::launch::async, [testserver, testport,
                                                     username, password]() {
      return get_nat_type_thru_socks5(testserver, testport, username, password);
    }));
  }
  // Here begin the GeoIP test
  writeLog(LOG_TYPE_INFO, "Now started fetching GeoIP info...");
  printMsg(SPEEDTEST_MESSAGE_STARTGEOIP, rpcmode, id);
  node.inboundGeoIP.set(std::async(
      std::launch::async, [node]() { return getGeoIPInfo(node.server, ""); }));
  node.outboundGeoIP.set(std::async(
      std::launch::async, [proxy]() { return getGeoIPInfo("", proxy); }));

  // 等待异步任务完成并处理结果
  if (!webserver_mode) {
    // 控制台模式：获取并显示详细结果
    processGeoIPResult(node, rpcmode, id);
    if (test_nat_type) {
      printMsg(SPEEDTEST_MESSAGE_GOTNAT, rpcmode, id, node.natType.get());
    }
  } else {
    // Web模式：仅确保数据就绪，防止/getresults阻塞
    ensureAsyncTasksReady(node);
  }

  // 总结测试结果
  writeLog(LOG_TYPE_INFO,
           "Average speed: " + node.avgSpeed + "  Max speed: " + node.maxSpeed +
               "  Upload speed: " + node.ulSpeed + "  Traffic used in bytes: " +
               std::to_string(node.totalRecvBytes));
  node.online = true;
  return SPEEDTEST_ERROR_NONE;
}
// test the nodes now
void batchTest(std::vector<nodeInfo> &nodes) {
  nodeInfo node;
  unsigned int onlines = 0;
  long long tottraffic = 0;
  cur_node_id = -1;

  writeLog(LOG_TYPE_INFO, "Total node(s) found: " + std::to_string(node_count));
  if (node_count == 0) {
    writeLog(LOG_TYPE_ERROR, "No nodes are found in this subscription.");
    printMsg(SPEEDTEST_ERROR_NONODES, rpcmode);
  } else {
    resultInit();
    writeLog(LOG_TYPE_INFO, "Speedtest will now begin.");
    printMsg(SPEEDTEST_MESSAGE_BEGIN, rpcmode);
    // first print out all nodes when in Web mode
    if (rpcmode) {
      for (nodeInfo &x : nodes)
        printMsg(SPEEDTEST_MESSAGE_GOTSERVER, rpcmode, std::to_string(x.id),
                 x.group, x.remarks);
    }

    // 协议分组：vmess+vless 为一组，其它为一组
    std::vector<nodeInfo *> v2ray_nodes, other_nodes;
    v2ray_nodes.reserve(nodes.size());
    other_nodes.reserve(nodes.size());
    for (auto &x : nodes) {
      if (custom_group.size() != 0)
        x.group = custom_group;
      if (x.linkType == SPEEDTEST_MESSAGE_FOUNDVMESS ||
          x.linkType == SPEEDTEST_MESSAGE_FOUNDVLESS) {
        v2ray_nodes.push_back(&x);
      } else {
        other_nodes.push_back(&x);
      }
    }

    // VMESS+VLESS：分片并发（单进程 v2ray）
    if (!v2ray_nodes.empty()) {
      // 新增：统一收敛 shard_size 与 concurrency（均支持“合并/继承”策略）
      const unsigned int hw = std::thread::hardware_concurrency();
      const int auto_workers = hw ? static_cast<int>(hw) * 2 : 8;
      const int auto_threshold =
          (parse_parallel_threshold > 0) ? parse_parallel_threshold : 512;

      // 未显式设置时，分片大小继承 parallel_threshold
      if (v2ray_shard_size <= 0)
        v2ray_shard_size = auto_threshold;
      if (v2ray_shard_size < 1)
        v2ray_shard_size = 1;
      // 未显式设置时，并发继承 parse.worker_count 或 2*HW
      if (v2ray_group_concurrency <= 0)
        v2ray_group_concurrency =
            (parse_worker_count > 0) ? parse_worker_count : auto_workers;
      if (v2ray_group_concurrency < 1)
        v2ray_group_concurrency = 1;
      if (v2ray_shard_size >= v2ray_nodes.size())
        v2ray_shard_size = v2ray_nodes.size();
      // 并发不超过分片大小
      if (v2ray_group_concurrency > v2ray_shard_size)
        v2ray_group_concurrency = v2ray_shard_size;

      writeLog(LOG_TYPE_INFO,
               "v2ray_shard_size=" + std::to_string(v2ray_shard_size) +
                   ", v2ray_group_concurrency=" +
                   std::to_string(v2ray_group_concurrency));

      testV2RayShards(v2ray_nodes, v2ray_shard_size, v2ray_group_concurrency);
    }

    // 其他协议（保持现有串行逻辑）
    for (auto *px : other_nodes) {
      singleTest(*px);
      tottraffic += px->totalRecvBytes;
      if (px->online)
        onlines++;
      // 显式 push：单个节点完成就推送
      webui_notify_node_tested(*px);
      // 新增：单节点结果立刻追加保存
      saveResult(*px);
    }

    // 汇总统计
    for (auto *px : v2ray_nodes) {
      tottraffic += px->totalRecvBytes;
      if (px->online)
        onlines++;
    }

    writeLog(LOG_TYPE_INFO,
             "All nodes tested. Total/Online nodes: " +
                 std::to_string(node_count) + "/" + std::to_string(onlines) +
                 " Traffic used: " + speedCalc(tottraffic * 1.0));
    // saveResult(nodes);
    if (export_picture && (webserver_mode || !multilink)) {
      printMsg(SPEEDTEST_MESSAGE_PICSAVING, rpcmode);
      writeLog(LOG_TYPE_INFO, "Now exporting result...");
      pngpath = exportRender(resultPath, nodes, export_with_maxspeed,
                             export_sort_method, export_color_style,
                             export_as_new_style, test_nat_type);
      writeLog(LOG_TYPE_INFO, "Result saved to " + pngpath + " .");
      printMsg(SPEEDTEST_MESSAGE_PICSAVED, rpcmode, pngpath);
      if (rpcmode)
        printMsg(SPEEDTEST_MESSAGE_PICDATA, rpcmode,
                 "data:image/png;base64," + fileToBase64(pngpath));
    }
  }
  cur_node_id = -1;
}
// 新增：从单节点 config.json 中提取第一个 outbound，拷贝到目标 allocator
static bool extract_first_outbound(const std::string &single_config_json,
                                   rapidjson::Value &out,
                                   rapidjson::Document::AllocatorType &alloc) {
  rapidjson::Document d;
  d.Parse(single_config_json.c_str());
  if (d.HasParseError() || !d.IsObject())
    return false;
  if (!d.HasMember("outbounds") || !d["outbounds"].IsArray() ||
      d["outbounds"].Empty())
    return false;
  const auto &ob0 = d["outbounds"][0];
  out.CopyFrom(ob0, alloc);
  return true;
}

// 新增：构造聚合 v2ray 配置（多入站/多出站 + 路由一一映射）
static std::string buildAggregatedV2RayConfig(
    const std::vector<std::pair<nodeInfo *, int>> &items) {
  using namespace rapidjson;
  Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  // log
  {
    Value log(kObjectType);
    log.AddMember("loglevel", "warning", alloc);
    doc.AddMember("log", log, alloc);
  }

  // inbounds
  Value inbounds(kArrayType);
  // outbounds
  Value outbounds(kArrayType);
  // routing
  Value routing(kObjectType);
  Value rules(kArrayType);

  for (auto &p : items) {
    nodeInfo *node = p.first;
    int port = p.second;
    std::string in_tag = "in_" + std::to_string(node->id);
    std::string out_tag = "out_" + std::to_string(node->id);

    // inbound: socks with udp enabled
    {
      Value inbound(kObjectType);
      inbound.AddMember(
          "tag", Value(in_tag.c_str(), (SizeType)in_tag.size(), alloc), alloc);
      inbound.AddMember("port", port, alloc);
      inbound.AddMember("listen", "127.0.0.1", alloc);
      inbound.AddMember("protocol", "socks", alloc);

      Value settings(kObjectType);
      settings.AddMember("udp", true, alloc);
      settings.AddMember("auth", "noauth", alloc);
      inbound.AddMember("settings", settings, alloc);

      inbounds.PushBack(inbound, alloc);
    }

    // outbound: 拷贝单节点出站并设置 tag
    {
      Value outbound(kObjectType);
      if (!extract_first_outbound(node->proxyStr, outbound, alloc)) {
        // 如果解析失败，构造一个直连作为兜底，避免 v2ray 配置非法
        Value fail_ob(kObjectType);
        fail_ob.AddMember("protocol", "freedom", alloc);
        Value settings(kObjectType);
        fail_ob.AddMember("settings", settings, alloc);
        fail_ob.AddMember(
            "tag", Value(out_tag.c_str(), (SizeType)out_tag.size(), alloc),
            alloc);
        outbounds.PushBack(fail_ob, alloc);
      } else {
        // 覆盖/设置 tag
        if (outbound.HasMember("tag"))
          outbound.RemoveMember("tag");
        outbound.AddMember(
            "tag", Value(out_tag.c_str(), (SizeType)out_tag.size(), alloc),
            alloc);
        // 不在聚合侧二次清洗；单节点构建阶段已完成必要规范化
        outbounds.PushBack(outbound, alloc);
      }
    }

    // routing rule: inbound tag -> outbound tag
    {
      Value rule(kObjectType);
      rule.AddMember("type", "field", alloc);

      Value inTags(kArrayType);
      inTags.PushBack(Value(in_tag.c_str(), (SizeType)in_tag.size(), alloc),
                      alloc);
      rule.AddMember("inboundTag", inTags, alloc);

      rule.AddMember("outboundTag",
                     Value(out_tag.c_str(), (SizeType)out_tag.size(), alloc),
                     alloc);
      rules.PushBack(rule, alloc);
    }
  }

  routing.AddMember("rules", rules, alloc);
  routing.AddMember("domainStrategy", "AsIs", alloc);

  doc.AddMember("inbounds", inbounds, alloc);
  doc.AddMember("outbounds", outbounds, alloc);
  doc.AddMember("routing", routing, alloc);

  // 可选：提供一个额外 direct 出站供内部使用（非必须）
  // 这里不添加，保持极简

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  return sb.GetString();
}

// 新增：准备分片端口（避免冲突，逐一使用 checkPort）
static std::vector<int> allocateShardPorts(size_t count, int start_hint_port) {
  std::vector<int> ports;
  ports.reserve(count);
  std::set<int> used;
  int hint = start_hint_port;
  for (size_t i = 0; i < count; ++i) {
    int p = checkPort(hint);
    if (p == -1) {
      // 已经从 hint 向上扫到 65535 都不可用，停止分配
      break;
    }
    // 去重保护：避免重复使用端口
    while (used.count(p)) {
      hint = p + 1;
      p = checkPort(hint);
      if (p == -1)
        break;
    }
    if (p == -1)
      break;

    ports.push_back(p);
    used.insert(p);
    hint = p + 1;
  }
  return ports;
}

// 新增：使用已就绪的本地 SOCKS 代理测试一个节点（并发调用的核心）
static int testNodeViaPreparedSocks(nodeInfo &node,
                                    const std::string &testserver, int testport,
                                    const std::string &username,
                                    const std::string &password) {
  // 基于 singleProtocalTest 的核心流程（移除写配置/启动/关闭客户端）
  node.remarks = trim(removeEmoji(node.remarks));
  int retVal = 0;
  std::string logdata, proxy;
  node.ulTarget = def_upload_target;
  std::string id = std::to_string(node.id + (rpcmode ? 0 : 1));

  // 面包屑：并发测试（已就绪的 SOCKS）开始
  set_breadcrumb(node, "preparedSocks:begin");

  writeLog(LOG_TYPE_INFO,
           "Received server. Group: " + node.group + " Name: " + node.remarks);
  auto start = steady_clock::now();

  if (!rpcmode)
    printMsg(SPEEDTEST_MESSAGE_GOTSERVER, rpcmode, id, node.group, node.remarks,
             std::to_string(node_count));

  try {
    proxy = buildSocks5ProxyString(testserver, testport, username, password);
    // TCP ping（直连，不走代理）
    printMsg(SPEEDTEST_MESSAGE_STARTPING, rpcmode, id);
    if (speedtest_mode != "speedonly") {
      writeLog(LOG_TYPE_INFO, "Now performing TCP ping...");
      retVal = tcping(node);
      if (retVal == SPEEDTEST_ERROR_NORESOLVE) {
        writeLog(LOG_TYPE_ERROR, "Node address resolve error.");
        printMsg(SPEEDTEST_ERROR_NORESOLVE, rpcmode, id);
        return SPEEDTEST_ERROR_NORESOLVE;
      }
      if (node.pkLoss == "100.00%") {
        writeLog(LOG_TYPE_ERROR, "Cannot connect to this node.");
        printMsg(SPEEDTEST_ERROR_NOCONNECTION, rpcmode, id);
        return SPEEDTEST_ERROR_NOCONNECTION;
      }
      // 修复：直接聚合 rawPing，不做空检查
      logdata = std::accumulate(
          std::next(std::begin(node.rawPing)), std::end(node.rawPing),
          std::to_string(node.rawPing[0]), [](std::string a, int b) {
            return std::move(a) + " " + std::to_string(b);
          });
      writeLog(LOG_TYPE_RAW, logdata);
      writeLog(LOG_TYPE_INFO,
               "TCP Ping: " + node.avgPing + "  Packet Loss: " + node.pkLoss);
    } else {
      node.pkLoss = "0.00%";
    }
    printMsg(SPEEDTEST_MESSAGE_GOTPING, rpcmode, id, node.avgPing, node.pkLoss);

    // 开始测试google时延
    if (test_site_ping) {
      printMsg(SPEEDTEST_MESSAGE_STARTGPING, rpcmode, id);
      writeLog(LOG_TYPE_INFO, "Now performing site ping...");

      // 面包屑：站点时延阶段
      set_breadcrumb(node, "preparedSocks:sitePing");

      sitePing(node, testserver, testport, username, password, site_ping_url);
      // 修复：直接聚合 rawSitePing，不做空检查
      logdata = std::accumulate(
          std::next(std::begin(node.rawSitePing)), std::end(node.rawSitePing),
          std::to_string(node.rawSitePing[0]), [](std::string a, int b) {
            return std::move(a) + " " + std::to_string(b);
          });
      writeLog(LOG_TYPE_RAW, logdata);
      writeLog(LOG_TYPE_INFO, "Site ping: " + node.sitePing);
      printMsg(SPEEDTEST_MESSAGE_GOTGPING, rpcmode, id, node.sitePing);
      // 如果所有时延都失败，就返回
      bool anySuccess = false;
      for (int v : node.rawSitePing) {
        if (v > 0) {
          anySuccess = true;
          break;
        }
      }
      if (!anySuccess) {
        writeLog(LOG_TYPE_ERROR, "Site ping failed or timed out.");
        printMsg(SPEEDTEST_ERROR_NOCONNECTION, rpcmode, id);
        return SPEEDTEST_ERROR_NOCONNECTION;
      }
    }

    // 开始测试下载速度
    printMsg(SPEEDTEST_MESSAGE_STARTSPEED, rpcmode, id);
    if (speedtest_mode != "pingonly") {
      getTestFile(node, proxy, downloadFiles, matchRules, def_test_file);
      writeLog(LOG_TYPE_INFO, "Now performing file download speed test...");

      // 面包屑：下载测速阶段
      set_breadcrumb(node, "preparedSocks:download");

      perform_test(node, testserver, testport, username, password,
                   def_thread_count);

      // 修复：直接聚合 rawSpeed，不做空检查
      logdata = std::accumulate(
          std::next(std::begin(node.rawSpeed)), std::end(node.rawSpeed),
          std::to_string(node.rawSpeed[0]), [](std::string a, int b) {
            return std::move(a) + " " + std::to_string(b);
          });
      writeLog(LOG_TYPE_RAW, logdata);

      if (node.totalRecvBytes == 0) {
        writeLog(LOG_TYPE_ERROR, "Speedtest returned no speed.");
        printMsg(SPEEDTEST_ERROR_RETEST, rpcmode, id);

        // 面包屑：下载重测阶段
        set_breadcrumb(node, "preparedSocks:download_retest");

        perform_test(node, testserver, testport, username, password,
                     def_thread_count);

        // 修复：直接聚合 rawSpeed（重测），不做空检查
        logdata = std::accumulate(
            std::next(std::begin(node.rawSpeed)), std::end(node.rawSpeed),
            std::to_string(node.rawSpeed[0]), [](std::string a, int b) {
              return std::move(a) + " " + std::to_string(b);
            });
        writeLog(LOG_TYPE_RAW, logdata);

        if (node.totalRecvBytes == 0) {
          writeLog(LOG_TYPE_ERROR, "Speedtest returned no speed 2 times.");
          printMsg(SPEEDTEST_ERROR_NOSPEED, rpcmode, id);
          printMsg(SPEEDTEST_MESSAGE_GOTSPEED, rpcmode, id, node.avgSpeed,
                   node.maxSpeed);
          return SPEEDTEST_ERROR_NOSPEED;
        }
      }
    }
    printMsg(SPEEDTEST_MESSAGE_GOTSPEED, rpcmode, id, node.avgSpeed,
             node.maxSpeed);

    // 开始测试上传
    if (test_upload) {
      writeLog(LOG_TYPE_INFO, "Now performing upload speed test...");
      printMsg(SPEEDTEST_MESSAGE_STARTUPD, rpcmode, id);

      // 面包屑：上传测速阶段
      set_breadcrumb(node, "preparedSocks:upload");

      upload_test(node, testserver, testport, username, password);
      printMsg(SPEEDTEST_MESSAGE_GOTUPD, rpcmode, id, node.ulSpeed);
    }
  } catch (const std::exception &ex) {
    set_breadcrumb(node, "preparedSocks:exception");
    writeLog(
        LOG_TYPE_ERROR,
        std::string("Unhandled std::exception in testNodeViaPreparedSocks: ") +
            ex.what());
    node.online = false;
    if (!rpcmode)
      printMsg(SPEEDTEST_ERROR_NOSPEED, rpcmode, id);
    return SPEEDTEST_ERROR_NOSPEED;
  } catch (...) {
    set_breadcrumb(node, "preparedSocks:exception");
    writeLog(LOG_TYPE_ERROR,
             "Unhandled non-std exception in testNodeViaPreparedSocks.");
    node.online = false;
    if (!rpcmode)
      printMsg(SPEEDTEST_ERROR_NOSPEED, rpcmode, id);
    return SPEEDTEST_ERROR_NOSPEED;
  }

  // 关键修复：Web 模式下，确保异步任务在返回前完成，避免被随后 kill 的 v2ray
  // 启动异步NAT类型检测任务（如果启用）
  if (test_nat_type) {
    printMsg(SPEEDTEST_MESSAGE_STARTNAT, rpcmode, id);
    node.natType.set(std::async(std::launch::async, [testserver, testport,
                                                     username, password]() {
      return get_nat_type_thru_socks5(testserver, testport, username, password);
    }));
  }
  // 启动异步GeoIP检测任务// 获得IP所属国家区域信息
  writeLog(LOG_TYPE_INFO, "Now started fetching GeoIP info...");
  printMsg(SPEEDTEST_MESSAGE_STARTGEOIP, rpcmode, id);
  node.inboundGeoIP.set(std::async(
      std::launch::async, [node]() { return getGeoIPInfo(node.server, ""); }));
  node.outboundGeoIP.set(std::async(
      std::launch::async, [proxy]() { return getGeoIPInfo("", proxy); }));
  // 等待异步任务完成并处理结果
  if (!webserver_mode) {
    // 控制台模式：获取并显示详细结果
    processGeoIPResult(node, rpcmode, id);
    if (test_nat_type) {
      printMsg(SPEEDTEST_MESSAGE_GOTNAT, rpcmode, id, node.natType.get());
    }
  } else {
    // Web模式：仅确保数据就绪，防止/getresults阻塞
    ensureAsyncTasksReady(node);
  }

  // 开始总结
  writeLog(LOG_TYPE_INFO,
           "Average speed: " + node.avgSpeed + "  Max speed: " + node.maxSpeed +
               "  Upload speed: " + node.ulSpeed + "  Traffic used in bytes: " +
               std::to_string(node.totalRecvBytes));
  node.online = true;
  auto end = steady_clock::now();
  auto lapse = duration_cast<seconds>(end - start);
  node.duration = lapse.count();
  return SPEEDTEST_ERROR_NONE;
}

static bool tryStartSubsetViaStdin(const std::vector<nodeInfo *> &subset,
                                   const std::vector<int> &ports) {
  if (subset.empty() || subset.size() != ports.size())
    return false;

  // 构造 items 并生成聚合配置（内存）
  std::vector<std::pair<nodeInfo *, int>> items;
  items.reserve(subset.size());
  for (size_t i = 0; i < subset.size(); ++i) {
    items.emplace_back(subset[i], ports[i]);
  }
  std::string config = buildAggregatedV2RayConfig(items);

  // 先做 -test 预检（stdin），失败则直接返回 false
  if (!testV2RayConfigStdin(config, "")) {
    writeLog(LOG_TYPE_ERROR,
             "Aggregated v2ray config (-test) failed. Subset cannot start.");
    return false;
  }

  // 使用 stdin 喂给 v2ray（免写盘，若不可用则在辅助函数中回退写盘）
  bool ok = runV2RayWithConfigStdin(config, "");
  if (!ok)
    return false;

  // 等待每个入站端口就绪（若有任何一个未就绪，视为失败）
  bool ready = true;
  for (auto p : ports) {
    if (!waitUntilSocksReady(socksaddr, p, "", "", 2000)) {
      ready = false;
      break;
    }
  }

  // 无论成功失败都终止子进程
  killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
  sleep(200);

  return ready;
}

// 二分递归：找出所有有效节点（无效节点设置 online=false 并推送）
static std::vector<nodeInfo *> getValidNodes(std::vector<nodeInfo *> &shard) {
  writeLog(LOG_TYPE_WARN,
           "Client startup failed. Trying to locate invalid nodes by "
           "divide-and-conquer...");

  std::function<std::vector<nodeInfo *>(const std::vector<nodeInfo *> &)>
      filter_valid =
          [&](const std::vector<nodeInfo *> &nodes) -> std::vector<nodeInfo *> {
    if (nodes.empty())
      return {};

    // 为当前子集临时分配端口
    auto ports = allocateShardPorts(nodes.size(), socksport);
    if (ports.size() != nodes.size()) {
      writeLog(LOG_TYPE_ERROR,
               "No enough local ports for subset testing. Mark subset failed.");
      // 端口不足时，保守地全部标记为失败
      for (auto *n : nodes) {
        n->online = false;
        webui_notify_node_tested(*n);
      }
      return {};
    }

    if (tryStartSubsetViaStdin(nodes, ports)) {
      // 整体子集可以启动，全部有效
      return nodes;
    }

    // 子集整体无法启动：若仅一个节点，标记无效并推送
    if (nodes.size() == 1) {
      nodeInfo *bad = nodes[0];
      bad->online = false;
      writeLog(LOG_TYPE_ERROR,
               "Located invalid node id=" + std::to_string(bad->id) + " (" +
                   bad->remarks + ")");
      webui_notify_node_tested(*bad);
      return {};
    }

    // 分治：左右子集分别判定
    size_t mid = nodes.size() / 2;
    std::vector<nodeInfo *> left(nodes.begin(), nodes.begin() + mid);
    std::vector<nodeInfo *> right(nodes.begin() + mid, nodes.end());

    auto valid_left = filter_valid(left);
    auto valid_right = filter_valid(right);

    // 合并有效子集
    valid_left.insert(valid_left.end(), valid_right.begin(), valid_right.end());
    return valid_left;
  };

  auto valid = filter_valid(shard);
  size_t removed = shard.size() - valid.size();
  writeLog(LOG_TYPE_WARN,
           "Invalid nodes removed from shard: " + std::to_string(removed) +
               " of " + std::to_string(shard.size()));
  return valid;
}
// 新增：分片准备（分配端口 + 生成并写入聚合 config.json）
static bool prepareShardConfigAndPorts(const std::vector<nodeInfo *> &shard,
                                       std::vector<int> &ports) {
  // 1) 分配端口
  ports = allocateShardPorts(shard.size(), socksport);
  if (ports.size() != shard.size()) {
    writeLog(LOG_TYPE_ERROR,
             "Insufficient local ports for shard. Mark shard nodes as failed.");
    return false;
  }

  // 2) 生成 items -> 构建聚合配置
  std::vector<std::pair<nodeInfo *, int>> items;
  items.reserve(shard.size());
  for (size_t k = 0; k < shard.size(); ++k) {
    items.emplace_back(shard[k], ports[k]);
  }
  std::string config = buildAggregatedV2RayConfig(items);

  // 3) 写盘（config.json）
  writeLog(LOG_TYPE_INFO, "Writing aggregated config file...");
  fileWrite("config.json", config, true);
  return true;
}

// 新增：标准流程的并发测试（sleep->就绪->并发测试->web通知）
static void testShardWithConcurrency(std::vector<nodeInfo *> &shard,
                                     const std::vector<int> &ports,
                                     int concurrency) {
  // 给入站监听一个极短就绪窗口，避免竞态误判
  sleep(300);

  // 等待端口就绪（最多 3s），未就绪标为失败并推送
  std::vector<size_t> ready_indices;
  ready_indices.reserve(shard.size());
  for (size_t k = 0; k < shard.size(); ++k) {
    int p = ports[k];
    if (waitUntilSocksReady(socksaddr, p, "", "", 3000)) {
      ready_indices.push_back(k);
    } else {
      writeLog(LOG_TYPE_WARN, "SOCKS inbound seems not ready at " + socksaddr +
                                  ":" + std::to_string(p) +
                                  " after waiting. Mark as failed.");
      shard[k]->online = false;
      webui_notify_node_tested(*shard[k]);
    }
  }
  if (ready_indices.empty()) {
    writeLog(LOG_TYPE_WARN, "No ready inbound ports in this shard.");
    return;
  }

  // 控制并发度的并发测试 + web 通知
  std::vector<std::future<void>> futures;
  for (auto idx : ready_indices) {
    while (futures.size() >= (size_t)concurrency) {
      futures.front().get();
      futures.erase(futures.begin());
    }
    nodeInfo *n = shard[idx];
    int p = ports[idx];
    futures.emplace_back(std::async(std::launch::async, [n, p]() {
      testNodeViaPreparedSocks(*n, socksaddr, p, "", "");
      webui_notify_node_tested(*n);
      // 新增：单节点结果立刻追加保存（并发环境下有互斥保护）
      saveResult(*n);
    }));
  }
  for (auto &f : futures)
    f.get();
}
// 新增：对某协议分组（VMESS 或 VLESS）进行“分片 + 单进程 v2ray 并发测试”
static void testV2RayShards(std::vector<nodeInfo *> &group, int shard_size,
                            int concurrency) {
  if (group.empty())
    return;

  // 分片迭代：严格 1→2→3→4→5 流程
  for (size_t i = 0; i < group.size(); i += shard_size) {
    size_t j_end = std::min(group.size(), i + (size_t)shard_size);
    std::vector<nodeInfo *> shard(group.begin() + i, group.begin() + j_end);

    // 面包屑：进入一个分片迭代
    set_breadcrumb(-1, "testV2RayShards:iter_begin");

    try {
      // (1) 分配端口并生成/写入配置文件
      std::vector<int> ports;

      // 面包屑：准备分片配置（分配端口+生成配置）
      set_breadcrumb(-1, "testV2RayShards:prepare_shard");

      if (!prepareShardConfigAndPorts(shard, ports)) {
        for (auto *n : shard) {
          n->online = false;
          webui_notify_node_tested(*n);
        }
        continue; // 下一个分片
      }

      // 面包屑：分片配置就绪
      set_breadcrumb(-1, "testV2RayShards:prepare_ok");

      // (2) 启动客户端
      set_breadcrumb(-1, "testV2RayShards:start_client");
      bool ok = runClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
      if (!ok) {
        // 面包屑：首次启动失败
        set_breadcrumb(-1, "testV2RayShards:client_fail");

        // (3) 启动失败：剔除失败节点，得到正常节点集合
        auto shard_valid = getValidNodes(shard);
        if (shard_valid.empty()) {
          writeLog(
              LOG_TYPE_ERROR,
              "Shard has no valid nodes after removal. Mark all as failed.");
          for (auto *n : shard) {
            n->online = false;
            webui_notify_node_tested(*n);
          }
          continue; // 下一个分片
        }

        // (4) 对有效子集：重新分配端口+生成/写入配置文件，然后再次启动
        std::vector<int> ports2;
        if (!prepareShardConfigAndPorts(shard_valid, ports2)) {
          for (auto *n : shard_valid) {
            n->online = false;
            webui_notify_node_tested(*n);
          }
          continue;
        }

        // 面包屑：重试启动客户端
        set_breadcrumb(-1, "testV2RayShards:client_retry_start");
        bool ok2 = runClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
        if (!ok2) {
          // 面包屑：重试仍失败
          set_breadcrumb(-1, "testV2RayShards:client_retry_fail");

          writeLog(LOG_TYPE_ERROR,
                   "Client startup still failed after removing invalid nodes. "
                   "Mark subset nodes as failed.");
          for (auto *n : shard_valid) {
            n->online = false;
            webui_notify_node_tested(*n);
          }
          continue; // 下一个分片
        }

        // (5) 标准流程：sleep->端口就绪->并发测试->汇集输出
        set_breadcrumb(-1, "testV2RayShards:concurrency_test");
        testShardWithConcurrency(shard_valid, ports2, concurrency);

        // 终止客户端
        set_breadcrumb(-1, "testV2RayShards:kill_client");
        killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
        sleep(200);
        continue; // 下一个分片
      }

      // 初次启动成功：(5) 标准流程：sleep->端口就绪->并发测试->汇集输出
      set_breadcrumb(-1, "testV2RayShards:concurrency_test");
      testShardWithConcurrency(shard, ports, concurrency);

      // 终止客户端
      set_breadcrumb(-1, "testV2RayShards:kill_client");
      killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
      sleep(200);
    } catch (const std::exception &ex) {
      // 面包屑：分片迭代异常
      set_breadcrumb(-1, "testV2RayShards:exception");
      writeLog(LOG_TYPE_ERROR,
               std::string(
                   "Unhandled std::exception in testV2RayShards iteration: ") +
                   ex.what());
      // 尝试清理客户端，避免遗留子进程
      killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
      sleep(200);
      // 保守处理：标记分片内所有节点失败并通知
      for (auto *n : shard) {
        n->online = false;
        webui_notify_node_tested(*n);
      }
    } catch (...) {
      set_breadcrumb(-1, "testV2RayShards:exception");
      writeLog(LOG_TYPE_ERROR,
               "Unhandled non-std exception in testV2RayShards iteration.");
      killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
      sleep(200);
      for (auto *n : shard) {
        n->online = false;
        webui_notify_node_tested(*n);
      }
    }
  }
}
void rewriteNodeID(std::vector<nodeInfo> &nodes) {
  int index = 0;
  for (auto &x : nodes) {
    if (x.proxyStr == "LOG")
      return;
    x.id = index;
    index++;
  }
}

void rewriteNodeGroupID(std::vector<nodeInfo> &nodes, int groupID) {
  std::for_each(nodes.begin(), nodes.end(),
                [&](nodeInfo &x) { x.groupID = groupID; });
}

// 全局去重：按 protocol(linkType) + host(server) + port 唯一，保序
static void dedupNodesByProtoHostPort(std::vector<nodeInfo> &nodes) {
  std::unordered_set<std::string> seen;
  std::vector<nodeInfo> out;
  out.reserve(nodes.size());

  for (auto &n : nodes) {
    // 从结果导入（LOG）不参与去重
    if (n.proxyStr == "LOG") {
      out.push_back(n);
      continue;
    }
    const std::string key = std::to_string(n.linkType) + "|" +
                            toLower(trim(n.server)) + "|" +
                            std::to_string(n.port);
    if (seen.insert(key).second) {
      out.push_back(n); // 首次出现，保留
    }
  }

  if (out.size() != nodes.size()) {
    writeLog(LOG_TYPE_INFO, "Global de-dup (protocol+host+port): " +
                                std::to_string(nodes.size()) + " -> " +
                                std::to_string(out.size()));
  }
  nodes.swap(out);
}

void addNodes(std::string link, bool multilink) {
  int linkType = -1;
  std::vector<nodeInfo> nodes;
  nodeInfo node;
  std::string strSub, strInput, fileContent, strProxy;

  link = replace_all_distinct(link, "\"", "");
  writeLog(LOG_TYPE_INFO, "Received Link.");
  if (startsWith(link, "vmess://") || startsWith(link, "vmess1://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
  else if (startsWith(link, "vless://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
  else if (startsWith(link, "ss://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDSS;
  else if (startsWith(link, "ssr://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
  else if (startsWith(link, "socks://") ||
           startsWith(link, "https://t.me/socks") ||
           startsWith(link, "tg://socks"))
    linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
  else if (startsWith(link, "trojan://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
  else if (startsWith(link, "http://") || startsWith(link, "https://") ||
           startsWith(link, "surge:///install-config"))
    linkType = SPEEDTEST_MESSAGE_FOUNDSUB;
  else if (startsWith(link, "Netch://"))
    linkType = SPEEDTEST_MESSAGE_FOUNDNETCH;
  else if (link == "data:upload")
    linkType = SPEEDTEST_MESSAGE_FOUNDUPD;
  else if (fileExist(link))
    linkType = SPEEDTEST_MESSAGE_FOUNDLOCAL;

  switch (linkType) {
  case SPEEDTEST_MESSAGE_FOUNDSUB:
    printMsg(SPEEDTEST_MESSAGE_FOUNDSUB, rpcmode);
    if (!rpcmode && !multilink && !webserver_mode && !sub_url.size()) {
      printMsg(SPEEDTEST_MESSAGE_GROUP, rpcmode);
      getline(std::cin, strInput);
      if (strInput.size()) {
        custom_group = rpcmode ? strInput : ACPToUTF8(strInput);
        writeLog(LOG_TYPE_INFO, "Received custom group: " + custom_group);
      }
    }
    writeLog(LOG_TYPE_INFO, "Downloading subscription data...");
    printMsg(SPEEDTEST_MESSAGE_FETCHSUB, rpcmode);
    if (strFind(link, "surge:///install-config")) // surge config link
      link = UrlDecode(getUrlArg(link, "url"));
    strSub = webGet(link);
    if (strSub.size() == 0) {
      if (webGetWasLimited()) {
        writeLog(LOG_TYPE_WARN, "Subscription size exceeded limit (" +
                                    std::to_string(http_get_max_size_mb) +
                                    " MB).");
        printMsg(SPEEDTEST_MESSAGE_SUBTOOLARGE, rpcmode,
                 std::to_string(http_get_max_size_mb));
      } else {
        // try to get it again with system proxy
        writeLog(LOG_TYPE_WARN,
                 "Cannot download subscription directly. Using system proxy.");
        strProxy = getSystemProxy();
        if (strProxy.size()) {
          printMsg(SPEEDTEST_ERROR_SUBFETCHERR, rpcmode);
          strSub = webGet(link, strProxy);
        } else
          writeLog(LOG_TYPE_WARN, "No system proxy is set. Skipping.");
      }
    }
    if (strSub.size()) {
      writeLog(LOG_TYPE_INFO, "Parsing subscription data...");
      explodeConfContent(strSub, override_conf_port, ss_libev, ssr_libev,
                         nodes);
      filterNodes(nodes, custom_exclude_remarks, custom_include_remarks,
                  curGroupID);
      copyNodes(nodes, allNodes);
    } else {
      writeLog(LOG_TYPE_ERROR, "Cannot download subscription data.");
      printMsg(SPEEDTEST_ERROR_INVALIDSUB, rpcmode);
    }
    break;
  case SPEEDTEST_MESSAGE_FOUNDLOCAL:
    printMsg(SPEEDTEST_MESSAGE_FOUNDLOCAL, rpcmode);
    if (!rpcmode && !multilink && !sub_url.size()) {
      printMsg(SPEEDTEST_MESSAGE_GROUP, rpcmode);
      getline(std::cin, strInput);
      if (strInput.size()) {
        custom_group = rpcmode ? strInput : ACPToUTF8(strInput);
        writeLog(LOG_TYPE_INFO, "Received custom group: " + custom_group);
      }
    }
    writeLog(LOG_TYPE_INFO, "Parsing configuration file data...");
    printMsg(SPEEDTEST_MESSAGE_PARSING, rpcmode);

    if (explodeLog(fileGet(link), nodes) == -1) {
      if (explodeConf(link, override_conf_port, ss_libev, ssr_libev, nodes) ==
          SPEEDTEST_ERROR_UNRECOGFILE) {
        printMsg(SPEEDTEST_ERROR_UNRECOGFILE, rpcmode);
        writeLog(LOG_TYPE_ERROR, "Invalid configuration file!");
        break;
      }
    }
    filterNodes(nodes, custom_exclude_remarks, custom_include_remarks,
                curGroupID);
    copyNodes(nodes, allNodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDUPD:
    printMsg(SPEEDTEST_MESSAGE_FOUNDUPD, rpcmode);
    std::cin.clear();
    // now we should ready to receive a large amount of data from stdin
    getline(std::cin, fileContent);
    fileContent = base64_decode(fileContent.substr(fileContent.find(",") + 1));
    writeLog(LOG_TYPE_INFO, "Parsing configuration file data...");
    printMsg(SPEEDTEST_MESSAGE_PARSING, rpcmode);
    if (explodeConfContent(fileContent, override_conf_port, ss_libev, ssr_libev,
                           nodes) == SPEEDTEST_ERROR_UNRECOGFILE) {
      printMsg(SPEEDTEST_ERROR_UNRECOGFILE, rpcmode);
      writeLog(LOG_TYPE_ERROR, "Invalid configuration file!");
    } else {
      filterNodes(nodes, custom_exclude_remarks, custom_include_remarks,
                  curGroupID);
      copyNodes(nodes, allNodes);
    }
    break;
  default:
    if (linkType > 0) {
      node_count = 1;
      printMsg(linkType, rpcmode);
      explode(link, ss_libev, ssr_libev, override_conf_port, node);
      if (custom_group.size() != 0)
        node.group = custom_group;
      if (node.server.empty()) {
        writeLog(LOG_TYPE_ERROR, "No valid link found.");
        printMsg(SPEEDTEST_ERROR_NORECOGLINK, rpcmode);
      } else {
        node.groupID = curGroupID;
        allNodes.push_back(node);
      }
    } else {
      writeLog(LOG_TYPE_ERROR, "No valid link found.");
      printMsg(SPEEDTEST_ERROR_NORECOGLINK, rpcmode);
    }
  }
}

void setcd(std::string &file) {
  char filename[256] = {};
  std::string path;
#ifdef _WIN32
  char szTemp[1024] = {};
  char *pname = NULL;
  DWORD retVal = GetFullPathName(file.data(), 1023, szTemp, &pname);
  if (!retVal)
    return;
  strcpy(filename, pname);
  strrchr(szTemp, '\\')[1] = '\0';
  path.assign(szTemp);
#else
  char *ret = realpath(file.data(), NULL);
  if (ret == NULL)
    return;
  strncpy(filename, strrchr(ret, '/') + 1, 255);
  strrchr(ret, '/')[1] = '\0';
  path.assign(ret);
  free(ret);
#endif // _WIN32
  file.assign(filename);
  chdir(path.data());
}

int main(int argc, char *argv[]) {
  std::vector<nodeInfo> nodes;
  nodeInfo node;
  std::string link;
  std::string curPNGPath, curPNGPathPrefix;
  std::cout << std::fixed;
  std::cout << std::setprecision(2);

#ifndef _DEBUG
  std::string prgpath = argv[0];
  setcd(prgpath); // switch to program directory
#endif            // _DEBUG
  chkArg(argc, argv);
  makeDir("logs");
  makeDir("results");
  logInit(rpcmode);
  readConf("pref.ini");

#ifdef _WIN32
  // start up windows socket library first
  WSADATA wsd;
  if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
    printMsg(SPEEDTEST_ERROR_WSAERR, rpcmode);
    return -1;
  }
  // along with some console window info
  SetConsoleOutputCP(65001);
#else
  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGABRT, SIG_IGN);
  signal(SIGHUP, signalHandler);
  signal(SIGQUIT, signalHandler);
#endif // _WIN32
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!rpcmode)
    SetConsoleTitle("StairSpeedtest " VERSION);

  // kill any client before testing
#ifdef __APPLE__
  killClient(SPEEDTEST_MESSAGE_FOUNDSS);
  killClient(SPEEDTEST_MESSAGE_FOUNDSSR);
  killClient(SPEEDTEST_MESSAGE_FOUNDVMESS);
  killClient(SPEEDTEST_MESSAGE_FOUNDVLESS);
  killClient(SPEEDTEST_MESSAGE_FOUNDTROJAN);
#endif // __APPLE__
  clientCheck();
  socksport = checkPort(socksport);
  if (socksport == -1) {
    writeLog(LOG_TYPE_ERROR, "No available local port starting from 32768.");
    return -1;
  }
  writeLog(LOG_TYPE_INFO, "Using local port: " + std::to_string(socksport));
  writeLog(LOG_TYPE_INFO, "Init completed.");
  // intro message
  if (webserver_mode) {
    stairspeed_webserver_routine(listen_address, listen_port);
    return 0;
  }
  printMsg(SPEEDTEST_MESSAGE_WELCOME, rpcmode);
  if (sub_url.size()) {
    link = sub_url;
    std::cout << "Provided from argument.\n" << std::endl;
  } else {
    getline(std::cin, link);
    if (!rpcmode)
      link = ACPToUTF8(link);
    writeLog(LOG_TYPE_INFO, "Input data: " + link);
    if (rpcmode) {
      string_array webargs = split(link, "^");
      if (webargs.size() == 6) {
        link = webargs[0];
        if (webargs[1] != "?empty?")
          custom_group = webargs[1];
        speedtest_mode = webargs[2];
        export_sort_method = webargs[4];
        export_with_maxspeed = webargs[5] == "true";
      } else {
        link = "?empty?";
      }
    }
  }

  if (strFind(link, "|")) {
    multilink = true;
    printMsg(SPEEDTEST_MESSAGE_MULTILINK, rpcmode);
    string_array linkList = split(link, "|");
    for (auto &x : linkList) {
      addNodes(x, multilink);
      curGroupID++;
    }
  } else {
    addNodes(link, multilink);
  }
  dedupNodesByProtoHostPort(allNodes); // 新增：全局去重（按协议+主机+端口）
  rewriteNodeID(allNodes);             // reset all index
  node_count = allNodes.size();
  if (allNodes.size() > 1) // group or multi-link
  {
    batchTest(allNodes);
    if (multilink) {
      if (export_picture) {
        if (multilink_export_as_one_image) {
          printMsg(SPEEDTEST_MESSAGE_PICSAVING, rpcmode);
          writeLog(LOG_TYPE_INFO, "Now exporting result...");
          curPNGPath = replace_all_distinct(resultPath, ".log", "") +
                       "-multilink-all.png";
          pngpath = exportRender(curPNGPath, allNodes, export_with_maxspeed,
                                 export_sort_method, export_color_style,
                                 export_as_new_style, test_nat_type);
          printMsg(SPEEDTEST_MESSAGE_PICSAVED, rpcmode, pngpath);
          writeLog(LOG_TYPE_INFO, "Result saved to " + pngpath + " .");
          if (rpcmode)
            printMsg(SPEEDTEST_MESSAGE_PICDATA, rpcmode,
                     "data:image/png;base64," + fileToBase64(pngpath));
        } else {
          printMsg(SPEEDTEST_MESSAGE_PICSAVING, rpcmode);
          curPNGPathPrefix = replace_all_distinct(resultPath, ".log", "");
          for (int i = 0; i < curGroupID; i++) {
            eraseElements(nodes);
            copyNodesWithGroupID(allNodes, nodes, i);
            if (!nodes.size())
              break;
            if ((nodes.size() == 1 && single_test_force_export) ||
                nodes.size() > 1) {
              printMsg(SPEEDTEST_MESSAGE_PICSAVINGMULTI, rpcmode,
                       std::to_string(i + 1));
              writeLog(LOG_TYPE_INFO, "Now exporting result for group " +
                                          std::to_string(i + 1) + "...");
              curPNGPath = curPNGPathPrefix + "-multilink-group" +
                           std::to_string(i + 1) + ".png";
              pngpath = exportRender(curPNGPath, nodes, export_with_maxspeed,
                                     export_sort_method, export_color_style,
                                     export_as_new_style, test_nat_type);
              printMsg(SPEEDTEST_MESSAGE_PICSAVEDMULTI, rpcmode,
                       std::to_string(i + 1), pngpath);
              writeLog(LOG_TYPE_INFO, "Group " + std::to_string(i + 1) +
                                          " result saved to " + pngpath + " .");
            } else
              writeLog(LOG_TYPE_INFO, "Group " + std::to_string(i + 1) +
                                          " result export skipped.");
          }
        }
      } else {
        writeLog(LOG_TYPE_INFO, "Result export skipped.");
      }
    }
    writeLog(LOG_TYPE_INFO, "Multi-link test completed.");
  } else if (allNodes.size() == 1) {
    writeLog(LOG_TYPE_INFO, "Speedtest will now begin.");
    printMsg(SPEEDTEST_MESSAGE_BEGIN, rpcmode);
    singleTest(allNodes[0]);
    if (export_picture && single_test_force_export) {
      printMsg(SPEEDTEST_MESSAGE_PICSAVING, rpcmode);
      writeLog(LOG_TYPE_INFO, "Now exporting result...");
      curPNGPath = "results" PATH_SLASH + getTime(1) + ".png";
      pngpath = exportRender(curPNGPath, allNodes, export_with_maxspeed,
                             export_sort_method, export_color_style,
                             export_as_new_style, test_nat_type);
      printMsg(SPEEDTEST_MESSAGE_PICSAVED, rpcmode, pngpath);
      writeLog(LOG_TYPE_INFO, "Result saved to " + pngpath + " .");
      if (rpcmode)
        printMsg(SPEEDTEST_MESSAGE_PICDATA, rpcmode,
                 "data:image/png;base64," + fileToBase64(pngpath));
    }
    writeLog(LOG_TYPE_INFO, "Single node test completed.");
  } else {
    writeLog(LOG_TYPE_ERROR, "No valid link found.");
    printMsg(SPEEDTEST_ERROR_NORECOGLINK, rpcmode);
  }
  logEOF();
  printMsg(SPEEDTEST_MESSAGE_EOF, rpcmode);
  sleep(1);
  // std::cin.clear();
  // std::cin.ignore();
  if (!rpcmode && sub_url.size() && pause_on_done)
    _getch();
#ifdef _WIN32
  // stop socket library before exit
  WSACleanup();
#else
  std::cout << std::endl;
#endif // _WIN32
  return 0;
}
