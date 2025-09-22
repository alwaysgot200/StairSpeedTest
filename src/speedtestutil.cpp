#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <time.h>
#include <utility>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "ini_reader.h"
#include "logger.h"
#include "misc.h"
#include "printout.h"
#include "rapidjson_extra.h"
#include "speedtestutil.h"
#include "string_hash.h"
#include "webget.h"
#include "yamlcpp_extra.h"

using namespace rapidjson;
using namespace YAML;

// Optional runtime config for parsing parallelism (provided by other TU)
extern int parse_parallel_threshold; // <=0 means use default 512
extern int parse_worker_count;       // <=0 means use default (2*HW or 8)

string_array ss_ciphers = {"rc4-md5",
                           "aes-128-gcm",
                           "aes-192-gcm",
                           "aes-256-gcm",
                           "aes-128-cfb",
                           "aes-192-cfb",
                           "aes-256-cfb",
                           "aes-128-ctr",
                           "aes-192-ctr",
                           "aes-256-ctr",
                           "camellia-128-cfb",
                           "camellia-192-cfb",
                           "camellia-256-cfb",
                           "bf-cfb",
                           "chacha20-ietf-poly1305",
                           "xchacha20-ietf-poly1305",
                           "salsa20",
                           "chacha20",
                           "chacha20-ietf"};
string_array ssr_ciphers = {"none",
                            "table",
                            "rc4",
                            "rc4-md5",
                            "aes-128-cfb",
                            "aes-192-cfb",
                            "aes-256-cfb",
                            "aes-128-ctr",
                            "aes-192-ctr",
                            "aes-256-ctr",
                            "bf-cfb",
                            "camellia-128-cfb",
                            "camellia-192-cfb",
                            "camellia-256-cfb",
                            "cast5-cfb",
                            "des-cfb",
                            "idea-cfb",
                            "rc2-cfb",
                            "seed-cfb",
                            "salsa20",
                            "chacha20",
                            "chacha20-ietf"};

std::map<std::string, std::string> parsedMD5;
std::string modSSMD5 = "f7653207090ce3389115e9c88541afe0";

// remake from speedtestutil
// output the constructed vmess config file data
void explodeVmess(std::string vmess, const std::string &custom_port,
                  nodeInfo &node) {
  // 空串防护
  if (vmess.empty())
    return;

  // 大小写不敏感前缀校验
  if (!regFind(vmess, "(?i)^(vmess|vmess1)://"))
    return;

  if (node.originalUrl.empty())
    node.originalUrl = vmess;

  std::string version, ps, add, port, type, id, aid, net, path, host, tls;
  Document jsondata;
  std::vector<std::string> vArray;
  if (regMatch(vmess, "(?i)vmess://(.*?)@(.*)")) {
    explodeStdVMess(vmess, custom_port, node);
    return;
  } else if (regMatch(vmess,
                      "(?i)vmess://(.*?)\\?(.*)")) // shadowrocket style link
  {
    explodeShadowrocket(vmess, custom_port, node);
    return;
  } else if (regMatch(vmess,
                      "(?i)vmess1://(.*?)\\?(.*)")) // kitsunebi style link
  {
    explodeKitsunebi(vmess, custom_port, node);
    return;
  }
  vmess = urlsafe_base64_decode(regReplace(vmess, "(?i)(vmess|vmess1)://", ""));
  if (regMatch(vmess, "(.*?) = (.*)")) {
    explodeQuan(vmess, custom_port, node);
    return;
  }
  jsondata.Parse(vmess.c_str());
  if (jsondata.HasParseError())
    return;

  version = "1"; // link without version will treat as version 1
  GetMember(jsondata, "v", version); // try to get version

  GetMember(jsondata, "ps", ps);
  GetMember(jsondata, "add", add);
  port = custom_port.size() ? custom_port : GetMember(jsondata, "port");
  if (port == "0")
    return;
  GetMember(jsondata, "type", type);
  GetMember(jsondata, "id", id);
  GetMember(jsondata, "aid", aid);
  GetMember(jsondata, "net", net);
  GetMember(jsondata, "tls", tls);

  GetMember(jsondata, "host", host);
  switch (to_int(version)) {
  case 1:
    if (host.size()) {
      vArray = split(host, ";");
      if (vArray.size() == 2) {
        host = vArray[0];
        path = vArray[1];
      }
    }
    break;
  case 2:
    path = GetMember(jsondata, "path");
    break;
  }

  add = trim(add);
  node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = ps;
  node.server = add;
  node.port = to_int(port, 1);
  try {
    node.proxyStr = vmessConstruct(node.group, ps, add, port, type, id, aid,
                                   net, "auto", path, host, "", tls);
  } catch (const std::exception &e) {
    writeLog(LOG_TYPE_ERROR, std::string("vmessConstruct() exception: ") +
                                 e.what() +
                                 " while building vmess for: " + add);
    node.linkType = -1; // mark invalid, caller will skip this node
    return;
  } catch (...) {
    writeLog(
        LOG_TYPE_ERROR,
        std::string(
            "vmessConstruct() unknown exception while building vmess for: ") +
            add);
    node.linkType = -1;
    return;
  }
}

void explodeVmessConf(std::string content, const std::string &custom_port,
                      bool libev, std::vector<nodeInfo> &nodes) {
  nodeInfo node;
  Document json;
  rapidjson::Value nodejson, settings;
  std::string group, ps, add, port, type, id, aid, net, path, host, edge, tls,
      cipher, subid;
  tribool udp, tfo, scv;
  int configType, index = nodes.size();
  std::map<std::string, std::string> subdata;
  std::map<std::string, std::string>::iterator iter;
  std::string streamset = "streamSettings", tcpset = "tcpSettings",
              wsset = "wsSettings";
  // 空串防护
  if (content.empty())
    return;
  regGetMatch(content, "((?i)streamsettings)", 2, 0, &streamset);
  regGetMatch(content, "((?i)tcpsettings)", 2, 0, &tcpset);
  regGetMatch(content, "((?i)wssettings)", 2, 0, &wsset);

  json.Parse(content.c_str());
  if (json.HasParseError())
    return;
  try {
    if (json.HasMember("outbounds")) // single config
    {
      if (json["outbounds"].Size() > 0 &&
          json["outbounds"][0].HasMember("settings") &&
          json["outbounds"][0]["settings"].HasMember("vnext") &&
          json["outbounds"][0]["settings"]["vnext"].Size() > 0) {
        nodejson = json["outbounds"][0];
        add = GetMember(nodejson["settings"]["vnext"][0], "address");
        port = custom_port.size()
                   ? custom_port
                   : GetMember(nodejson["settings"]["vnext"][0], "port");
        if (port == "0")
          return;
        if (nodejson["settings"]["vnext"][0]["users"].Size()) {
          id = GetMember(nodejson["settings"]["vnext"][0]["users"][0], "id");
          aid = GetMember(nodejson["settings"]["vnext"][0]["users"][0],
                          "alterId");
          cipher = GetMember(nodejson["settings"]["vnext"][0]["users"][0],
                             "security");
        }
        if (nodejson.HasMember(streamset.data())) {
          net = GetMember(nodejson[streamset.data()], "network");
          tls = GetMember(nodejson[streamset.data()], "security");
          if (net == "ws") {
            if (nodejson[streamset.data()].HasMember(wsset.data()))
              settings = nodejson[streamset.data()][wsset.data()];
            else
              settings.RemoveAllMembers();
            path = GetMember(settings, "path");
            if (settings.HasMember("headers")) {
              host = GetMember(settings["headers"], "Host");
              edge = GetMember(settings["headers"], "Edge");
            }
          }
          if (nodejson[streamset.data()].HasMember(tcpset.data()))
            settings = nodejson[streamset.data()][tcpset.data()];
          else
            settings.RemoveAllMembers();
          if (settings.IsObject() && settings.HasMember("header")) {
            type = GetMember(settings["header"], "type");
            if (type == "http") {
              if (settings["header"].HasMember("request")) {
                if (settings["header"]["request"].HasMember("path") &&
                    settings["header"]["request"]["path"].Size())
                  settings["header"]["request"]["path"][0] >> path;
                if (settings["header"]["request"].HasMember("headers")) {
                  host = GetMember(settings["header"]["request"]["headers"],
                                   "Host");
                  edge = GetMember(settings["header"]["request"]["headers"],
                                   "Edge");
                }
              }
            }
          }
        }
        node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
        node.group = V2RAY_DEFAULT_GROUP;
        node.remarks = add + ":" + port;
        node.server = add;
        node.port = to_int(port, 1);
        node.proxyStr =
            vmessConstruct(node.group, node.remarks, add, port, type, id, aid,
                           net, cipher, path, host, edge, tls, udp, tfo, scv);
        nodes.emplace_back(std::move(node));
        node = nodeInfo();
      }
      return;
    }
  } catch (std::exception &e) {
    writeLog(0, "VMessConf parser throws an error. Leaving...",
             LOG_LEVEL_WARNING);
    return;
    // ignore
  }
  // read all subscribe remark as group name
  for (unsigned int i = 0; i < json["subItem"].Size(); i++)
    subdata.insert(std::pair<std::string, std::string>(
        json["subItem"][i]["id"].GetString(),
        json["subItem"][i]["remarks"].GetString()));

  for (unsigned int i = 0; i < json["vmess"].Size(); i++) {
    if (json["vmess"][i]["address"].IsNull() ||
        json["vmess"][i]["port"].IsNull() || json["vmess"][i]["id"].IsNull())
      continue;

    // common info
    json["vmess"][i]["remarks"] >> ps;
    json["vmess"][i]["address"] >> add;
    port =
        custom_port.size() ? custom_port : GetMember(json["vmess"][i], "port");
    if (port == "0")
      continue;
    json["vmess"][i]["subid"] >> subid;

    if (subid.size()) {
      iter = subdata.find(subid);
      if (iter != subdata.end())
        group = iter->second;
    }
    if (ps.empty())
      ps = add + ":" + port;

    scv = GetMember(json["vmess"][i], "allowInsecure");
    json["vmess"][i]["configType"] >> configType;
    switch (configType) {
    case 1: // vmess config
      json["vmess"][i]["headerType"] >> type;
      json["vmess"][i]["id"] >> id;
      json["vmess"][i]["alterId"] >> aid;
      json["vmess"][i]["network"] >> net;
      json["vmess"][i]["path"] >> path;
      json["vmess"][i]["requestHost"] >> host;
      json["vmess"][i]["streamSecurity"] >> tls;
      json["vmess"][i]["security"] >> cipher;
      group = V2RAY_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
      node.proxyStr =
          vmessConstruct(group, ps, add, port, type, id, aid, net, cipher, path,
                         host, "", tls, udp, tfo, scv);
      break;
    case 3: // ss config
      json["vmess"][i]["id"] >> id;
      json["vmess"][i]["security"] >> cipher;
      group = SS_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.proxyStr = ssConstruct(group, ps, add, port, id, cipher, "", "",
                                  libev, udp, tfo, scv);
      break;
    case 4: // socks config
      group = SOCKS_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
      node.proxyStr =
          socksConstruct(group, ps, add, port, "", "", udp, tfo, scv);
      break;
    default:
      continue;
    }

    node.group = group;
    node.remarks = ps;
    node.id = index;
    node.server = add;
    node.port = to_int(port, 1);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
  }
  return;
}

void explodeVless(std::string vless, const std::string &custom_port,
                  nodeInfo &node) {
  std::string version, ps, add, port, type, id, aid, net, path, host, tls;
  Document jsondata;
  std::vector<std::string> vArray;
  // 空串防护
  if (vless.empty())
    return;

  // Ensure scheme is vless:// (大小写不敏感)
  if (!regFind(vless, "(?i)^vless://"))
    return;

  // 记录原始 vless 分享链接
  if (node.originalUrl.empty())
    node.originalUrl = vless;

  if (regMatch(vless, "(?i)vless://(.*?)@(.*)")) {
    explodeStdVLess(vless, custom_port, node);
    return;
  } else if (regMatch(vless,
                      "(?i)vless://(.*?)\\?(.*)")) // shadowrocket style link
  {
    explodeShadowrocket(vless, custom_port, node);
    return;
  }
  vless = urlsafe_base64_decode(regReplace(vless, "(?i)(vless)://", ""));
  if (regMatch(vless, "(.*?) = (.*)")) {
    explodeQuan(vless, custom_port, node);
    return;
  }
  jsondata.Parse(vless.c_str());
  if (jsondata.HasParseError())
    return;

  version = "1"; // link without version will treat as version 1
  GetMember(jsondata, "v", version); // try to get version

  GetMember(jsondata, "ps", ps);
  GetMember(jsondata, "add", add);
  port = custom_port.size() ? custom_port : GetMember(jsondata, "port");
  if (port == "0")
    return;
  GetMember(jsondata, "type", type);
  GetMember(jsondata, "id", id);
  GetMember(jsondata, "aid", aid);
  GetMember(jsondata, "net", net);
  GetMember(jsondata, "tls", tls);

  GetMember(jsondata, "host", host);
  switch (to_int(version)) {
  case 1:
    if (host.size()) {
      vArray = split(host, ";");
      if (vArray.size() == 2) {
        host = vArray[0];
        path = vArray[1];
      }
    }
    break;
  case 2:
    path = GetMember(jsondata, "path");
    break;
  }

  add = trim(add);
  node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = ps;
  node.server = add;
  node.port = to_int(port, 1);
  node.proxyStr = vlessConstruct(add, port, type, id, net, path, host, "", tls,
                                 tribool(), "", "", "", "", "", "");
}

