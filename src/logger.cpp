#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"
#include "misc.h"
#include "nodeinfo.h"
#include "printout.h"
#include "version.h"

#include <sys/time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif // _WIN32

typedef std::lock_guard<std::mutex> guarded_mutex;
std::mutex logger_mutex;

std::string curtime, result_content;
std::string resultPath, logPath;

int makeDir(const char *path) {
#ifdef _WIN32
  return mkdir(path);
#else
  return mkdir(path, 0755);
#endif // _WIN32
}

// 新增：全局日志级别阈值（默认 INFO）
static std::atomic<int> g_log_level_threshold{LOG_LEVEL_INFO};

// 新增：将字符串日志级别转换为枚举
static int parseLogLevelStr(const std::string &s) {
  std::string v = toLower(trim(s));
  if (v == "fatal") return LOG_LEVEL_FATAL;
  if (v == "error") return LOG_LEVEL_ERROR;
  if (v == "warn" || v == "warning") return LOG_LEVEL_WARNING;
  if (v == "info") return LOG_LEVEL_INFO;
  if (v == "debug") return LOG_LEVEL_DEBUG;
  if (v == "verbose") return LOG_LEVEL_VERBOSE;
  return LOG_LEVEL_INFO;
}

// 新增：不同日志类型的默认级别映射
static int mapTypeToLevel(int type) {
  switch (type) {
  case LOG_TYPE_ERROR: return LOG_LEVEL_ERROR;
  case LOG_TYPE_WARN:  return LOG_LEVEL_WARNING;
  case LOG_TYPE_INFO:  return LOG_LEVEL_INFO;
  case LOG_TYPE_RENDER: return LOG_LEVEL_INFO;
  case LOG_TYPE_DEBUG: return LOG_LEVEL_DEBUG;
  case LOG_TYPE_RAW:   return LOG_LEVEL_VERBOSE;
  // 下面这些默认视为调试级别，便于按需降噪
  case LOG_TYPE_TCPING:
  case LOG_TYPE_GPING:
  case LOG_TYPE_FILEDL:
  case LOG_TYPE_FILEUL:
  case LOG_TYPE_RULES:
  case LOG_TYPE_GEOIP:
  case LOG_TYPE_STUN:
    return LOG_LEVEL_DEBUG;
  default:
    return LOG_LEVEL_INFO;
  }
}

std::string getTime(int type) {
  time_t lt;
  char tmpbuf[32], cMillis[7];
  std::string format;
  timeval tv;
  gettimeofday(&tv, NULL);
  snprintf(cMillis, 7, "%.6ld", (long)tv.tv_usec);
  lt = time(NULL);
  struct tm *local = localtime(&lt);
  switch (type) {
  case 1:
    format = "%Y%m%d-%H%M%S";
    break;
  case 2:
    format = "%Y/%m/%d %a %H:%M:%S." + std::string(cMillis);
    break;
  case 3:
    format = "%Y-%m-%d %H:%M:%S";
    break;
  }
  strftime(tmpbuf, 32, format.data(), local);
  return std::string(tmpbuf);
}

void logInit(bool rpcmode) {
  curtime = getTime(1);
  logPath = "logs" PATH_SLASH + curtime + ".log";
  // 从偏好设置读取日志级别（默认 info）
  std::string lv = PrefsGetStr("advanced", "log_level", "info");
  g_log_level_threshold.store(parseLogLevelStr(lv), std::memory_order_relaxed);

  std::string log_header = "Stair Speedtest " VERSION " started in ";
  if (rpcmode)
    log_header += "GUI mode.";
  else
    log_header += "CLI mode.";
  writeLog(LOG_TYPE_INFO, log_header);
}

void resultInit() {
  curtime = getTime(1);
  resultPath = "results" PATH_SLASH + curtime + ".log";
}

