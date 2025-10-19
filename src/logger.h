#pragma once

#include <string>

// Log types
#define LOG_TYPE_ERROR 0
#define LOG_TYPE_INFO  1
#define LOG_TYPE_RAW   2
#define LOG_TYPE_WARN  3
#define LOG_TYPE_GEOIP 4
#define LOG_TYPE_TCPING 5
#define LOG_TYPE_FILEDL 6
#define LOG_TYPE_FILEUL 7
#define LOG_TYPE_RULES 8
#define LOG_TYPE_GPING 9
#define LOG_TYPE_RENDER 10
#define LOG_TYPE_DEBUG 11
#define LOG_TYPE_STUN 12

// Log levels
#define LOG_LEVEL_FATAL   10
#define LOG_LEVEL_ERROR   20
#define LOG_LEVEL_WARNING 30
#define LOG_LEVEL_INFO    40
#define LOG_LEVEL_DEBUG   50
#define LOG_LEVEL_VERBOSE 60

void logInit(bool rpcmode = false);
void logEOF();

void writeLog(int type, std::string content, int level = LOG_LEVEL_VERBOSE);

// 导出结果文件路径（供 main.cpp、renderer.cpp 使用）
extern std::string resultPath;
// 初始化结果文件名（results/<timestamp>.log）
void resultInit();
// 创建目录的跨平台封装
int makeDir(const char *path);

// 提供渲染使用的时间字符串工具
std::string getTime(int type);

// Breadcrumb helpers
bool breadcrumbInitFromEnv();
bool breadcrumbIsEnabled();
void set_breadcrumb(int node_id, const char *stage);
void set_breadcrumb(const struct nodeInfo &node, const char *stage);
void installCrashFilter();