void explodeVlessConf(std::string content, const std::string &custom_port,
                      bool libev, std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (content.empty())
    return;

  nodeInfo node;
  Document json;
  rapidjson::Value nodejson, settings;
  std::string group, ps, add, port, type, id, aid, net, path, host, edge, tls,
      cipher, subid;
  tribool udp, tfo, scv;
  int configType, index = nodes.size();
  std::map<std::string, std::string> subdata;
  std::map<std::string, std::string>::iterator iter;
  std::string streamset = "streamSettings", tcpset = "tcpSettings",
              wsset = "wsSettings";
  regGetMatch(content, "((?i)streamsettings)", 2, 0, &streamset);
  regGetMatch(content, "((?i)tcpsettings)", 2, 0, &tcpset);
  regGetMatch(content, "((?i)wssettings)", 2, 0, &wsset);

  // 可选字段：alpn/fp/flow/pbk/sid
  std::string alpn, fp, flow, pbk, sid;

  json.Parse(content.c_str());
  if (json.HasParseError())
    return;

  try {
    if (json.HasMember("outbounds")) { // single config
      if (json["outbounds"].Size() > 0 &&
          json["outbounds"][0].HasMember("settings") &&
          json["outbounds"][0]["settings"].HasMember("vnext") &&
          json["outbounds"][0]["settings"]["vnext"].Size() > 0) {
        nodejson = json["outbounds"][0];
        add = GetMember(nodejson["settings"]["vnext"][0], "address");
        port = custom_port.size()
                   ? custom_port
                   : GetMember(nodejson["settings"]["vnext"][0], "port");
        if (port == "0")
          return;

        // users[0]
        if (nodejson["settings"]["vnext"][0]["users"].Size()) {
          id = GetMember(nodejson["settings"]["vnext"][0]["users"][0], "id");
          aid = GetMember(nodejson["settings"]["vnext"][0]["users"][0],
                          "alterId");
          cipher = GetMember(nodejson["settings"]["vnext"][0]["users"][0],
                             "security");
          // flow（vless 用户级字段）
          flow =
              GetMember(nodejson["settings"]["vnext"][0]["users"][0], "flow");
        }

        std::string realServerName, tlsServerName;

        if (nodejson.HasMember(streamset.data())) {
          net = GetMember(nodejson[streamset.data()], "network");
          tls = GetMember(nodejson[streamset.data()], "security");

          // wsSettings
          if (net == "ws") {
            if (nodejson[streamset.data()].HasMember(wsset.data()))
              settings = nodejson[streamset.data()][wsset.data()];
            else
              settings.RemoveAllMembers();
            path = GetMember(settings, "path");
            if (settings.HasMember("headers")) {
              host = GetMember(settings["headers"], "Host");
              edge = GetMember(settings["headers"], "Edge");
            }
          }

          // tcpSettings（http 伪装）
          if (nodejson[streamset.data()].HasMember(tcpset.data()))
            settings = nodejson[streamset.data()][tcpset.data()];
          else
            settings.RemoveAllMembers();
          if (settings.IsObject() && settings.HasMember("header")) {
            type = GetMember(settings["header"], "type");
            if (type == "http") {
              if (settings["header"].HasMember("request")) {
                if (settings["header"]["request"].HasMember("path") &&
                    settings["header"]["request"]["path"].Size())
                  settings["header"]["request"]["path"][0] >> path;
                if (settings["header"]["request"].HasMember("headers")) {
                  host = GetMember(settings["header"]["request"]["headers"],
                                   "Host");
                  edge = GetMember(settings["header"]["request"]["headers"],
                                   "Edge");
                }
              }
            }
          }

          // TLS/Reality 额外字段解析
          if (tls == "tls" &&
              nodejson[streamset.data()].HasMember("tlsSettings") &&
              nodejson[streamset.data()]["tlsSettings"].IsObject()) {
            const auto &tlsSettings = nodejson[streamset.data()]["tlsSettings"];
            tlsServerName = GetMember(tlsSettings, "serverName");

            // alpn: 数组或字符串
            if (tlsSettings.HasMember("alpn")) {
              const auto &a = tlsSettings["alpn"];
              if (a.IsArray()) {
                alpn.clear();
                for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
                  if (a[i].IsString()) {
                    if (!alpn.empty())
                      alpn += ",";
                    alpn += a[i].GetString();
                  }
                }
              } else if (a.IsString()) {
                alpn = a.GetString();
              }
            }
            // fingerprint
            fp = GetMember(tlsSettings, "fingerprint");
          } else if (tls == "reality" &&
                     nodejson[streamset.data()].HasMember("realitySettings") &&
                     nodejson[streamset.data()]["realitySettings"].IsObject()) {
            const auto &realSettings =
                nodejson[streamset.data()]["realitySettings"];
            realServerName = GetMember(realSettings, "serverName");
            pbk = GetMember(realSettings, "publicKey");
            sid = GetMember(realSettings, "shortId");

            // alpn: 数组或字符串
            if (realSettings.HasMember("alpn")) {
              const auto &a = realSettings["alpn"];
              if (a.IsArray()) {
                alpn.clear();
                for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
                  if (a[i].IsString()) {
                    if (!alpn.empty())
                      alpn += ",";
                    alpn += a[i].GetString();
                  }
                }
              } else if (a.IsString()) {
                alpn = a.GetString();
              }
            }
            // fingerprint
            fp = GetMember(realSettings, "fingerprint");
          }

          // 统一 SNI 优先级：Reality.serverName > TLS.serverName > Edge > Host
          {
            std::string finalSNI;
            if (!realServerName.empty())
              finalSNI = realServerName;
            if (finalSNI.empty() && !tlsServerName.empty())
              finalSNI = tlsServerName;
            if (finalSNI.empty() && !edge.empty())
              finalSNI = edge;
            if (finalSNI.empty() && !host.empty())
              finalSNI = host;
            if (!finalSNI.empty())
              edge = finalSNI;
          }
        }

        node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
        node.group = V2RAY_DEFAULT_GROUP;
        node.remarks = add + ":" + port;
        node.server = add;
        node.port = to_int(port, 1);
        node.proxyStr =
            vlessConstruct(add, port, type, id, net, path, host, edge, tls, scv,
                           alpn, fp, flow, pbk, sid, "");
        nodes.emplace_back(std::move(node));
        node = nodeInfo();
        // 重置可选字段，避免污染后续节点
        alpn.clear();
        fp.clear();
        flow.clear();
        pbk.clear();
        sid.clear();
        edge.clear();
      }
      return;
    }
  } catch (std::exception &e) {
    writeLog(0, "VlessConf parser throws an error. Leaving...",
             LOG_LEVEL_WARNING);
    return;
  }

  // read all subscribe remark as group name
  for (unsigned int i = 0; i < json["subItem"].Size(); i++)
    subdata.insert(std::pair<std::string, std::string>(
        json["subItem"][i]["id"].GetString(),
        json["subItem"][i]["remarks"].GetString()));

  for (unsigned int i = 0; i < json["vless"].Size(); i++) {
    if (json["vless"][i]["address"].IsNull() ||
        json["vless"][i]["port"].IsNull() || json["vless"][i]["id"].IsNull())
      continue;

    // common info
    json["vless"][i]["remarks"] >> ps;
    json["vless"][i]["address"] >> add;
    port =
        custom_port.size() ? custom_port : GetMember(json["vless"][i], "port");
    if (port == "0")
      continue;
    json["vless"][i]["subid"] >> subid;

    // 重置可选字段
    alpn.clear();
    fp.clear();
    flow.clear();
    pbk.clear();
    sid.clear();
    edge.clear();

    if (subid.size()) {
      iter = subdata.find(subid);
      if (iter != subdata.end())
        group = iter->second;
    }
    if (ps.empty())
      ps = add + ":" + port;

    scv = GetMember(json["vless"][i], "allowInsecure");
    json["vless"][i]["configType"] >> configType;
    switch (configType) {
    case 1: { // vless config
      json["vless"][i]["headerType"] >> type;
      json["vless"][i]["id"] >> id;
      json["vless"][i]["alterId"] >> aid;
      json["vless"][i]["network"] >> net;
      json["vless"][i]["path"] >> path;
      json["vless"][i]["requestHost"] >> host;
      json["vless"][i]["streamSecurity"] >> tls;
      json["vless"][i]["security"] >> cipher;

      // 顶层 flow/alpn/fp/pbk/sid（如果订阅直接给出）
      flow = GetMember(json["vless"][i], "flow");
      if (json["vless"][i].HasMember("alpn")) {
        const auto &a = json["vless"][i]["alpn"];
        if (a.IsArray()) {
          alpn.clear();
          for (rapidjson::SizeType j = 0; j < a.Size(); ++j) {
            if (a[j].IsString()) {
              if (!alpn.empty())
                alpn += ",";
              alpn += a[j].GetString();
            }
          }
        } else if (a.IsString()) {
          alpn = a.GetString();
        }
      }
      // fp/fingerprint 兼容
      fp = GetMember(json["vless"][i], "fp");
      if (fp.empty())
        fp = GetMember(json["vless"][i], "fingerprint");
      // pbk/publicKey 兼容
      pbk = GetMember(json["vless"][i], "pbk");
      if (pbk.empty())
        pbk = GetMember(json["vless"][i], "publicKey");
      // sid/shortId 兼容
      sid = GetMember(json["vless"][i], "sid");
      if (sid.empty())
        sid = GetMember(json["vless"][i], "shortId");

      // 若条目包含 realitySettings / tlsSettings，优先使用其字段，并抽取
      // serverName
      std::string realServerName, tlsServerName;
      if (json["vless"][i].HasMember("realitySettings") &&
          json["vless"][i]["realitySettings"].IsObject()) {
        const auto &realSettings = json["vless"][i]["realitySettings"];
        realServerName = GetMember(realSettings, "serverName");
        std::string pbk2 = GetMember(realSettings, "publicKey");
        if (!pbk2.empty())
          pbk = pbk2;
        std::string sid2 = GetMember(realSettings, "shortId");
        if (!sid2.empty())
          sid = sid2;
        if (realSettings.HasMember("alpn")) {
          const auto &a = realSettings["alpn"];
          if (a.IsArray()) {
            alpn.clear();
            for (rapidjson::SizeType j = 0; j < a.Size(); ++j) {
              if (a[j].IsString()) {
                if (!alpn.empty())
                  alpn += ",";
                alpn += a[j].GetString();
              }
            }
          } else if (a.IsString()) {
            alpn = a.GetString();
          }
        }
        std::string fp2 = GetMember(realSettings, "fingerprint");
        if (!fp2.empty())
          fp = fp2;
      } else if (json["vless"][i].HasMember("tlsSettings") &&
                 json["vless"][i]["tlsSettings"].IsObject()) {
        const auto &tlsSettings = json["vless"][i]["tlsSettings"];
        tlsServerName = GetMember(tlsSettings, "serverName");
        if (tlsSettings.HasMember("alpn")) {
          const auto &a = tlsSettings["alpn"];
          if (a.IsArray()) {
            alpn.clear();
            for (rapidjson::SizeType j = 0; j < a.Size(); ++j) {
              if (a[j].IsString()) {
                if (!alpn.empty())
                  alpn += ",";
                alpn += a[j].GetString();
              }
            }
          } else if (a.IsString()) {
            alpn = a.GetString();
          }
        }
        std::string fp2 = GetMember(tlsSettings, "fingerprint");
        if (!fp2.empty())
          fp = fp2;
      }

      // 统一 SNI 优先级：Reality.serverName > TLS.serverName > Edge > Host
      {
        std::string finalSNI;
        if (!realServerName.empty())
          finalSNI = realServerName;
        if (finalSNI.empty() && !tlsServerName.empty())
          finalSNI = tlsServerName;
        if (finalSNI.empty() && !edge.empty())
          finalSNI = edge; // 来自 HTTP/WS headers 的 Edge
        if (finalSNI.empty() && !host.empty())
          finalSNI = host; // 回退 Host
        if (!finalSNI.empty())
          edge = finalSNI; // edge 作为 SNI 传入构造器
      }

      group = V2RAY_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
      node.proxyStr = vlessConstruct(add, port, type, id, net, path, host, edge,
                                     tls, scv, alpn, fp, flow, pbk, sid, "");
      break;
    }
    case 3: // ss config
      json["vless"][i]["id"] >> id;
      json["vless"][i]["security"] >> cipher;
      group = SS_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.proxyStr = ssConstruct(group, ps, add, port, id, cipher, "", "",
                                  libev, udp, tfo, scv);
      break;
    case 4: // socks config
      group = SOCKS_DEFAULT_GROUP;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
      node.proxyStr =
          socksConstruct(group, ps, add, port, "", "", udp, tfo, scv);
      break;
    default:
      continue;
    }

    node.group = group;
    node.remarks = ps;
    node.id = index;
    node.server = add;
    node.port = to_int(port, 1);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
  }
  return;
}
void explodeSS(std::string ss, bool libev, const std::string &custom_port,
               nodeInfo &node) {
  std::string ps, password, method, server, port, plugins, plugin, pluginopts,
      addition, group = SS_DEFAULT_GROUP, secret;
  // std::vector<std::string> args, secret;
  if (node.originalUrl.empty())
    node.originalUrl = ss;
  ss = replace_all_distinct(ss.substr(5), "/?", "?");
  if (strFind(ss, "#")) {
    ps = UrlDecode(ss.substr(ss.find("#") + 1));
    ss.erase(ss.find("#"));
  }

  if (strFind(ss, "?")) {
    addition = ss.substr(ss.find("?") + 1);
    plugins = UrlDecode(getUrlArg(addition, "plugin"));
    plugin = plugins.substr(0, plugins.find(";"));
    pluginopts = plugins.substr(plugins.find(";") + 1);
    if (getUrlArg(addition, "group").size())
      group = urlsafe_base64_decode(getUrlArg(addition, "group"));
    ss.erase(ss.find("?"));
  }
  if (strFind(ss, "@")) {
    if (regGetMatch(ss, "(.*?)@(.*):(.*)", 4, 0, &secret, &server, &port))
      return;
    if (regGetMatch(urlsafe_base64_decode(secret), "(.*?):(.*)", 3, 0, &method,
                    &password))
      return;
  } else {
    if (regGetMatch(urlsafe_base64_decode(ss), "(.*?):(.*)@(.*):(.*)", 5, 0,
                    &method, &password, &server, &port))
      return;
  }
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;
  if (ps.empty())
    ps = server + ":" + port;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
  node.group = group;
  node.remarks = ps;
  node.server = server;
  node.port = to_int(port, 1);
  node.proxyStr = ssConstruct(group, ps, server, port, password, method, plugin,
                              pluginopts, libev);
}

