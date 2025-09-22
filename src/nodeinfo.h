#ifndef NODEINFO_H_INCLUDED
#define NODEINFO_H_INCLUDED

#include <future>
#include <string>

#include "geoip.h"
#include "misc.h"

struct nodeInfo {
  int linkType = -1;
  int id = -1;
  int groupID = -1;
  bool online = false;
  std::string group;
  std::string remarks;
  std::string server;
  int port = 0;
  std::string proxyStr;
  // 记录原始分享链接（如 vless://...），仅在由分享链接解析时填充
  std::string originalUrl;
  unsigned long long rawSpeed[20] = {};
  unsigned long long totalRecvBytes = 0;
  int duration = 0;
  std::string avgSpeed = "N/A";
  std::string maxSpeed = "N/A";
  std::string ulSpeed = "N/A";
  std::string pkLoss = "100.00%";
  int rawPing[6] = {};
  std::string avgPing = "0.00";
  int rawSitePing[10] = {};
  std::string sitePing = "0.00";
  std::string traffic;
  FutureHelper<geoIPInfo> inboundGeoIP;
  FutureHelper<geoIPInfo> outboundGeoIP;
  std::string testFile;
  std::string ulTarget;
  FutureHelper<std::string> natType{"Unknown"};
  unsigned long long completed_seq = 0; // 新增：完成序号（单调递增）
  unsigned long long completed_ms = 0;  // 新增：完成时间（毫秒）
};

#endif // NODEINFO_H_INCLUDED