void writeLog(int type, std::string content, int level) {
  // 级别过滤：仅当消息级别不高于阈值时才写入
  int effective_level = (level == LOG_LEVEL_VERBOSE) ? mapTypeToLevel(type) : level;
  if (effective_level > g_log_level_threshold.load(std::memory_order_relaxed)) {
    return;
  }

  guarded_mutex guard(logger_mutex);
  std::string timestr = "[" + getTime(2) + "]", typestr = "[UNKNOWN]";
  switch (type) {
  case LOG_TYPE_ERROR:
    typestr = "[ERROR]";
    break;
  case LOG_TYPE_INFO:
    typestr = "[INFO]";
    break;
  case LOG_TYPE_RAW:
    typestr = "[RAW]";
    break;
  case LOG_TYPE_WARN:
    typestr = "[WARNING]";
    break;
  case LOG_TYPE_GEOIP:
    typestr = "[GEOIP]";
    break;
  case LOG_TYPE_TCPING:
    typestr = "[TCPING]";
    break;
  case LOG_TYPE_FILEDL:
    typestr = "[FILEDL]";
    break;
  case LOG_TYPE_FILEUL:
    typestr = "[FILEUL]";
    break;
  case LOG_TYPE_RULES:
    typestr = "[RULES]";
    break;
  case LOG_TYPE_GPING:
    typestr = "[GPING]";
    break;
  case LOG_TYPE_RENDER:
    typestr = "[RENDER]";
    break;
  case LOG_TYPE_DEBUG: // DEBUG 标签
    typestr = "[DEBUG]";
    break;
  case LOG_TYPE_STUN:
    typestr = "[STUN]";
    break;
  }
  content = timestr + typestr + content + "\n";
  fileWrite(logPath, content, false);
}

void logEOF() {
  writeLog(LOG_TYPE_INFO, "Program terminated.");
  fileWrite(logPath, "--EOF--", false);
}

// =========================
// 面包屑与崩溃拦截实现
// =========================
#ifndef BREADCRUMB_ENABLED
#define BREADCRUMB_ENABLED 1
#endif

#if defined(BREADCRUMB_ENABLED) && BREADCRUMB_ENABLED
static std::atomic<bool> g_breadcrumb_enabled{true};
#else
static std::atomic<bool> g_breadcrumb_enabled{false};
#endif

#ifdef _WIN32
static std::atomic<int> g_current_node_id{-1};
static std::atomic<const char *> g_current_stage{"init"};
static std::mutex g_breadcrumb_mutex;
static std::string g_current_node_key;
#endif

// 引用 main.cpp 中的全局节点数组，以便在仅给 id 的情况下组键
extern std::vector<nodeInfo> allNodes;

bool breadcrumbInitFromEnv()
{
#if BREADCRUMB_ENABLED
    bool enabled = PrefsBreadcrumbEnabled(true);
#else
    bool enabled = PrefsBreadcrumbEnabled(false);
#endif
    g_breadcrumb_enabled.store(enabled, std::memory_order_relaxed);
    return g_breadcrumb_enabled.load(std::memory_order_relaxed);
}