void explodeSSD(std::string link, bool libev, const std::string &custom_port,
                std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (link.empty())
    return;

  Document jsondata;
  nodeInfo node;
  unsigned int index = nodes.size(), listType = 0, listCount = 0;
  std::string group, port, method, password, server, remarks;
  std::string plugin, pluginopts;
  std::map<unsigned int, std::string> node_map;

  link = urlsafe_base64_decode(link.substr(6));
  jsondata.Parse(link.c_str());
  if (jsondata.HasParseError())
    return;
  if (!jsondata.HasMember("servers"))
    return;
  GetMember(jsondata, "airport", group);

  if (jsondata["servers"].IsArray()) {
    listType = 0;
    listCount = jsondata["servers"].Size();
  } else if (jsondata["servers"].IsObject()) {
    listType = 1;
    listCount = jsondata["servers"].MemberCount();
    unsigned int node_index = 0;
    for (rapidjson::Value::MemberIterator iter =
             jsondata["servers"].MemberBegin();
         iter != jsondata["servers"].MemberEnd(); iter++) {
      node_map.emplace(node_index, iter->name.GetString());
      node_index++;
    }
  } else
    return;

  rapidjson::Value singlenode;
  for (unsigned int i = 0; i < listCount; i++) {
    // get default info
    GetMember(jsondata, "port", port);
    GetMember(jsondata, "encryption", method);
    GetMember(jsondata, "password", password);
    GetMember(jsondata, "plugin", plugin);
    GetMember(jsondata, "plugin_options", pluginopts);

    // get server-specific info
    switch (listType) {
    case 0:
      singlenode = jsondata["servers"][i];
      break;
    case 1:
      singlenode = jsondata["servers"].FindMember(node_map[i].data())->value;
      break;
    default:
      continue;
    }
    singlenode["server"] >> server;
    GetMember(singlenode, "remarks", remarks);
    GetMember(singlenode, "port", port);
    GetMember(singlenode, "encryption", method);
    GetMember(singlenode, "password", password);
    GetMember(singlenode, "plugin", plugin);
    GetMember(singlenode, "plugin_options", pluginopts);

    if (custom_port.size())
      port = custom_port;
    if (port == "0")
      continue;

    node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
    node.group = group;
    node.remarks = remarks;
    node.server = server;
    node.port = to_int(port, 1);
    node.proxyStr = ssConstruct(group, remarks, server, port, password, method,
                                plugin, pluginopts, libev);

    // 为 ssd:// 订阅的逐条节点构造原始 ss 分享链接
    {
      std::string host = server;
      if (isIPv6(host)) host = "[" + host + "]";
      std::string secret = urlsafe_base64_encode(method + ":" + password);
      std::string orig = "ss://" + secret + "@" + host + ":" + port;

      std::string query;
      // plugin 参数（形如 plugin=pluginName;opts，需要整体 UrlEncode）
      if (!plugin.empty() || !pluginopts.empty()) {
        std::string plugin_full = plugin;
        if (!pluginopts.empty()) {
          if (!plugin_full.empty()) plugin_full += ";";
          plugin_full += pluginopts;
        }
        query += "plugin=" + UrlEncode(plugin_full);
      }
      // group 参数（urlsafe_base64 编码）
      if (!group.empty()) {
        if (!query.empty()) query += "&";
        query += "group=" + urlsafe_base64_encode(group);
      }
      if (!query.empty()) orig += "?" + query;
      // 备注放在 # 后（UrlEncode）
      if (!remarks.empty()) orig += "#" + UrlEncode(remarks);

      node.originalUrl = orig;
    }

    node.id = index;
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
  return;
}

void explodeSSAndroid(std::string ss, bool libev,
                      const std::string &custom_port,
                      std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (ss.empty())
    return;

  std::string ps, password, method, server, port, group = SS_DEFAULT_GROUP;
  std::string plugin, pluginopts;

  Document json;
  nodeInfo node;
  int index = nodes.size();
  // first add some extra data before parsing
  ss = "{\"nodes\":" + ss + "}";
  json.Parse(ss.c_str());
  if (json.HasParseError())
    return;

  for (unsigned int i = 0; i < json["nodes"].Size(); i++) {
    server = GetMember(json["nodes"][i], "server");
    if (server.empty())
      continue;
    ps = GetMember(json["nodes"][i], "remarks");
    port = custom_port.size() ? custom_port
                              : GetMember(json["nodes"][i], "server_port");
    if (port == "0")
      continue;
    if (ps.empty())
      ps = server + ":" + port;
    password = GetMember(json["nodes"][i], "password");
    method = GetMember(json["nodes"][i], "method");
    plugin = GetMember(json["nodes"][i], "plugin");
    pluginopts = GetMember(json["nodes"][i], "plugin_opts");

    node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
    node.id = index;
    node.group = group;
    node.remarks = ps;
    node.server = server;
    node.port = to_int(port, 1);
    node.proxyStr = ssConstruct(group, ps, server, port, password, method,
                                plugin, pluginopts, libev);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
}

void explodeSSConf(std::string content, const std::string &custom_port,
                   bool libev, std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (content.empty())
    return;

  nodeInfo node;
  Document json;
  std::string ps, password, method, server, port, plugin, pluginopts,
      group = SS_DEFAULT_GROUP;
  int index = nodes.size();

  json.Parse(content.c_str());
  if (json.HasParseError())
    return;
  const char *section = json.HasMember("version") &&
                                json.HasMember("remarks") &&
                                json.HasMember("servers")
                            ? "servers"
                            : "configs";
  if (!json.HasMember(section))
    return;
  GetMember(json, "remarks", group);

  for (unsigned int i = 0; i < json[section].Size(); i++) {
    ps = GetMember(json[section][i], "remarks");
    port = custom_port.size() ? custom_port
                              : GetMember(json[section][i], "server_port");
    if (port == "0")
      continue;
    if (ps.empty())
      ps = server + ":" + port;

    password = GetMember(json[section][i], "password");
    method = GetMember(json[section][i], "method");
    server = GetMember(json[section][i], "server");
    plugin = GetMember(json[section][i], "plugin");
    pluginopts = GetMember(json[section][i], "plugin_opts");

    node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
    node.group = group;
    node.remarks = ps;
    node.id = index;
    node.server = server;
    node.port = to_int(port, 1);
    node.proxyStr = ssConstruct(group, ps, server, port, password, method,
                                plugin, pluginopts, libev);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
  return;
}

void explodeSSR(std::string ssr, bool ss_libev, bool ssr_libev,
                const std::string &custom_port, nodeInfo &node) {
  std::string strobfs;
  std::string remarks, group, server, port, method, password, protocol,
      protoparam, obfs, obfsparam, remarks_base64;
  if (node.originalUrl.empty())
    node.originalUrl = ssr;
  ssr = replace_all_distinct(ssr.substr(6), "\r", "");
  ssr = urlsafe_base64_decode(ssr);
  if (strFind(ssr, "/?")) {
    strobfs = ssr.substr(ssr.find("/?") + 2);
    ssr = ssr.substr(0, ssr.find("/?"));
    group = urlsafe_base64_decode(getUrlArg(strobfs, "group"));
    remarks = urlsafe_base64_decode(getUrlArg(strobfs, "remarks"));
    remarks_base64 = urlsafe_base64_reverse(getUrlArg(strobfs, "remarks"));
    obfsparam = regReplace(
        urlsafe_base64_decode(getUrlArg(strobfs, "obfsparam")), "\\s", "");
    protoparam = regReplace(
        urlsafe_base64_decode(getUrlArg(strobfs, "protoparam")), "\\s", "");
  }

  if (regGetMatch(ssr, "(.*):(.*?):(.*?):(.*?):(.*?):(.*)", 7, 0, &server,
                  &port, &protocol, &method, &obfs, &password))
    return;
  password = urlsafe_base64_decode(password);
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;

  if (group.empty())
    group = SSR_DEFAULT_GROUP;
  if (remarks.empty()) {
    remarks = server + ":" + port;
    remarks_base64 = base64_encode(remarks);
  }

  node.group = group;
  node.remarks = remarks;
  node.server = server;
  node.port = to_int(port, 1);
  if (find(ss_ciphers.begin(), ss_ciphers.end(), method) != ss_ciphers.end() &&
      (obfs.empty() || obfs == "plain") &&
      (protocol.empty() || protocol == "origin")) {
    node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
    node.proxyStr = ssConstruct(group, remarks, server, port, password, method,
                                "", "", ss_libev);
  } else {
    node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
    node.proxyStr =
        ssrConstruct(group, remarks, remarks_base64, server, port, protocol,
                     method, obfs, password, obfsparam, protoparam, ssr_libev);
  }
}

void explodeSSRConf(std::string content, const std::string &custom_port,
                    bool ss_libev, bool ssr_libev,
                    std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (content.empty())
    return;

  nodeInfo node;
  Document json;
  std::string remarks, remarks_base64, group, server, port, method, password,
      protocol, protoparam, obfs, obfsparam, plugin, pluginopts;
  int index = nodes.size();

  json.Parse(content.c_str());
  if (json.HasParseError())
    return;

  if (json.HasMember("local_port") &&
      json.HasMember("local_address")) // single libev config
  {
    server = GetMember(json, "server");
    port = GetMember(json, "server_port");
    node.remarks = server + ":" + port;
    node.server = server;
    node.port = to_int(port, 1);
    method = GetMember(json, "method");
    obfs = GetMember(json, "obfs");
    protocol = GetMember(json, "protocol");
    if (find(ss_ciphers.begin(), ss_ciphers.end(), method) !=
            ss_ciphers.end() &&
        (obfs.empty() || obfs == "plain") &&
        (protocol.empty() || protocol == "origin")) {
      plugin = GetMember(json, "plugin");
      pluginopts = GetMember(json, "plugin_opts");
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.group = SS_DEFAULT_GROUP;
      node.proxyStr =
          ssConstruct(node.group, node.remarks, server, port, password, method,
                      plugin, pluginopts, ss_libev);
    } else {
      protoparam = GetMember(json, "protocol_param");
      obfsparam = GetMember(json, "obfs_param");
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
      node.group = SSR_DEFAULT_GROUP;
      node.proxyStr = ssrConstruct(
          node.group, node.remarks, base64_encode(node.remarks), server, port,
          protocol, method, obfs, password, obfsparam, protoparam, ssr_libev);
    }
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    return;
  }

  for (unsigned int i = 0; i < json["configs"].Size(); i++) {
    group = GetMember(json["configs"][i], "group");
    if (group.empty())
      group = SSR_DEFAULT_GROUP;
    remarks = GetMember(json["configs"][i], "remarks");
    server = GetMember(json["configs"][i], "server");
    port = custom_port.size() ? custom_port
                              : GetMember(json["configs"][i], "server_port");
    if (port == "0")
      continue;
    if (remarks.empty())
      remarks = server + ":" + port;

    remarks_base64 =
        GetMember(json["configs"][i],
                  "remarks_base64"); // electron-ssr does not contain this field
    password = GetMember(json["configs"][i], "password");
    method = GetMember(json["configs"][i], "method");

    protocol = GetMember(json["configs"][i], "protocol");
    protoparam = GetMember(json["configs"][i], "protocolparam");
    obfs = GetMember(json["configs"][i], "obfs");
    obfsparam = GetMember(json["configs"][i], "obfsparam");

    node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
    node.group = group;
    node.remarks = remarks;
    node.id = index;
    node.server = server;
    node.port = to_int(port, 1);
    node.proxyStr =
        ssrConstruct(group, remarks, remarks_base64, server, port, protocol,
                     method, obfs, password, obfsparam, protoparam, ssr_libev);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
  return;
}

void explodeSocks(std::string link, const std::string &custom_port,
                  nodeInfo &node) {
  std::string group, remarks, server, port, username, password;
  if (node.originalUrl.empty())
    node.originalUrl = link;
  if (strFind(link, "socks://")) // v2rayn socks link
  {
    std::vector<std::string> arguments;
    if (strFind(link, "#")) {
      remarks = UrlDecode(link.substr(link.find("#") + 1));
      link.erase(link.find("#"));
    }
    link = urlsafe_base64_decode(link.substr(8));
    arguments = split(link, ":");
    if (arguments.size() < 2)
      return;
    server = arguments[0];
    port = arguments[1];
  } else if (strFind(link, "https://t.me/socks") ||
             strFind(link, "tg://socks")) // telegram style socks link
  {
    server = getUrlArg(link, "server");
    port = getUrlArg(link, "port");
    username = UrlDecode(getUrlArg(link, "user"));
    password = UrlDecode(getUrlArg(link, "pass"));
    remarks = UrlDecode(getUrlArg(link, "remarks"));
    group = UrlDecode(getUrlArg(link, "group"));
  }
  if (group.empty())
    group = SOCKS_DEFAULT_GROUP;
  if (remarks.empty())
    remarks = server + ":" + port;
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
  node.group = group;
  node.remarks = remarks;
  node.server = server;
  node.port = to_int(port, 1);
  node.proxyStr =
      socksConstruct(group, remarks, server, port, username, password);
}

void explodeHTTP(const std::string &link, const std::string &custom_port,
                 nodeInfo &node) {
  std::string group, remarks, server, port, username, password;
  if (node.originalUrl.empty())
    node.originalUrl = link; // tg://http / https://t.me/http
  server = getUrlArg(link, "server");
  port = getUrlArg(link, "port");
  username = UrlDecode(getUrlArg(link, "user"));
  password = UrlDecode(getUrlArg(link, "pass"));
  remarks = UrlDecode(getUrlArg(link, "remarks"));
  group = UrlDecode(getUrlArg(link, "group"));

  if (group.empty())
    group = HTTP_DEFAULT_GROUP;
  if (remarks.empty())
    remarks = server + ":" + port;
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
  node.group = group;
  node.remarks = remarks;
  node.server = server;
  node.port = to_int(port, 1);
  node.proxyStr = httpConstruct(group, remarks, server, port, username,
                                password, strFind(link, "/https"));
}

void explodeHTTPSub(std::string link, const std::string &custom_port,
                    nodeInfo &node) {
  std::string group, remarks, server, port, username, password;
  std::string addition;
  bool tls = strFind(link, "https://");
  string_size pos = link.find("?");
  if (pos != link.npos) {
    addition = link.substr(pos + 1);
    link.erase(pos);
    remarks = UrlDecode(getUrlArg(addition, "remarks"));
    group = UrlDecode(getUrlArg(addition, "group"));
  }
  link.erase(0, link.find("://") + 3);
  link = urlsafe_base64_decode(link);
  if (strFind(link, "@")) {
    if (regGetMatch(link, "(.*?):(.*?)@(.*):(.*)", 5, 0, &username, &password,
                    &server, &port))
      return;
  } else {
    if (regGetMatch(link, "(.*):(.*)", 3, 0, &server, &port))
      return;
  }

  if (group.empty())
    group = HTTP_DEFAULT_GROUP;
  if (remarks.empty())
    remarks = server + ":" + port;
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
  node.group = group;
  node.remarks = remarks;
  node.server = server;
  node.port = to_int(port, 1);
  node.proxyStr =
      httpConstruct(group, remarks, server, port, username, password, tls);
}

void explodeTrojan(std::string trojan, const std::string &custom_port,
                   nodeInfo &node) {
  std::string server, port, psk, addition, group, remark, host;
  tribool tfo, scv;
  if (node.originalUrl.empty())
    node.originalUrl = trojan;
  trojan.erase(0, 9);
  string_size pos = trojan.rfind("#");

  if (pos != trojan.npos) {
    remark = UrlDecode(trojan.substr(pos + 1));
    trojan.erase(pos);
  }
  pos = trojan.find("?");
  if (pos != trojan.npos) {
    addition = trojan.substr(pos + 1);
    trojan.erase(pos);
  }

  if (regGetMatch(trojan, "(.*?)@(.*):(.*)", 4, 0, &psk, &server, &port))
    return;
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;

  host = getUrlArg(addition, "peer");
  tfo = getUrlArg(addition, "tfo");
  scv = getUrlArg(addition, "allowInsecure");
  group = UrlDecode(getUrlArg(addition, "group"));

  if (remark.empty())
    remark = server + ":" + port;
  if (host.empty() && !isIPv4(server) && !isIPv6(server))
    host = server;
  if (group.empty())
    group = TROJAN_DEFAULT_GROUP;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
  node.group = group;
  node.remarks = remark;
  node.server = server;
  node.port = to_int(port, 1);
  node.proxyStr = trojanConstruct(group, remark, server, port, psk, host, true,
                                  tribool(), tfo, scv);
}

void explodeQuan(const std::string &quan, const std::string &custom_port,
                 nodeInfo &node) {
  std::string strTemp, itemName, itemVal;
  std::string group = V2RAY_DEFAULT_GROUP, ps, add, port, cipher, type = "none",
              id, aid = "0", net = "tcp", path, host, edge, tls;
  string_array configs, vArray, headers;
  strTemp = regReplace(quan, "(.*?) = (.*)", "$1,$2");
  configs = split(strTemp, ",");

  if (configs[1] == "vmess") {
    if (configs.size() < 6)
      return;
    ps = trim(configs[0]);
    add = trim(configs[2]);
    port = custom_port.size() ? custom_port : trim(configs[3]);
    if (port == "0")
      return;
    cipher = trim(configs[4]);
    id = trim(replace_all_distinct(configs[5], "\"", ""));

    // read link
    for (unsigned int i = 6; i < configs.size(); i++) {
      vArray = split(configs[i], "=");
      if (vArray.size() < 2)
        continue;
      itemName = trim(vArray[0]);
      itemVal = trim(vArray[1]);
      switch (hash_(itemName)) {
      case "group"_hash:
        group = itemVal;
        break;
      case "over-tls"_hash:
        tls = itemVal == "true" ? "tls" : "";
        break;
      case "tls-host"_hash:
        host = itemVal;
        break;
      case "obfs-path"_hash:
        path = replace_all_distinct(itemVal, "\"", "");
        break;
      case "obfs-header"_hash:
        headers =
            split(replace_all_distinct(replace_all_distinct(itemVal, "\"", ""),
                                       "[Rr][Nn]", "|"),
                  "|");
        for (std::string &x : headers) {
          if (regFind(x, "(?i)Host: "))
            host = x.substr(6);
          else if (regFind(x, "(?i)Edge: "))
            edge = x.substr(6);
        }
        break;
      case "obfs"_hash:
        if (itemVal == "ws")
          net = "ws";
        break;
      default:
        continue;
      }
    }
    if (path.empty())
      path = "/";

    node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
    node.group = group;
    node.remarks = ps;
    node.server = add;
    node.port = to_int(port, 1);
    node.proxyStr = vmessConstruct(group, ps, add, port, type, id, aid, net,
                                   cipher, path, host, edge, tls);
  }
}

void explodeNetch(std::string netch, bool ss_libev, bool ssr_libev,
                  const std::string &custom_port, nodeInfo &node) {
  // 空串防护
  if (netch.empty())
    return;

  Document json;
  std::string type, group, remark, address, port, username, password, method,
      plugin, pluginopts, protocol, protoparam, obfs, obfsparam, id, aid,
      transprot, faketype, host, edge, path, tls;
  tribool udp, tfo, scv;

  // 支持两种输入：
  // 1) 标准链接形式：Netch://<base64>
  // 2) 仅含 base64 负载：<base64>
  if (startsWith(netch, "Netch://"))
    netch = urlsafe_base64_decode(netch.substr(8));
  else
    netch = urlsafe_base64_decode(netch);

  json.Parse(netch.c_str());
  if (json.HasParseError())
    return;
  type = GetMember(json, "Type");
  group = GetMember(json, "Group");
  remark = GetMember(json, "Remark");
  address = GetMember(json, "Hostname");
  udp = GetMember(json, "EnableUDP");
  tfo = GetMember(json, "EnableTFO");
  scv = GetMember(json, "AllowInsecure");
  port = custom_port.size() ? custom_port : GetMember(json, "Port");
  if (port == "0")
    return;
  method = GetMember(json, "EncryptMethod");
  password = GetMember(json, "Password");
  if (remark.empty())
    remark = address + ":" + port;
  switch (hash_(type)) {
  case "SS"_hash:
    plugin = GetMember(json, "Plugin");
    pluginopts = GetMember(json, "PluginOption");
    if (group.empty())
      group = SS_DEFAULT_GROUP;
    node.group = group;
    node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
    node.proxyStr = ssConstruct(group, remark, address, port, password, method,
                                plugin, pluginopts, ss_libev, udp, tfo, scv);
    break;
  case "SSR"_hash:
    protocol = GetMember(json, "Protocol");
    obfs = GetMember(json, "OBFS");
    if (find(ss_ciphers.begin(), ss_ciphers.end(), method) !=
            ss_ciphers.end() &&
        (obfs.empty() || obfs == "plain") &&
        (protocol.empty() || protocol == "origin")) {
      plugin = GetMember(json, "Plugin");
      pluginopts = GetMember(json, "PluginOption");
      if (group.empty())
        group = SS_DEFAULT_GROUP;
      node.group = group;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.proxyStr =
          ssConstruct(group, remark, address, port, password, method, plugin,
                      pluginopts, ss_libev, udp, tfo, scv);
    } else {
      protoparam = GetMember(json, "ProtocolParam");
      obfsparam = GetMember(json, "OBFSParam");
      if (group.empty())
        group = SSR_DEFAULT_GROUP;
      node.group = group;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
      node.proxyStr = ssrConstruct(
          group, remark, base64_encode(remark), address, port, protocol, method,
          obfs, password, obfsparam, protoparam, ssr_libev, udp, tfo, scv);
    }
    break;
  case "VMess"_hash:
    id = GetMember(json, "UserID");
    aid = GetMember(json, "AlterID");
    transprot = GetMember(json, "TransferProtocol");
    faketype = GetMember(json, "FakeType");
    host = GetMember(json, "Host");
    path = GetMember(json, "Path");
    edge = GetMember(json, "Edge");
    tls = GetMember(json, "TLSSecure");
    node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
    if (group.empty())
      group = V2RAY_DEFAULT_GROUP;
    node.group = group;
    node.proxyStr =
        vmessConstruct(group, remark, address, port, faketype, id, aid,
                       transprot, method, path, host, edge, tls, udp, tfo, scv);
    break;
  case "VLess"_hash:
    id = GetMember(json, "UserID");
    aid = GetMember(json, "AlterID");
    transprot = GetMember(json, "TransferProtocol");
    faketype = GetMember(json, "FakeType");
    host = GetMember(json, "Host");
    path = GetMember(json, "Path");
    edge = GetMember(json, "Edge");
    tls = GetMember(json, "TLSSecure");
    node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
    if (group.empty())
      group = V2RAY_DEFAULT_GROUP;
    node.group = group;
    // 修复：server -> address，net ->
    // transprot；并补齐新签名的5个可选字符串参数
    node.proxyStr = vlessConstruct(address, port, "", id, transprot, path, host,
                                   edge, tls, scv, "", "", "", "", "", "");
    break;
  case "Socks5"_hash:
    username = GetMember(json, "Username");
    node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
    if (group.empty())
      group = SOCKS_DEFAULT_GROUP;
    node.group = group;
    node.proxyStr = socksConstruct(group, remark, address, port, username,
                                   password, udp, tfo, scv);
    break;
  case "HTTP"_hash:
  case "HTTPS"_hash:
    node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
    if (group.empty())
      group = HTTP_DEFAULT_GROUP;
    node.group = group;
    node.proxyStr = httpConstruct(group, remark, address, port, username,
                                  password, type == "HTTPS", tfo, scv);
    break;
  case "Trojan"_hash:
    host = GetMember(json, "Host");
    tls = GetMember(json, "TLSSecure");
    node.linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
    if (group.empty())
      group = TROJAN_DEFAULT_GROUP;
    node.group = group;
    node.proxyStr = trojanConstruct(group, remark, address, port, password,
                                    host, tls == "true", udp, tfo, scv);
    break;
  case "Snell"_hash:
    obfs = GetMember(json, "OBFS");
    host = GetMember(json, "Host");
    node.linkType = SPEEDTEST_MESSAGE_FOUNDSNELL;
    if (group.empty())
      group = SNELL_DEFAULT_GROUP;
    node.group = group;
    node.proxyStr = snellConstruct(group, remark, address, port, password, obfs,
                                   host, udp, tfo, scv);
    break;
  default:
    return;
  }

  node.remarks = remark;
  node.server = address;
  node.port = (unsigned short)to_int(port, 1);
}

void explodeClash(Node yamlnode, const std::string &custom_port,
                  std::vector<nodeInfo> &nodes, bool ss_libev, bool ssr_libev) {
  std::string proxytype, ps, server, port, cipher, group, password; // common
  std::string type = "none", id, aid = "0", net = "tcp", path, host, edge,
              tls; // vmess
  std::string plugin, pluginopts, pluginopts_mode, pluginopts_host,
      pluginopts_mux;                                // ss
  std::string protocol, protoparam, obfs, obfsparam; // ssr
  std::string user;                                  // socks
  tribool udp, tfo, scv;
  nodeInfo node;
  Node singleproxy;
  unsigned int index = nodes.size();
  const std::string section =
      yamlnode["proxies"].IsDefined() ? "proxies" : "Proxy";
  for (unsigned int i = 0; i < yamlnode[section].size(); i++) {
    singleproxy = yamlnode[section][i];
    singleproxy["type"] >>= proxytype;
    singleproxy["name"] >>= ps;
    singleproxy["server"] >>= server;
    port = custom_port.empty() ? safe_as<std::string>(singleproxy["port"])
                               : custom_port;
    if (port.empty() || port == "0")
      continue;
    udp = safe_as<std::string>(singleproxy["udp"]);
    scv = safe_as<std::string>(singleproxy["skip-cert-verify"]);
    switch (hash_(proxytype)) {
    case "vmess"_hash:
      group = V2RAY_DEFAULT_GROUP;

      singleproxy["uuid"] >>= id;
      singleproxy["alterId"] >>= aid;
      singleproxy["cipher"] >>= cipher;
      net = singleproxy["network"].IsDefined()
                ? safe_as<std::string>(singleproxy["network"])
                : "tcp";
      if (net == "http") {
        singleproxy["http-opts"]["path"][0] >>= path;
        singleproxy["http-opts"]["headers"]["Host"][0] >>= host;
        edge.clear();
      } else {
        path = singleproxy["ws-path"].IsDefined()
                   ? safe_as<std::string>(singleproxy["ws-path"])
                   : "/";
        singleproxy["ws-headers"]["Host"] >>= host;
        singleproxy["ws-headers"]["Edge"] >>= edge;
      }
      tls = safe_as<std::string>(singleproxy["tls"]) == "true" ? "tls" : "";

      node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
      node.proxyStr =
          vmessConstruct(group, ps, server, port, "", id, aid, net, cipher,
                         path, host, edge, tls, udp, tfo, scv);
      break;
    case "vless"_hash:
      group = V2RAY_DEFAULT_GROUP;

      singleproxy["uuid"] >>= id;
      singleproxy["alterId"] >>= aid;
      singleproxy["cipher"] >>= cipher;
      net = singleproxy["network"].IsDefined()
                ? safe_as<std::string>(singleproxy["network"])
                : "tcp";
      if (net == "http") {
        singleproxy["http-opts"]["path"][0] >>= path;
        singleproxy["http-opts"]["headers"]["Host"][0] >>= host;
        edge.clear();
      } else {
        path = singleproxy["ws-path"].IsDefined()
                   ? safe_as<std::string>(singleproxy["ws-path"])
                   : "/";
        singleproxy["ws-headers"]["Host"] >>= host;
        singleproxy["ws-headers"]["Edge"] >>= edge;
      }
      tls = safe_as<std::string>(singleproxy["tls"]) == "true" ? "tls" : "";

      node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
      node.proxyStr = vlessConstruct(server, port, type, id, net, path, host,
                                     edge, tls, scv, "", "", "", "", "", "");
      break;
    case "ss"_hash:
      group = SS_DEFAULT_GROUP;

      singleproxy["cipher"] >>= cipher;
      singleproxy["password"] >>= password;
      if (singleproxy["plugin"].IsDefined()) {
        switch (hash_(safe_as<std::string>(singleproxy["plugin"]))) {
        case "obfs"_hash:
          plugin = "simple-obfs";
          if (singleproxy["plugin-opts"].IsDefined()) {
            singleproxy["plugin-opts"]["mode"] >>= pluginopts_mode;
            singleproxy["plugin-opts"]["host"] >>= pluginopts_host;
          }
          break;
        case "v2ray-plugin"_hash:
          plugin = "v2ray-plugin";
          if (singleproxy["plugin-opts"].IsDefined()) {
            singleproxy["plugin-opts"]["mode"] >>= pluginopts_mode;
            singleproxy["plugin-opts"]["host"] >>= pluginopts_host;
            tls =
                safe_as<bool>(singleproxy["plugin-opts"]["tls"]) ? "tls;" : "";
            singleproxy["plugin-opts"]["path"] >>= path;
            pluginopts_mux = safe_as<bool>(singleproxy["plugin-opts"]["mux"])
                                 ? "mux=4;"
                                 : "";
          }
          break;
        default:
          break;
        }
      } else if (singleproxy["obfs"].IsDefined()) {
        plugin = "simple-obfs";
        singleproxy["obfs"] >>= pluginopts_mode;
        singleproxy["obfs-host"] >>= pluginopts_host;
      } else
        plugin.clear();

      switch (hash_(plugin)) {
      case "simple-obfs"_hash:
      case "obfs-local"_hash:
        pluginopts = "obfs=" + pluginopts_mode;
        pluginopts +=
            pluginopts_host.empty() ? "" : ";obfs-host=" + pluginopts_host;
        break;
      case "v2ray-plugin"_hash:
        pluginopts = "mode=" + pluginopts_mode + ";" + tls + pluginopts_mux;
        if (pluginopts_host.size())
          pluginopts += "host=" + pluginopts_host + ";";
        if (path.size())
          pluginopts += "path=" + path + ";";
        if (pluginopts_mux.size())
          pluginopts += "mux=" + pluginopts_mux + ";";
        break;
      }

      // support for go-shadowsocks2
      if (cipher == "AEAD_CHACHA20_POLY1305")
        cipher = "chacha20-ietf-poly1305";
      else if (strFind(cipher, "AEAD")) {
        cipher = replace_all_distinct(replace_all_distinct(cipher, "AEAD_", ""),
                                      "_", "-");
        std::transform(cipher.begin(), cipher.end(), cipher.begin(), ::tolower);
      }

      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.proxyStr = ssConstruct(group, ps, server, port, password, cipher,
                                  plugin, pluginopts, ss_libev, udp, tfo, scv);
      break;
    case "socks"_hash:
      group = SOCKS_DEFAULT_GROUP;

      singleproxy["username"] >>= user;
      singleproxy["password"] >>= password;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
      node.proxyStr = socksConstruct(group, ps, server, port, user, password);
      break;
    case "ssr"_hash:
      group = SSR_DEFAULT_GROUP;

      singleproxy["cipher"] >>= cipher;
      singleproxy["password"] >>= password;
      singleproxy["protocol"] >>= protocol;
      singleproxy["obfs"] >>= obfs;
      if (singleproxy["protocol-param"].IsDefined())
        singleproxy["protocol-param"] >>= protoparam;
      else
        singleproxy["protocolparam"] >>= protoparam;
      if (singleproxy["obfs-param"].IsDefined())
        singleproxy["obfs-param"] >>= obfsparam;
      else
        singleproxy["obfsparam"] >>= obfsparam;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
      node.proxyStr = ssrConstruct(group, ps, base64_encode(ps), server, port,
                                   protocol, cipher, obfs, password, obfsparam,
                                   protoparam, ssr_libev, udp, tfo, scv);
      break;
    case "http"_hash:
      group = HTTP_DEFAULT_GROUP;

      singleproxy["username"] >>= user;
      singleproxy["password"] >>= password;
      singleproxy["tls"] >>= tls;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
      node.proxyStr = httpConstruct(group, ps, server, port, user, password,
                                    tls == "true", tfo, scv);
      break;
    case "trojan"_hash:
      group = TROJAN_DEFAULT_GROUP;
      singleproxy["password"] >>= password;
      singleproxy["sni"] >>= host;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
      node.proxyStr = trojanConstruct(group, ps, server, port, password, host,
                                      true, udp, tfo, scv);
      break;
    case "snell"_hash:
      group = SNELL_DEFAULT_GROUP;
      singleproxy["psk"] >> password;
      singleproxy["obfs-opts"]["mode"] >>= obfs;
      singleproxy["obfs-opts"]["host"] >>= host;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDSNELL;
      node.proxyStr = snellConstruct(group, ps, server, port, password, obfs,
                                     host, udp, tfo, scv);
      break;
    default:
      continue;
    }

    node.group = group;
    node.remarks = ps;
    node.server = server;
    node.port = to_int(port, 1);
    node.id = index;
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
  return;
}

void explodeStdVMess(std::string vmess, const std::string &custom_port,
                     nodeInfo &node) {
  std::string add, port, type, id, aid, net, path, host, tls, remarks;
  std::string addition;
  vmess = vmess.substr(8);
  string_size pos;

  pos = vmess.rfind("#");
  if (pos != vmess.npos) {
    remarks = UrlDecode(vmess.substr(pos + 1));
    vmess.erase(pos);
  }
  const std::string stdvmess_matcher =
      R"(^([a-z]+)(?:\+([a-z]+))?:([\da-f]{4}(?:[\da-f]{4}-){4}[\da-f]{12})-(\d+)@(.+):(\d+)(?:\/?\?(.*))?$)";
  if (regGetMatch(vmess, stdvmess_matcher, 8, 0, &net, &tls, &id, &aid, &add,
                  &port, &addition))
    return;

  switch (hash_(net)) {
  case "tcp"_hash:
  case "kcp"_hash:
    type = getUrlArg(addition, "type");
    break;
  case "http"_hash:
  case "ws"_hash:
    host = getUrlArg(addition, "host");
    path = getUrlArg(addition, "path");
    break;
  case "quic"_hash:
    type = getUrlArg(addition, "security");
    host = getUrlArg(addition, "type");
    path = getUrlArg(addition, "key");
    break;
  default:
    return;
  }

  if (!custom_port.empty())
    port = custom_port;
  if (remarks.empty())
    remarks = add + ":" + port;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = remarks;
  node.server = add;
  node.port = to_int(port, 0);
  node.proxyStr = vmessConstruct(node.group, remarks, add, port, type, id, aid,
                                 net, "auto", path, host, "", tls);
  return;
}
void explodeStdVLess(std::string vless, const std::string &custom_port,
                     nodeInfo &node) {
  std::string add, port, type, id, aid, net, path, host, tls, remarks;
  std::string addition, sni;

  if (!regMatch(vless, "(?i)^vless://.*"))
    return;

  // 记录原始 vless 分享链接
  if (node.originalUrl.empty())
    node.originalUrl = vless;

  vless = vless.substr(8);
  string_size pos;

  pos = vless.rfind("#");
  if (pos != vless.npos) {
    remarks = UrlDecode(vless.substr(pos + 1));
    vless.erase(pos);
  }

  pos = vless.find("?");
  if (pos != vless.npos) {
    addition = vless.substr(pos + 1);
    vless.erase(pos);
  }

  pos = vless.find("@");
  if (pos == vless.npos)
    return;
  id = vless.substr(0, pos);
  std::string addrport = vless.substr(pos + 1);

  if (!addrport.empty() && addrport[0] == '[') {
    string_size rb = addrport.find("]");
    if (rb == addrport.npos || rb + 1 >= addrport.size() ||
        addrport[rb + 1] != ':')
      return;
    add = addrport.substr(1, rb - 1);
    port = addrport.substr(rb + 2);
  } else {
    string_size cpos = addrport.rfind(":");
    if (cpos == addrport.npos)
      return;
    add = addrport.substr(0, cpos);
    port = addrport.substr(cpos + 1);
  }

  net = trim(getUrlArg(addition, "type"));
  tls = trim(getUrlArg(addition, "security"));
  host = getUrlArg(addition, "host");
  path = getUrlArg(addition, "path");
  sni = getUrlArg(addition, "sni");
  std::string serviceName = getUrlArg(addition, "serviceName");
  std::string headerType = trim(getUrlArg(addition, "headerType"));
  std::string alpn = getUrlArg(addition, "alpn");
  std::string fp = getUrlArg(addition, "fp");
  std::string flow = getUrlArg(addition, "flow");
  std::string pbk = getUrlArg(addition, "pbk");
  std::string sid = getUrlArg(addition, "sid");

  // Reality 参数校验与提示
  if (regMatch(tls, "(?i)^reality$")) {
    if (trim(pbk).empty()) {
      writeLog(
          0,
          "VLESS(reality) pbk(publicKey) is empty; realitySettings.publicKey "
          "will be empty and the client cannot establish REALITY handshake.",
          LOG_LEVEL_WARNING);
    }
    if (trim(sid).empty()) {
      writeLog(
          0,
          "VLESS(reality) sid(shortId) is empty; some servers require shortId "
          "for routing.",
          LOG_LEVEL_WARNING);
    }
    bool sni_missing = trim(sni).empty();
    bool host_missing = trim(host).empty();
    bool add_is_ip = isIPv4(add) || isIPv6(add);
    if (sni_missing && host_missing && add_is_ip) {
      writeLog(
          0,
          "VLESS(reality) SNI(serverName) not provided and cannot be derived "
          "from host/domain (server address is IP). realitySettings.serverName "
          "will be empty, handshake may fail.",
          LOG_LEVEL_WARNING);
    }
  }

  // parse mux and concurrency
  std::string mux_arg = getUrlArg(addition, "mux");
  std::string conc_arg = getUrlArg(addition, "concurrency");
  std::string mux_json;
  {
    std::string m = trim(mux_arg);
    std::string c = trim(conc_arg);
    if (!m.empty() || !c.empty()) {
      bool enabled = true;
      int concurrency = -1;
      if (!c.empty() && regMatch(c, "^-?\\d+$"))
        concurrency = to_int(c, -1);
      if (!m.empty()) {
        if (regMatch(m, "(?i)^(false|0)$")) {
          enabled = false;
        } else if (regMatch(m, "(?i)^(true|1)$")) {
          enabled = true;
        } else if (regMatch(m, "^-?\\d+$")) {
          concurrency = to_int(m, -1);
          enabled = (concurrency > 0);
        }
      } else {
        // no mux given but concurrency provided, enable by default
        enabled = true;
      }
      if (concurrency == 0)
        enabled = false;
      mux_json = std::string("{\"enabled\":") + (enabled ? "true" : "false") +
                 ",\"concurrency\":" + std::to_string(concurrency) + "}";
    }
  }

  if (net == "xhttp" || net == "httpupgrade")
    net = "http";
  if (net.empty())
    net = "tcp";

  if (net == "grpc") {
    if (!serviceName.empty())
      path = serviceName;
    if (trim(path).empty()) {
      writeLog(
          0,
          "VLESS(grpc) serviceName/path is empty; grpcSettings.serviceName "
          "will be empty, which may not work with some servers.",
          LOG_LEVEL_WARNING);
    }
  } else if (net == "tcp") {
    if (headerType == "http" || trim(getUrlArg(addition, "type")) == "http")
      type = "http";
  } else if (net == "kcp") {
    if (!headerType.empty())
      type = headerType;
  } else if (net == "quic") {
    type = getUrlArg(addition, "security");
    host = getUrlArg(addition, "type");
    path = getUrlArg(addition, "key");
  }

  if (!custom_port.empty())
    port = custom_port;
  if (port == "0")
    return;
  if (remarks.empty())
    remarks = add + ":" + port;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = remarks;
  node.server = add;
  node.port = to_int(port, 0);
  node.proxyStr =
      vlessConstruct(add, port, type, id, net, path, host, sni, tls,
                     tribool() /*scv*/, alpn, fp, flow, pbk, sid, mux_json);
  return;
}

void explodeShadowrocket(std::string rocket, const std::string &custom_port,
                         nodeInfo &node) {
  std::string add, port, type, id, aid, net = "tcp", path, host, tls, cipher,
                                        remarks;
  std::string obfs; // for other style of link
  std::string addition;
  rocket = rocket.substr(8);

  string_size pos = rocket.find("?");
  addition = rocket.substr(pos + 1);
  rocket.erase(pos);

  if (regGetMatch(urlsafe_base64_decode(rocket), "(.*?):(.*)@(.*):(.*)", 5, 0,
                  &cipher, &id, &add, &port))
    return;
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;
  remarks = UrlDecode(getUrlArg(addition, "remarks"));
  obfs = getUrlArg(addition, "obfs");
  if (obfs.size()) {
    if (obfs == "websocket") {
      net = "ws";
      host = getUrlArg(addition, "obfsParam");
      path = getUrlArg(addition, "path");
    }
  } else {
    net = getUrlArg(addition, "network");
    host = getUrlArg(addition, "wsHost");
    path = getUrlArg(addition, "wspath");
  }
  tls = getUrlArg(addition, "tls") == "1" ? "tls" : "";
  aid = getUrlArg(addition, "aid");

  if (aid.empty())
    aid = "0";

  if (remarks.empty())
    remarks = add + ":" + port;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = remarks;
  node.server = add;
  node.port = to_int(port, 0);
  node.proxyStr = vmessConstruct(node.group, remarks, add, port, type, id, aid,
                                 net, cipher, path, host, "", tls);
}

void explodeKitsunebi(std::string kit, const std::string &custom_port,
                      nodeInfo &node) {
  std::string add, port, type, id, aid = "0", net = "tcp", path, host, tls,
                                   cipher = "auto", remarks;
  std::string addition;
  string_size pos;
  kit = kit.substr(9);

  pos = kit.find("#");
  if (pos != kit.npos) {
    remarks = kit.substr(pos + 1);
    kit = kit.substr(0, pos);
  }

  pos = kit.find("?");
  addition = kit.substr(pos + 1);
  kit = kit.substr(0, pos);

  if (regGetMatch(kit, "(.*?)@(.*):(.*)", 4, 0, &id, &add, &port))
    return;
  pos = port.find("/");
  if (pos != port.npos) {
    path = port.substr(pos);
    port.erase(pos);
  }
  if (custom_port.size())
    port = custom_port;
  if (port == "0")
    return;
  net = getUrlArg(addition, "network");
  tls = getUrlArg(addition, "tls") == "true" ? "tls" : "";
  host = getUrlArg(addition, "ws.host");

  if (remarks.empty())
    remarks = add + ":" + port;

  node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
  node.group = V2RAY_DEFAULT_GROUP;
  node.remarks = remarks;
  node.server = add;
  node.port = to_int(port, 0);
  node.proxyStr = vmessConstruct(node.group, remarks, add, port, type, id, aid,
                                 net, cipher, path, host, "", tls);
}

bool explodeSurge(std::string surge, const std::string &custom_port,
                  std::vector<nodeInfo> &nodes, bool libev) {
  // 空串防护
  if (surge.empty())
    return false;

  std::multimap<std::string, std::string> proxies;
  nodeInfo node;
  unsigned int i, index = nodes.size();
  INIReader ini;

  /*
  if(!strFind(surge, "[Proxy]"))
      return false;
  */

  ini.store_isolated_line = true;
  ini.keep_empty_section = false;
  ini.allow_dup_section_titles = true;
  ini.SetIsolatedItemsSection("Proxy");
  ini.IncludeSection("Proxy");
  ini.AddDirectSaveSection("Proxy");
  if (surge.find("[Proxy]") != surge.npos)
    surge = regReplace(surge, "^[\\S\\s]*?\\[", "[", false);
  ini.Parse(surge);

  if (!ini.SectionExist("Proxy"))
    return false;
  ini.EnterSection("Proxy");
  ini.GetItems(proxies);

  const std::string proxystr = "(.*?)\\s*=\\s*(.*)";

  for (auto &x : proxies) {
    std::string remarks, server, port, method, username, password; // common
    std::string plugin, pluginopts, pluginopts_mode, pluginopts_host, mod_url,
        mod_md5;                                // ss
    std::string id, net, tls, host, edge, path; // v2
    std::string protocol, protoparam;           // ssr
    std::string itemName, itemVal, config;
    std::vector<std::string> configs, vArray, headers, header;
    tribool udp, tfo, scv, tls13;

    /*
    remarks = regReplace(x.second, proxystr, "$1");
    configs = split(regReplace(x.second, proxystr, "$2"), ",");
    */
    regGetMatch(x.second, proxystr, 3, 0, &remarks, &config);
    configs = split(config, ",");
    if (configs.size() < 3)
      continue;
    switch (hash_(configs[0])) {
    case "direct"_hash:
    case "reject"_hash:
    case "reject-tinygif"_hash:
      continue;
    case "custom"_hash: // surge 2 style custom proxy
      // remove module detection to speed up parsing and compatible with broken
      // module
      /*
      mod_url = trim(configs[5]);
      if(parsedMD5.count(mod_url) > 0)
      {
          mod_md5 = parsedMD5[mod_url]; //read calculated MD5 from map
      }
      else
      {
          mod_md5 = getMD5(webGet(mod_url)); //retrieve module and calculate MD5
          parsedMD5.insert(std::pair<std::string, std::string>(mod_url,
      mod_md5)); //save unrecognized module MD5 to map
      }
      */

      // if(mod_md5 == modSSMD5) //is SSEncrypt module
      {
        if (configs.size() < 5)
          continue;
        server = trim(configs[1]);
        port = custom_port.empty() ? trim(configs[2]) : custom_port;
        if (port == "0")
          continue;
        method = trim(configs[3]);
        password = trim(configs[4]);

        for (i = 6; i < configs.size(); i++) {
          vArray = split(configs[i], "=");
          if (vArray.size() < 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "obfs"_hash:
            plugin = "simple-obfs";
            pluginopts_mode = itemVal;
            break;
          case "obfs-host"_hash:
            pluginopts_host = itemVal;
            break;
          case "udp-relay"_hash:
            udp = itemVal;
            break;
          case "tfo"_hash:
            tfo = itemVal;
            break;
          default:
            continue;
          }
        }
        if (plugin.size()) {
          pluginopts = "obfs=" + pluginopts_mode;
          pluginopts +=
              pluginopts_host.empty() ? "" : ";obfs-host=" + pluginopts_host;
        }

        node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
        node.group = SS_DEFAULT_GROUP;
        node.proxyStr =
            ssConstruct(node.group, remarks, server, port, password, method,
                        plugin, pluginopts, libev, udp, tfo, scv);
      }
      // else
      //     continue;
      break;
    case "ss"_hash: // surge 3 style ss proxy
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;

      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() < 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "encrypt-method"_hash:
          method = itemVal;
          break;
        case "password"_hash:
          password = itemVal;
          break;
        case "obfs"_hash:
          plugin = "simple-obfs";
          pluginopts_mode = itemVal;
          break;
        case "obfs-host"_hash:
          pluginopts_host = itemVal;
          break;
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        default:
          continue;
        }
      }
      if (plugin.size()) {
        pluginopts = "obfs=" + pluginopts_mode;
        pluginopts +=
            pluginopts_host.empty() ? "" : ";obfs-host=" + pluginopts_host;
      }

      node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
      node.group = SS_DEFAULT_GROUP;
      node.proxyStr =
          ssConstruct(node.group, remarks, server, port, password, method,
                      plugin, pluginopts, libev, udp, tfo, scv);
      break;
    case "socks5"_hash: // surge 3 style socks5 proxy
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
      node.group = SOCKS_DEFAULT_GROUP;
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;
      if (configs.size() >= 5) {
        username = trim(configs[3]);
        password = trim(configs[4]);
      }
      for (i = 5; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() < 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        default:
          continue;
        }
      }
      node.proxyStr = socksConstruct(node.group, remarks, server, port,
                                     username, password, udp, tfo, scv);
      break;
    case "vmess"_hash: // surge 4 style vmess proxy
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;
      net = "tcp";
      method = "auto";

      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() != 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "username"_hash:
          id = itemVal;
          break;
        case "ws"_hash:
          net = itemVal == "true" ? "ws" : "tcp";
          break;
        case "tls"_hash:
          tls = itemVal == "true" ? "tls" : "";
          break;
        case "ws-path"_hash:
          path = itemVal;
          break;
        case "obfs-host"_hash:
          host = itemVal;
          break;
        case "ws-headers"_hash:
          headers = split(itemVal, "|");
          for (auto &y : headers) {
            header = split(trim(y), ":");
            if (header.size() != 2)
              continue;
            else if (regMatch(header[0], "(?i)host"))
              host = trim_quote(header[1]);
            else if (regMatch(header[0], "(?i)edge"))
              edge = trim_quote(header[1]);
          }
          break;
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        case "tls13"_hash:
          tls13 = itemVal;
          break;
        default:
          continue;
        }
      }
      if (host.empty() && !isIPv4(server) && !isIPv6(server))
        host = server;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
      node.group = V2RAY_DEFAULT_GROUP;
      node.proxyStr =
          vmessConstruct(node.group, remarks, server, port, "", id, "0", net,
                         method, path, host, edge, tls, udp, tfo, scv, tls13);
      break;
    case "vless"_hash: // surge 4 style vless proxy
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;
      net = "tcp";
      method = "auto";

      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() != 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "username"_hash:
          id = itemVal;
          break;
        case "ws"_hash:
          net = itemVal == "true" ? "ws" : "tcp";
          break;
        case "tls"_hash:
          tls = itemVal == "true" ? "tls" : "";
          break;
        case "ws-path"_hash:
          path = itemVal;
          break;
        case "obfs-host"_hash:
          host = itemVal;
          break;
        case "ws-headers"_hash:
          headers = split(itemVal, "|");
          for (auto &y : headers) {
            header = split(trim(y), ":");
            if (header.size() != 2)
              continue;
            else if (regMatch(header[0], "(?i)host"))
              host = trim_quote(header[1]);
            else if (regMatch(header[0], "(?i)edge"))
              edge = trim_quote(header[1]);
          }
          break;
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        case "tls13"_hash:
          tls13 = itemVal;
          break;
        default:
          continue;
        }
      }
      if (host.empty() && !isIPv4(server) && !isIPv6(server))
        host = server;

      node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
      node.group = V2RAY_DEFAULT_GROUP;
      // 修复：补齐 alpn/fp/flow/pbk/sid 五个参数
      node.proxyStr = vlessConstruct(server, port, "", id, net, path, host,
                                     edge, tls, scv, "", "", "", "", "", "");
      break;
    case "http"_hash: // http proxy
      node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
      node.group = HTTP_DEFAULT_GROUP;
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;
      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() < 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "username"_hash:
          username = itemVal;
          break;
        case "password"_hash:
          password = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        default:
          continue;
        }
      }
      node.proxyStr = httpConstruct(node.group, remarks, server, port, username,
                                    password, false, tfo, scv);
      break;
    case "trojan"_hash: // surge 4 style trojan proxy
      node.linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
      node.group = TROJAN_DEFAULT_GROUP;
      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;

      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() != 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "password"_hash:
          password = itemVal;
          break;
        case "sni"_hash:
          host = itemVal;
          break;
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        default:
          continue;
        }
      }
      if (host.empty() && !isIPv4(server) && !isIPv6(server))
        host = server;

      node.proxyStr = trojanConstruct(node.group, remarks, server, port,
                                      password, host, true, udp, tfo, scv);
      break;
    case "snell"_hash:
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSNELL;
      node.group = SNELL_DEFAULT_GROUP;

      server = trim(configs[1]);
      port = custom_port.empty() ? trim(configs[2]) : custom_port;
      if (port == "0")
        continue;

      for (i = 3; i < configs.size(); i++) {
        vArray = split(configs[i], "=");
        if (vArray.size() != 2)
          continue;
        itemName = trim(vArray[0]);
        itemVal = trim(vArray[1]);
        switch (hash_(itemName)) {
        case "psk"_hash:
          password = itemVal;
          break;
        case "obfs"_hash:
          plugin = itemVal;
          break;
        case "obfs-host"_hash:
          host = itemVal;
          break;
        case "udp-relay"_hash:
          udp = itemVal;
          break;
        case "tfo"_hash:
          tfo = itemVal;
          break;
        case "skip-cert-verify"_hash:
          scv = itemVal;
          break;
        default:
          continue;
        }
      }
      if (host.empty() && !isIPv4(server) && !isIPv6(server))
        host = server;

      node.proxyStr = snellConstruct(node.group, remarks, server, port,
                                     password, plugin, host, udp, tfo, scv);
      break;
    default:
      switch (hash_(remarks)) {
      case "shadowsocks"_hash: // quantumult x style ss/ssr link
        server = trim(configs[0].substr(0, configs[0].rfind(":")));
        port = custom_port.empty()
                   ? trim(configs[0].substr(configs[0].rfind(":") + 1))
                   : custom_port;
        if (port == "0")
          continue;

        for (i = 1; i < configs.size(); i++) {
          vArray = split(trim(configs[i]), "=");
          if (vArray.size() != 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "method"_hash:
            method = itemVal;
            break;
          case "password"_hash:
            password = itemVal;
            break;
          case "tag"_hash:
            remarks = itemVal;
            break;
          case "ssr-protocol"_hash:
            protocol = itemVal;
            break;
          case "ssr-protocol-param"_hash:
            protoparam = itemVal;
            break;
          case "obfs"_hash: {
            switch (hash_(itemVal)) {
            case "http"_hash:
            case "tls"_hash:
              plugin = "simple-obfs";
              pluginopts_mode = itemVal;
              break;
            case "wss"_hash:
              tls = "tls";
              [[fallthrough]];
            case "ws"_hash:
              pluginopts_mode = "websocket";
              plugin = "v2ray-plugin";
              break;
            default:
              pluginopts_mode = itemVal;
            }
            break;
          }
          case "obfs-host"_hash:
            pluginopts_host = itemVal;
            break;
          case "obfs-uri"_hash:
            path = itemVal;
            break;
          case "udp-relay"_hash:
            udp = itemVal;
            break;
          case "fast-open"_hash:
            tfo = itemVal;
            break;
          case "tls13"_hash:
            tls13 = itemVal;
            break;
          default:
            continue;
          }
        }
        if (remarks.empty())
          remarks = server + ":" + port;
        switch (hash_(plugin)) {
        case "simple-obfs"_hash:
          pluginopts = "obfs=" + pluginopts_mode;
          if (pluginopts_host.size())
            pluginopts += ";obfs-host=" + pluginopts_host;
          break;
        case "v2ray-plugin"_hash:
          if (pluginopts_host.empty() && !isIPv4(server) && !isIPv6(server))
            pluginopts_host = server;
          pluginopts = "mode=" + pluginopts_mode;
          if (pluginopts_host.size())
            pluginopts += ";host=" + pluginopts_host;
          if (path.size())
            pluginopts += ";path=" + path;
          pluginopts += ";" + tls;
          break;
        }

        if (protocol.size()) {
          node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
          node.group = SSR_DEFAULT_GROUP;
          node.proxyStr =
              ssrConstruct(node.group, remarks, base64_encode(remarks), server,
                           port, protocol, method, pluginopts_mode, password,
                           pluginopts_host, protoparam, libev, udp, tfo, scv);
        } else {
          node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
          node.group = SS_DEFAULT_GROUP;
          node.proxyStr =
              ssConstruct(node.group, remarks, server, port, password, method,
                          plugin, pluginopts, libev, udp, tfo, scv, tls13);
        }
        break;
      case "vmess"_hash: // quantumult x style vmess link
        server = trim(configs[0].substr(0, configs[0].rfind(":")));
        port = custom_port.empty()
                   ? trim(configs[0].substr(configs[0].rfind(":") + 1))
                   : custom_port;
        if (port == "0")
          continue;
        net = "tcp";

        for (i = 1; i < configs.size(); i++) {
          vArray = split(trim(configs[i]), "=");
          if (vArray.size() != 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "method"_hash:
            method = itemVal;
            break;
          case "password"_hash:
            id = itemVal;
            break;
          case "tag"_hash:
            remarks = itemVal;
            break;
          case "obfs"_hash:
            switch (hash_(itemVal)) {
            case "ws"_hash:
              net = "ws";
              break;
            case "over-tls"_hash:
              tls = "tls";
              break;
            case "wss"_hash:
              net = "ws";
              tls = "tls";
              break;
            }
            break;
          case "obfs-host"_hash:
            host = itemVal;
            break;
          case "obfs-uri"_hash:
            path = itemVal;
            break;
          case "over-tls"_hash:
            tls = itemVal == "true" ? "tls" : "";
            break;
          case "udp-relay"_hash:
            udp = itemVal;
            break;
          case "fast-open"_hash:
            tfo = itemVal;
            break;
          case "tls13"_hash:
            tls13 = itemVal;
            break;
          default:
            continue;
          }
        }
        if (remarks.empty())
          remarks = server + ":" + port;

        if (host.empty() && !isIPv4(server) && !isIPv6(server))
          host = server;

        node.linkType = SPEEDTEST_MESSAGE_FOUNDVMESS;
        node.group = V2RAY_DEFAULT_GROUP;
        node.proxyStr =
            vmessConstruct(node.group, remarks, server, port, "", id, "0", net,
                           method, path, host, "", tls, udp, tfo, scv, tls13);
        break;
      case "vless"_hash: // quantumult x style vless link
        server = trim(configs[0].substr(0, configs[0].rfind(":")));
        port = custom_port.empty()
                   ? trim(configs[0].substr(configs[0].rfind(":") + 1))
                   : custom_port;
        if (port == "0")
          continue;
        net = "tcp";

        for (i = 1; i < configs.size(); i++) {
          vArray = split(trim(configs[i]), "=");
          if (vArray.size() != 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "method"_hash:
            method = itemVal;
            break;
          case "password"_hash:
            id = itemVal;
            break;
          case "tag"_hash:
            remarks = itemVal;
            break;
          case "obfs"_hash:
            switch (hash_(itemVal)) {
            case "ws"_hash:
              net = "ws";
              break;
            case "over-tls"_hash:
              tls = "tls";
              break;
            case "wss"_hash:
              net = "ws";
              tls = "tls";
              break;
            }
            break;
          case "obfs-host"_hash:
            host = itemVal;
            break;
          case "obfs-uri"_hash:
            path = itemVal;
            break;
          case "over-tls"_hash:
            tls = itemVal == "true" ? "tls" : "";
            break;
          case "udp-relay"_hash:
            udp = itemVal;
            break;
          case "fast-open"_hash:
            tfo = itemVal;
            break;
          case "tls13"_hash:
            tls13 = itemVal;
            break;
          default:
            continue;
          }
        }
        if (remarks.empty())
          remarks = server + ":" + port;

        if (host.empty() && !isIPv4(server) && !isIPv6(server))
          host = server;
        node.linkType = SPEEDTEST_MESSAGE_FOUNDVLESS;
        node.group = V2RAY_DEFAULT_GROUP;
        // 修复：补齐 alpn/fp/flow/pbk/sid 五个参数
        node.proxyStr = vlessConstruct(server, port, "", id, net, path, host,
                                       edge, tls, scv, "", "", "", "", "", "");
        break;

      case "trojan"_hash: // quantumult x style trojan link
        server = trim(configs[0].substr(0, configs[0].rfind(":")));
        port = custom_port.empty()
                   ? trim(configs[0].substr(configs[0].rfind(":") + 1))
                   : custom_port;
        if (port == "0")
          continue;

        for (i = 1; i < configs.size(); i++) {
          vArray = split(trim(configs[i]), "=");
          if (vArray.size() != 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "password"_hash:
            password = itemVal;
            break;
          case "tag"_hash:
            remarks = itemVal;
            break;
          case "over-tls"_hash:
            tls = itemVal;
            break;
          case "tls-host"_hash:
            host = itemVal;
            break;
          case "udp-relay"_hash:
            udp = itemVal;
            break;
          case "fast-open"_hash:
            tfo = itemVal;
            break;
          case "tls-verification"_hash:
            scv = itemVal == "false";
            break;
          case "tls13"_hash:
            tls13 = itemVal;
            break;
          default:
            continue;
          }
        }
        if (remarks.empty())
          remarks = server + ":" + port;

        if (host.empty() && !isIPv4(server) && !isIPv6(server))
          host = server;

        node.linkType = SPEEDTEST_MESSAGE_FOUNDTROJAN;
        node.group = TROJAN_DEFAULT_GROUP;
        node.proxyStr =
            trojanConstruct(node.group, remarks, server, port, password, host,
                            tls == "true", udp, tfo, scv, tls13);
        break;
      case "http"_hash: // quantumult x style http links
        server = trim(configs[0].substr(0, configs[0].rfind(":")));
        port = custom_port.empty()
                   ? trim(configs[0].substr(configs[0].rfind(":") + 1))
                   : custom_port;
        if (port == "0")
          continue;

        for (i = 1; i < configs.size(); i++) {
          vArray = split(trim(configs[i]), "=");
          if (vArray.size() != 2)
            continue;
          itemName = trim(vArray[0]);
          itemVal = trim(vArray[1]);
          switch (hash_(itemName)) {
          case "username"_hash:
            username = itemVal;
            break;
          case "password"_hash:
            password = itemVal;
            break;
          case "tag"_hash:
            remarks = itemVal;
            break;
          case "over-tls"_hash:
            tls = itemVal;
            break;
          case "tls-verification"_hash:
            scv = itemVal == "false";
            break;
          case "tls13"_hash:
            tls13 = itemVal;
            break;
          case "fast-open"_hash:
            tfo = itemVal;
            break;
          default:
            continue;
          }
        }
        if (remarks.empty())
          remarks = server + ":" + port;

        if (host.empty() && !isIPv4(server) && !isIPv6(server))
          host = server;

        if (username == "none")
          username.clear();
        if (password == "none")
          password.clear();

        node.linkType = SPEEDTEST_MESSAGE_FOUNDHTTP;
        node.group = HTTP_DEFAULT_GROUP;
        node.proxyStr =
            httpConstruct(node.group, remarks, server, port, username, password,
                          tls == "true", tfo, scv, tls13);
        break;
      default:
        continue;
      }
      break;
    }

    node.remarks = remarks;
    node.server = server;
    node.port = to_int(port);
    node.id = index;
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
  return index;
}

void explodeSSTap(std::string sstap, const std::string &custom_port,
                  std::vector<nodeInfo> &nodes, bool ss_libev, bool ssr_libev) {
  // 空串防护
  if (sstap.empty())
    return;

  std::string configType, group, remarks, server, port;
  std::string cipher;
  std::string user, pass;
  std::string protocol, protoparam, obfs, obfsparam;
  Document json;
  nodeInfo node;
  unsigned int index = nodes.size();
  json.Parse(sstap.c_str());
  if (json.HasParseError())
    return;

  for (unsigned int i = 0; i < json["configs"].Size(); i++) {
    json["configs"][i]["group"] >> group;
    json["configs"][i]["remarks"] >> remarks;
    json["configs"][i]["server"] >> server;
    port = custom_port.size() ? custom_port
                              : GetMember(json["configs"][i], "server_port");
    if (port == "0")
      continue;

    if (remarks.empty())
      remarks = server + ":" + port;

    json["configs"][i]["password"] >> pass;
    json["configs"][i]["type"] >> configType;
    switch (to_int(configType, 0)) {
    case 5: // socks 5
      json["configs"][i]["username"] >> user;
      node.linkType = SPEEDTEST_MESSAGE_FOUNDSOCKS;
      node.proxyStr = socksConstruct(group, remarks, server, port, user, pass);
      break;
    case 6: // ss/ssr
      json["configs"][i]["protocol"] >> protocol;
      json["configs"][i]["obfs"] >> obfs;
      json["configs"][i]["method"] >> cipher;
      if (find(ss_ciphers.begin(), ss_ciphers.end(), cipher) !=
              ss_ciphers.end() &&
          protocol == "origin" && obfs == "plain") // is ss
      {
        node.linkType = SPEEDTEST_MESSAGE_FOUNDSS;
        node.proxyStr = ssConstruct(group, remarks, server, port, pass, cipher,
                                    "", "", ss_libev);
      } else // is ssr cipher
      {
        json["configs"][i]["obfsparam"] >> obfsparam;
        json["configs"][i]["protocolparam"] >> protoparam;
        node.linkType = SPEEDTEST_MESSAGE_FOUNDSSR;
        node.proxyStr = ssrConstruct(group, remarks, base64_encode(remarks),
                                     server, port, protocol, cipher, obfs, pass,
                                     obfsparam, protoparam, ssr_libev);
      }
      break;
    default:
      continue;
    }

    node.group = group;
    node.remarks = remarks;
    node.id = index;
    node.server = server;
    node.port = to_int(port, 1);
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
  }
}