bool breadcrumbIsEnabled() {
#if defined(BREADCRUMB_ENABLED) && BREADCRUMB_ENABLED
  return g_breadcrumb_enabled.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

void set_breadcrumb(int node_id, const char *stage) {
#ifdef _WIN32
  g_current_node_id.store(node_id, std::memory_order_relaxed);
  g_current_stage.store(stage, std::memory_order_relaxed);

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
  if (breadcrumbIsEnabled()) {
    writeLog(LOG_TYPE_DEBUG,
             "Breadcrumb: node=" + key + " stage=" + std::string(stage));
  }
#else
  (void)node_id;
  (void)stage;
#endif
}

void set_breadcrumb(const nodeInfo &node, const char *stage) {
#ifdef _WIN32
  g_current_node_id.store(node.id, std::memory_order_relaxed);
  g_current_stage.store(stage, std::memory_order_relaxed);

  std::string key = std::to_string(node.id) + "|" +
                    std::to_string(node.linkType) + "|" + node.server + "|" +
                    std::to_string(node.port);
  {
    std::lock_guard<std::mutex> lk(g_breadcrumb_mutex);
    g_current_node_key = key;
  }
  if (breadcrumbIsEnabled()) {
    writeLog(LOG_TYPE_DEBUG,
             "Breadcrumb: node=" + key + " stage=" + std::string(stage));
  }
#else
  (void)node;
  (void)stage;
#endif
}

#ifdef _WIN32
static LONG WINAPI CrashUnhandledFilter(EXCEPTION_POINTERS *ep) {
  std::string msg = "Unhandled exception. Last breadcrumb: node_id=" +
                    std::to_string(g_current_node_id.load()) +
                    " stage=" + std::string(g_current_stage.load());
  writeLog(LOG_TYPE_ERROR, msg);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void installCrashFilter() {
#ifdef _WIN32
  SetUnhandledExceptionFilter(CrashUnhandledFilter);
#endif
}

/*

void resultInit(bool export_with_maxspeed)
{
    curtime = getTime(1);
    resultPath = "results" PATH_SLASH + curtime + ".log";
    result_content = "group,remarks,loss,ping,avgspeed";
    if(export_with_maxspeed)
        result_content += ",maxspeed";
    result_content += "\n";
    fileWrite(resultPath, result_content, true);
}

void writeResult(nodeInfo *node, bool export_with_maxspeed)
{
    std::string content = node->group + "," + node->remarks + "," + node->pkLoss
+ "," + node->avgPing + "," + node->avgSpeed; if(export_with_maxspeed) content
+= "," + node->maxSpeed; result_content += content + "\n";
    //write2file(resultPath,result_content.str(),true);
    writeToFile(resultPath, content, false);
}

void resultEOF(std::string traffic, int worknodes, int totnodes)
{
    result_content += "Traffic used : " + traffic + ". Working Node(s) : [" +
std::to_string(worknodes) + "/" + std::to_string(totnodes) + "]\n";
    result_content += "Generated at " + getTime(3) + "\n";
    result_content += "By Stair Speedtest " VERSION ".\n";
    writeToFile(resultPath,result_content,true);
}

void exportResult(std::string outpath, std::string utiljspath, std::string
stylepath, bool export_with_maxspeed)
{
    if(utiljspath.empty())
        return;
    std::string strInput;
    vector<std::string> params;
    ifstream inputjs, inputstyle;
    ofstream outfile;
    stringstream result_content_stream;
    result_content_stream<<result_content;
    inputjs.open(utiljspath, ios::in);
    inputstyle.open(stylepath, ios::in);
    outfile.open(outpath, ios::out);
    outfile<<"<html><head><meta http-equiv=\"Content-Type\" content=\"text/html;
charset=UTF-8\" /><style type=\"text/css\">"<<endl; while(getline(inputstyle,
strInput))
    {
        outfile<<strInput<<endl;
    }
    inputstyle.close();
    outfile<<"</style><script language=\"javascript\">"<<endl;
    while(getline(inputjs, strInput))
    {
        outfile<<strInput<<endl;
    }
    inputjs.close();
    outfile<<"</script></head><body onload=\"loadevent()\"><table id=\"table\"
rules=\"all\">"; while(getline(result_content_stream, strInput))
    {
        if(strInput.empty())
            continue;
        if(strFind(strInput, "avgspeed"))
            continue;
        if(strFind(strInput, "%,"))
        {
            params = split(strInput, ",");
            outfile<<"<tr><td>"<<params[0]<<"</td><td>"<<params[1]<<"</td><td>"<<params[2]<<"</td><td>"<<params[3]<<"</td><td
class=\"speed\">"<<params[4]<<"</td>"; if(export_with_maxspeed) outfile<<"<td
class=\"speed\">"<<params[5]<<"</td>"; outfile<<"</tr>";
        }
        if(strFind(strInput, "Traffic used :"))
            outfile<<"<tr id=\"traffic\"><td>"<<strInput<<"</td></tr>";
        if(strFind(strInput, "Generated at"))
            outfile<<"<tr id=\"gentime\"><td>"<<strInput<<"</td></tr>";
        if(strFind(strInput, "By "))
            outfile<<"<tr id=\"about\"><td>"<<strInput<<"</td></tr>";
    }
    outfile<<"</table></body></html>";
    outfile.close();
}
*/