void explodeNetchConf(std::string netch, bool ss_libev, bool ssr_libev,
                      const std::string &custom_port,
                      std::vector<nodeInfo> &nodes) {
  // 空串防护
  if (netch.empty())
    return;

  // 输入为 Base64 时，先进行解码
  netch = urlsafe_base64_decode(netch);

  Document json;
  nodeInfo node;
  unsigned int index = nodes.size();

  json.Parse(netch.c_str());
  if (json.HasParseError())
    return;

  if (!json.HasMember("Server"))
    return;

  for (unsigned int i = 0; i < json["Server"].Size(); i++) {
    explodeNetch("Netch://" + base64_encode(SerializeObject(json["Server"][i])),
                 ss_libev, ssr_libev, custom_port, node);

    node.id = index;
    nodes.emplace_back(std::move(node));
    node = nodeInfo();
    index++;
  }
}

bool chkIgnore(const nodeInfo &node, string_array &exclude_remarks,
               string_array &include_remarks) {
  bool excluded = false, included = false;
  // std::string remarks = UTF8ToACP(node.remarks);
  std::string remarks = node.remarks;
  // writeLog(LOG_TYPE_INFO, "Comparing exclude remarks...");
  excluded =
      std::any_of(exclude_remarks.cbegin(), exclude_remarks.cend(),
                  [&remarks](const auto &x) { return regFind(remarks, x); });
  if (include_remarks.size() != 0) {
    // writeLog(LOG_TYPE_INFO, "Comparing include remarks...");
    included =
        std::any_of(include_remarks.cbegin(), include_remarks.cend(),
                    [&remarks](const auto &x) { return regFind(remarks, x); });
  } else {
    included = true;
  }

  return excluded || !included;
}
// got the nodes from receive data
int explodeConf(std::string filepath, const std::string &custom_port,
                bool sslibev, bool ssrlibev, std::vector<nodeInfo> &nodes) {
  std::ifstream infile;
  std::stringstream contentstrm;
  infile.open(filepath);

  contentstrm << infile.rdbuf();
  infile.close();

  return explodeConfContent(contentstrm.str(), custom_port, sslibev, ssrlibev,
                            nodes);
}

int explodeConfContent(const std::string &content,
                       const std::string &custom_port, bool sslibev,
                       bool ssrlibev, std::vector<nodeInfo> &nodes) {
  int filetype = -1;

  // Prefer subscription-style detection: contains multiple different schemes
  bool has_vmess = regFind(content, "(?i)\\bvmess(1)?://");
  bool has_vless = regFind(content, "(?i)\\bvless://");
  bool has_ss = regFind(content, "(?i)\\bss://");
  bool has_ssr = regFind(content, "(?i)\\bssr://");
  bool has_trojan = regFind(content, "(?i)\\btrojan://");
  bool has_socks = regFind(content, "(?i)\\bsocks://");
  int scheme_types = (int)has_vmess + (int)has_vless + (int)has_ss +
                     (int)has_ssr + (int)has_trojan + (int)has_socks;

  if (scheme_types >= 1) {
    // Try explicit subscription parsing first
    explodeSub(content, sslibev, ssrlibev, custom_port, nodes);
    if (nodes.size() > 0) {
      return SPEEDTEST_ERROR_NONE;
    }
    // Fallback to legacy detectors if nothing parsed
  }

  if (strFind(content, "\"version\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDSS;
  else if (strFind(content, "\"serverSubscribes\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDSSR;
  else if (strFind(content, "\"vmess\"") && strFind(content, "vnext"))
    filetype = SPEEDTEST_MESSAGE_FOUNDVMESS;
  else if (strFind(content, "\"vless\"") && strFind(content, "\"outbound\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDVLESS;
  else if (strFind(content, "\"proxy_apps\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDSSCONF;
  else if (strFind(content, "\"idInUse\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDSSTAP;
  else if (strFind(content, "\"local_address\"") &&
           strFind(content, "\"local_port\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDSSR; // use ssr config parser
  else if (strFind(content, "\"ModeFileNameType\""))
    filetype = SPEEDTEST_MESSAGE_FOUNDNETCH;

  switch (filetype) {
  case SPEEDTEST_MESSAGE_FOUNDSS:
    explodeSSConf(content, custom_port, sslibev, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSSR:
    explodeSSRConf(content, custom_port, sslibev, ssrlibev, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDVMESS:
    explodeVmessConf(content, custom_port, sslibev, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDVLESS:
    explodeVlessConf(content, custom_port, sslibev, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSSCONF:
    explodeSSAndroid(content, sslibev, custom_port, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSSTAP:
    explodeSSTap(content, custom_port, nodes, sslibev, ssrlibev);
    break;
  case SPEEDTEST_MESSAGE_FOUNDNETCH:
    explodeNetchConf(content, sslibev, ssrlibev, custom_port, nodes);
    break;
  case SPEEDTEST_MESSAGE_FOUNDSUB:
    // 显式按订阅解析
    explodeSub(content, sslibev, ssrlibev, custom_port, nodes);
    break;
  default:
    // 兜底：尝试作为本地订阅解析（包含 Base64 订阅、纯链接行、Surge/Clash 等）
    explodeSub(content, sslibev, ssrlibev, custom_port, nodes);
  }

  if (nodes.size() == 0)
    return SPEEDTEST_ERROR_UNRECOGFILE;
  else
    return SPEEDTEST_ERROR_NONE;
}

void explode(const std::string &link, bool sslibev, bool ssrlibev,
             const std::string &custom_port, nodeInfo &node) {
  // TODO: replace strFind with startsWith if appropriate
  if (regFind(link, "(?i)^ssr://"))
    explodeSSR(link, sslibev, ssrlibev, custom_port, node);
  else if (regFind(link, "(?i)^(vmess|vmess1)://"))
    explodeVmess(link, custom_port, node);
  else if (regFind(link, "(?i)^vless://"))
    explodeVless(link, custom_port, node);
  else if (regFind(link, "(?i)^ss://"))
    explodeSS(link, sslibev, custom_port, node);
  else if (regFind(link, "(?i)^socks://") ||
           regFind(link, "(?i)^https://t\\.me/socks") ||
           regFind(link, "(?i)^tg://socks"))
    explodeSocks(link, custom_port, node);
  else if (regFind(link, "(?i)^https://t\\.me/http") ||
           regFind(link, "(?i)^tg://http")) // telegram style http link
    explodeHTTP(link, custom_port, node);
  else if (regFind(link, "(?i)^Netch://"))
    explodeNetch(link, sslibev, ssrlibev, custom_port, node);
  else if (regFind(link, "(?i)^trojan://"))
    explodeTrojan(link, custom_port, node);
  else if (isLink(link))
    explodeHTTPSub(link, custom_port, node);
}

void explodeSub(std::string sub, bool sslibev, bool ssrlibev,
                const std::string &custom_port, std::vector<nodeInfo> &nodes) {
  std::stringstream strstream;
  std::string strLink;
  bool processed = false;
  nodeInfo node;

  // try to parse as SSD configuration
  if (regFind(sub, "(?i)^ssd://")) {
    explodeSSD(sub, sslibev, custom_port, nodes);
    processed = true;
  }

  // try to parse as clash configuration
  try {
    if (!processed && regFind(sub, "\"?(Proxy|proxies)\"?:")) {
      regGetMatch(sub, R"(^(?:Proxy|proxies):$\s(?:(?:^ +?.*$| *?-.*$|)\s?)+)",
                  1, &sub);
      Node yamlnode = Load(sub);
      if (yamlnode.size() &&
          (yamlnode["Proxy"].IsDefined() || yamlnode["proxies"].IsDefined())) {
        explodeClash(yamlnode, custom_port, nodes, sslibev, ssrlibev);
        processed = true;
      }
    }
  } catch (std::exception &e) {
    // ignore
  }

  // try to parse as surge configuration
  if (!processed && explodeSurge(sub, custom_port, nodes, sslibev)) {
    processed = true;
  }

  // try to parse as normal subscription
  if (!processed) {
    // 仅在“看起来像
    // Base64”且原文不含协议头的情况下尝试解码，并对解码结果做协议/格式校验
    std::string t = trim(sub);
    bool contains_scheme =
        regFind(t, "(?i)(ssr|vmess|vmess1|vless|ss|trojan|socks|http)://");
    bool looks_b64_chars = regFind(t, "^[A-Za-z0-9\\-_/+=\\r\\n]+$");

    if (!contains_scheme && looks_b64_chars) {
      std::string decoded = urlsafe_base64_decode(t);
      bool decoded_contains_scheme = regFind(
          decoded, "(?i)(ssr|vmess|vmess1|vless|ss|trojan|socks|http)://");
      bool decoded_is_surge_style =
          regFind(decoded, "(vmess|vless|shadowsocks|http|trojan)\\s*?=");
      if (decoded_contains_scheme || decoded_is_surge_style) {
        sub = decoded; // 仅当解码结果符合预期协议/格式时采用
      } else {
        sub = t; // 否则保留原文，视为非 Base64
      }
    } else {
      sub = t; // 已有协议头或字符集不符，直接视为非 Base64
    }

    if (regFind(sub, "(vmess|vless|shadowsocks|http|trojan)\\s*?=")) {
      if (explodeSurge(sub, custom_port, nodes, sslibev))
        return;
    }

    // 将订阅内容按行拆分
    std::vector<std::string> lines;
    lines.reserve(sub.size() / 48 + 8); // 粗略预估，减少realloc
    {
      std::string cur;
      cur.reserve(256);
      for (char ch : sub) {
        if (ch == '\n') {
          if (!cur.empty() && cur.back() == '\r')
            cur.pop_back();
          lines.emplace_back(std::move(cur));
          cur.clear();
          cur.reserve(256);
        } else {
          cur.push_back(ch);
        }
      }
      if (!cur.empty()) {
        if (!cur.empty() && cur.back() == '\r')
          cur.pop_back();
        lines.emplace_back(std::move(cur));
      }
    }

    // 大订阅并行解析，小订阅保持顺序解析
    const int threshold_cfg =
        (parse_parallel_threshold > 0) ? parse_parallel_threshold : 512;
    if (lines.size() >= static_cast<size_t>(threshold_cfg)) {
      const unsigned int hw = std::thread::hardware_concurrency();
      int worker_cfg = (parse_worker_count > 0)
                           ? parse_worker_count
                           : (hw ? static_cast<int>(hw) * 2 : 8);
      const size_t worker_count = static_cast<size_t>(std::max(2, worker_cfg));
      std::atomic<size_t> next_idx{0};

      // 每线程本地缓存，避免对 nodes 的锁竞争
      std::vector<std::vector<std::pair<int, nodeInfo>>> buckets(worker_count);
      auto worker = [&](size_t tid) {
        std::vector<std::pair<int, nodeInfo>> local;
        local.reserve(256);
        while (true) {
          size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
          if (i >= lines.size())
            break;
          const std::string &strLinkLocal = lines[i];
          if (strLinkLocal.empty())
            continue;

          nodeInfo n;
          n.linkType = -1;
          try {
            explode(strLinkLocal, sslibev, ssrlibev, custom_port, n);
          } catch (const std::exception &e) {
            writeLog(LOG_TYPE_ERROR,
                     std::string("explode() exception: ") + e.what() +
                         " while parsing link: " + strLinkLocal);
            continue;
          } catch (...) {
            writeLog(LOG_TYPE_ERROR,
                     "explode() unknown exception while parsing link: " +
                         strLinkLocal);
            continue;
          }
          if (n.linkType == -1)
            continue;

          local.emplace_back(static_cast<int>(i), std::move(n));
        }
        buckets[tid] = std::move(local);
      };

      // 启动线程
      std::vector<std::thread> threads;
      threads.reserve(worker_count);
      for (size_t t = 0; t < worker_count; ++t) {
        threads.emplace_back(worker, t);
      }
      for (auto &th : threads)
        th.join();

      // 合并并按原始行号排序，确保输出顺序稳定
      size_t total = 0;
      for (auto &b : buckets)
        total += b.size();
      std::vector<std::pair<int, nodeInfo>> merged;
      merged.reserve(total);
      for (auto &b : buckets) {
        if (!b.empty()) {
          merged.insert(merged.end(), std::make_move_iterator(b.begin()),
                        std::make_move_iterator(b.end()));
        }
      }
      std::sort(merged.begin(), merged.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });

      // 一次性追加到 nodes
      nodes.reserve(nodes.size() + merged.size());
      for (auto &p : merged) {
        nodes.emplace_back(std::move(p.second));
      }
    } else {
      // 串行解析（小订阅避免线程开销）
      std::stringstream ss;
      ss.clear();
      ss.str(std::string());
      ss << sub;
      while (std::getline(ss, strLink, '\n')) {
        if (!strLink.empty() && strLink.back() == '\r')
          strLink.pop_back();

        nodeInfo n;
        n.linkType = -1;
        try {
          explode(strLink, sslibev, ssrlibev, custom_port, n);
        } catch (const std::exception &e) {
          writeLog(LOG_TYPE_ERROR, std::string("explode() exception: ") +
                                       e.what() +
                                       " while parsing link: " + strLink);
          continue;
        } catch (...) {
          writeLog(LOG_TYPE_ERROR,
                   "explode() unknown exception while parsing link: " +
                       strLink);
          continue;
        }

        if (strLink.size() == 0 || n.linkType == -1) {
          continue;
        }
        nodes.emplace_back(std::move(n)); // got the right formed node content
      }
    }
  }
}

void filterNodes(std::vector<nodeInfo> &nodes, string_array &exclude_remarks,
                 string_array &include_remarks, int groupID) {
  int node_index = 0;
  std::vector<nodeInfo>::iterator iter = nodes.begin();

  std::vector<std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>>
      exclude_patterns, include_patterns;
  unsigned int i = 0;
  PCRE2_SIZE erroroffset;
  int errornumber, rc;

  // Compile exclude patterns
  for (i = 0; i < exclude_remarks.size(); i++) {
    std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> pattern(
        pcre2_compile(
            reinterpret_cast<const unsigned char *>(exclude_remarks[i].c_str()),
            exclude_remarks[i].size(),
            PCRE2_UTF | PCRE2_MULTILINE | PCRE2_ALT_BSUX, &errornumber,
            &erroroffset, NULL),
        &pcre2_code_free);
    if (!pattern)
      return;
    exclude_patterns.emplace_back(std::move(pattern));
    pcre2_jit_compile(exclude_patterns.back().get(), 0);
  }
  // Compile include patterns
  for (i = 0; i < include_remarks.size(); i++) {
    std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> pattern(
        pcre2_compile(
            reinterpret_cast<const unsigned char *>(include_remarks[i].c_str()),
            include_remarks[i].size(),
            PCRE2_UTF | PCRE2_MULTILINE | PCRE2_ALT_BSUX, &errornumber,
            &erroroffset, NULL),
        &pcre2_code_free);
    if (!pattern)
      return;
    include_patterns.emplace_back(std::move(pattern));
    pcre2_jit_compile(include_patterns.back().get(), 0);
  }

  writeLog(LOG_TYPE_INFO, "Filter started.");

  // Parallel filtering for large lists
  const size_t parallel_threshold = 512;
  if (nodes.size() >= parallel_threshold) {
    const unsigned int hw = std::thread::hardware_concurrency();
    int worker_cfg = (parse_worker_count > 0)
                           ? parse_worker_count
                           : (hw ? static_cast<int>(hw) * 2 : 8);
    const size_t worker_count =
        static_cast<size_t>(std::max(2, worker_cfg));
    std::atomic<size_t> next_idx{0};

    // Result bitmap: 1 = keep, 0 = drop
    std::vector<char> keep(nodes.size(), 0);

    auto worker = [&](size_t /*tid*/) {
      // Prepare thread-local match_data for each pattern
      std::vector<
          std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>>
          ex_md, in_md;
      ex_md.reserve(exclude_patterns.size());
      in_md.reserve(include_patterns.size());
      for (auto &pc : exclude_patterns) {
        ex_md.emplace_back(
            std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>(
                pcre2_match_data_create_from_pattern(pc.get(), NULL),
                &pcre2_match_data_free));
      }
      for (auto &pc : include_patterns) {
        in_md.emplace_back(
            std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>(
                pcre2_match_data_create_from_pattern(pc.get(), NULL),
                &pcre2_match_data_free));
      }

      while (true) {
        size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= nodes.size())
          break;
        const std::string &remarks = nodes[idx].remarks;

        bool excluded = false;
        bool included = include_patterns.empty();
        int rc_local = 0;

        // Exclude matching
        for (size_t j = 0; j < exclude_patterns.size(); ++j) {
          rc_local = pcre2_match(
              exclude_patterns[j].get(),
              reinterpret_cast<const unsigned char *>(remarks.c_str()),
              remarks.size(), 0, 0, ex_md[j].get(), NULL);
          if (rc_local >= 0) {
            excluded = true;
            break;
          }
        }

        // Include matching (if any include patterns provided)
        if (!excluded && !included) {
          for (size_t j = 0; j < include_patterns.size(); ++j) {
            rc_local = pcre2_match(
                include_patterns[j].get(),
                reinterpret_cast<const unsigned char *>(remarks.c_str()),
                remarks.size(), 0, 0, in_md[j].get(), NULL);
            if (rc_local >= 0) {
              included = true;
              break;
            }
          }
        }

        keep[idx] = (!excluded && included) ? 1 : 0;
      }
    };

    // Launch workers
    std::vector<std::thread> threads;
    threads.reserve(worker_count);
    for (size_t t = 0; t < worker_count; ++t) {
      threads.emplace_back(worker, t);
    }
    for (auto &th : threads)
      th.join();

    // Merge and log
    std::vector<nodeInfo> filtered;
    filtered.reserve(nodes.size());
    node_index = 0;
    for (size_t idx = 0; idx < nodes.size(); ++idx) {
      if (keep[idx]) {
        writeLog(LOG_TYPE_INFO, "Node  " + nodes[idx].group + " - " +
                                    nodes[idx].remarks + "  has been added.");
        nodes[idx].id = node_index;
        nodes[idx].groupID = groupID;
        ++node_index;
        filtered.emplace_back(std::move(nodes[idx]));
      } else {
        writeLog(LOG_TYPE_INFO,
                 "Node  " + nodes[idx].group + " - " + nodes[idx].remarks +
                     "  has been ignored and will not be added.");
      }
    }
    nodes.swap(filtered);
  } else {
    // Sequential path for small lists
    while (iter != nodes.end()) {
      bool excluded = false, included = false;

      // Exclude patterns
      for (i = 0; i < exclude_patterns.size(); i++) {
        std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> md(
            pcre2_match_data_create_from_pattern(exclude_patterns[i].get(),
                                                 NULL),
            &pcre2_match_data_free);
        rc = pcre2_match(
            exclude_patterns[i].get(),
            reinterpret_cast<const unsigned char *>(iter->remarks.c_str()),
            iter->remarks.size(), 0, 0, md.get(), NULL);
        if (rc >= 0) {
          excluded = true;
          break;
        }
      }

      // Include patterns (if provided)
      if (!excluded) {
        if (include_patterns.size() > 0) {
          for (i = 0; i < include_patterns.size(); i++) {
            std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>
                md(pcre2_match_data_create_from_pattern(
                       include_patterns[i].get(), NULL),
                   &pcre2_match_data_free);
            rc = pcre2_match(
                include_patterns[i].get(),
                reinterpret_cast<const unsigned char *>(iter->remarks.c_str()),
                iter->remarks.size(), 0, 0, md.get(), NULL);
            if (rc >= 0) {
              included = true;
              break;
            }
          }
        } else {
          included = true;
        }
      }

      if (excluded || !included) {
        writeLog(LOG_TYPE_INFO,
                 "Node  " + iter->group + " - " + iter->remarks +
                     "  has been ignored and will not be added.");
        iter = nodes.erase(iter);
      } else {
        writeLog(LOG_TYPE_INFO, "Node  " + iter->group + " - " + iter->remarks +
                                    "  has been added.");
        iter->id = node_index;
        iter->groupID = groupID;
        ++node_index;
        ++iter;
      }
    }
  }

  writeLog(LOG_TYPE_INFO, "Filter done.");
}

unsigned long long streamToInt(const std::string &stream) {
  if (!stream.size())
    return 0;
  double streamval = 1.0;
  std::vector<std::string> units = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  size_t index = units.size();
  do {
    index--;
    if (endsWith(stream, units[index])) {
      streamval =
          std::pow(1024, index) *
          to_number<float>(
              stream.substr(0, stream.size() - units[index].size()), 0.0);
      break;
    }
  } while (index != 0);
  return (unsigned long long)streamval;
}

static inline double percentToDouble(const std::string &percent) {
  return stof(percent.substr(0, percent.size() - 1)) / 100.0;
}

time_t dateStringToTimestamp(std::string date) {
  time_t rawtime;
  time(&rawtime);
  if (startsWith(date, "left=")) {
    time_t seconds_left = 0;
    date.erase(0, 5);
    if (endsWith(date, "d")) {
      date.erase(date.size() - 1);
      seconds_left = to_number<double>(date, 0.0) * 86400.0;
    }
    return rawtime + seconds_left;
  } else {
    struct tm *expire_time;
    std::vector<std::string> date_array = split(date, ":");
    if (date_array.size() != 6)
      return 0;

    expire_time = localtime(&rawtime);
    expire_time->tm_year = to_int(date_array[0], 1900) - 1900;
    expire_time->tm_mon = to_int(date_array[1], 1) - 1;
    expire_time->tm_mday = to_int(date_array[2]);
    expire_time->tm_hour = to_int(date_array[3]);
    expire_time->tm_min = to_int(date_array[4]);
    expire_time->tm_sec = to_int(date_array[5]);
    return mktime(expire_time);
  }
}

bool getSubInfoFromHeader(const std::string &header, std::string &result) {
  std::string pattern = R"(^(?i:Subscription-UserInfo): (.*?)\s*?$)", retStr;
  if (regFind(header, pattern)) {
    regGetMatch(header, pattern, 2, 0, &retStr);
    if (retStr.size()) {
      result = retStr;
      return true;
    }
  }
  return false;
}

bool getSubInfoFromNodes(const std::vector<nodeInfo> &nodes,
                         const string_array &stream_rules,
                         const string_array &time_rules, std::string &result) {
  std::string remarks, pattern, target, stream_info, time_info, retStr;
  string_size spos;

  for (const nodeInfo &x : nodes) {
    remarks = x.remarks;
    if (!stream_info.size()) {
      for (const std::string &y : stream_rules) {
        spos = y.rfind("|");
        if (spos == y.npos)
          continue;
        pattern = y.substr(0, spos);
        target = y.substr(spos + 1);
        if (regMatch(remarks, pattern)) {
          retStr = regReplace(remarks, pattern, target);
          if (retStr != remarks) {
            stream_info = retStr;
            break;
          }
        } else
          continue;
      }
    }

    remarks = x.remarks;
    if (!time_info.size()) {
      for (const std::string &y : time_rules) {
        spos = y.rfind("|");
        if (spos == y.npos)
          continue;
        pattern = y.substr(0, spos);
        target = y.substr(spos + 1);
        if (regMatch(remarks, pattern)) {
          retStr = regReplace(remarks, pattern, target);
          if (retStr != remarks) {
            time_info = retStr;
            break;
          }
        } else
          continue;
      }
    }

    if (stream_info.size() && time_info.size())
      break;
  }

  if (!stream_info.size() && !time_info.size())
    return false;

  // calculate how much stream left
  unsigned long long total = 0, left, used = 0, expire = 0;
  std::string total_str = getUrlArg(stream_info, "total"),
              left_str = getUrlArg(stream_info, "left"),
              used_str = getUrlArg(stream_info, "used");
  if (strFind(total_str, "%")) {
    if (used_str.size()) {
      used = streamToInt(used_str);
      total = used / (1 - percentToDouble(total_str));
    } else if (left_str.size()) {
      left = streamToInt(left_str);
      total = left / percentToDouble(total_str);
      used = total - left;
    }
  } else {
    total = streamToInt(total_str);
    if (used_str.size()) {
      used = streamToInt(used_str);
    } else if (left_str.size()) {
      left = streamToInt(left_str);
      used = total - left;
    }
  }

  result = "upload=0; download=" + std::to_string(used) +
           "; total=" + std::to_string(total) + ";";

  // calculate expire time
  expire = dateStringToTimestamp(time_info);
  if (expire)
    result += " expire=" + std::to_string(expire) + ";";

  return true;
}

bool getSubInfoFromSSD(const std::string &sub, std::string &result) {
  // 空串防护
  if (sub.empty())
    return false;

  rapidjson::Document json;
  std::string decoded = urlsafe_base64_decode(regReplace(sub, "^ssd://", ""));
  json.Parse(decoded.c_str());
  if (json.HasParseError())
    return false;

  std::string used_str = GetMember(json, "traffic_used"),
              total_str = GetMember(json, "traffic_total"),
              expire_str = GetMember(json, "expiry");
  if (!used_str.size() || !total_str.size())
    return false;
  unsigned long long used = stod(used_str) * std::pow(1024, 3),
                     total = stod(total_str) * std::pow(1024, 3), expire;
  result = "upload=0; download=" + std::to_string(used) +
           "; total=" + std::to_string(total) + ";";

  expire = dateStringToTimestamp(
      regReplace(expire_str, "(\\d+)-(\\d+)-(\\d+) (.*)", "$1:$2:$3:$4"));
  if (expire)
    result += " expire=" + std::to_string(expire) + ";";

  return true;
}
