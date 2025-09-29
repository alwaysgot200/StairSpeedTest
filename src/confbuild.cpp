#include <numeric>
#include <string>
#include <vector>

#include "ini_reader.h"
#include "misc.h"
#include "nodeinfo.h"

extern int socksport;

std::string base_ss_win =
    R"({"version":"4.1.6","configs":[?config?],"strategy":null,"index":0,"global":false,"enabled":false,"shareOverLan":true,"isDefault":false,"localPort":?localport?,"portableMode":true,"pacUrl":null,"useOnlinePac":false,"secureLocalPac":true,"availabilityStatistics":false,"autoCheckUpdate":true,"checkPreRelease":false,"isVerboseLogging":false,"logViewer":{"topMost":false,"wrapText":false,"toolbarShown":false,"Font":"Consolas, 8pt","BackgroundColor":"Black","TextColor":"White"},"proxy":{"useProxy":false,"proxyType":0,"proxyServer":"","proxyPort":0,"proxyTimeout":3,"useAuth":false,"authUser":"","authPwd":""},"hotkey":{"SwitchSystemProxy":"","SwitchSystemProxyMode":"","SwitchAllowLan":"","ShowLogs":"","ServerMoveUp":"","ServerMoveDown":"","RegHotkeysAtStartup":false}})";
std::string config_ss_win =
    R"({"server":"?server?","server_port":?port?,"password":"?password?","method":"?method?","plugin":"?plugin?","plugin_opts":"?plugin_opts?","plugin_args":"","remarks":"?remarks?","timeout":5})";
std::string config_ss_libev =
    R"({"server":"?server?","server_port":?port?,"password":"?password?","method":"?method?","plugin":"?plugin?","plugin_opts":"?plugin_opts?","plugin_args":"","local_address":"127.0.0.1","local_port":?localport?,"reuse_port":true})";
std::string base_ssr_win =
    R"({"configs":[?config?],"index":0,"random":true,"sysProxyMode":1,"shareOverLan":false,"localPort":?localport?,"localAuthPassword":null,"localDnsServer":"","dnsServer":"","reconnectTimes":2,"balanceAlgorithm":"LowException","randomInGroup":false,"TTL":0,"connectTimeout":5,"proxyRuleMode":2,"proxyEnable":false,"pacDirectGoProxy":false,"proxyType":0,"proxyHost":null,"proxyPort":0,"proxyAuthUser":null,"proxyAuthPass":null,"proxyUserAgent":null,"authUser":null,"authPass":null,"autoBan":false,"checkSwitchAutoCloseAll":false,"logEnable":false,"sameHostForSameTarget":false,"keepVisitTime":180,"isHideTips":false,"nodeFeedAutoUpdate":true,"serverSubscribes":[],"token":{},"portMap":{}})";
std::string config_ssr_win =
    R"({"remarks":"?remarks?","id":"18C4949EBCFE46687AE4A7645725D35F","server":"?server?","server_port":?port?,"server_udp_port":0,"password":"?password?","method":"?method?","protocol":"?protocol?","protocolparam":"?protoparam?","obfs":"?obfs?","obfsparam":"?obfsparam?","remarks_base64":"?remarks_base64?","group":"?group?","enable":true,"udp_over_tcp":false})";
std::string config_ssr_libev =
    R"({"server":"?server?","server_port":?port?,"protocol":"?protocol?","method":"?method?","obfs":"?obfs?","password":"?password?","obfs_param":"?obfsparam?","protocol_param":"?protoparam?","local_address":"127.0.0.1","local_port":?localport?,"reuse_port":true})";
// 顶部常量定义处（更新 base_vmess，加入 "grpcSettings": ?grpcset?）
std::string base_vmess =
    R"({"log":{"access":"logs/v2ray-access.log","error":"logs/v2ray-error.log","loglevel":"debug"},"inbounds":[{"port":?localport?,"listen":"127.0.0.1","protocol":"socks","settings":{"udp":true}}],"outbounds":[{"tag":"proxy","protocol":"vmess","settings":{"vnext":[{"address":"?add?","port":?port?,"users":[{"id":"?id?","alterId":?aid?,"email":"t@t.tt","security":"?cipher?"}]}]},"streamSettings":{"network":"?net?","security":"?tls?","tlsSettings":?tlsset?,"tcpSettings":?tcpset?,"wsSettings":?wsset?,"kcpSettings":?kcpset?,"httpSettings":?h2set?,"quicSettings":?quicset?,"grpcSettings":?grpcset?},"mux":{"enabled":false}}],"routing":{"domainStrategy":"IPIfNonMatch"}})";
std::string wsset_vmess =
    R"({"connectionReuse":true,"path":"?path?","headers":{"Host":"?host?"?edge?}})";
std::string tcpset_vmess =
    R"({"connectionReuse":true,"header":{"type":"?type?","request":{"version":"1.1","method":"GET","path":["?path?"],"headers":{"Host":["?host?"],"User-Agent":["Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/55.0.2883.75 Safari/537.36","Mozilla/5.0 (iPhone; CPU iPhone OS 10_0_2 like Mac OS X) AppleWebKit/601.1 (KHTML, like Gecko) CriOS/53.0.2785.109 Mobile/14A456 Safari/601.1.46"],"Accept-Encoding":["gzip, deflate"],"Connection":["keep-alive"],"Pragma":"no-cache"}}}})";
std::string tlsset_vmess =
    R"({"serverName":"?serverName?","allowInsecure":?verify?,"allowInsecureCiphers":true})";
std::string kcpset_vmess =
    R"({"mtu":1350,"tti":50,"uplinkCapacity":12,"downlinkCapacity":100,"congestion":false,"readBufferSize":2,"writeBufferSize":2,"header":{"type":"?type?"}})";
std::string h2set_vmess = R"({"path":"?path?","host":[?host?]})";
std::string quicset_vmess =
    R"({"security":"?host?","key":"?path?","header":{"type":"?type?"}})";
std::string base_trojan =
    R"({"run_type":"client","local_addr":"127.0.0.1","local_port":?localport?,"remote_addr":"?server?","remote_port":?port?,"password":["?password?"],"ssl":{"verify":?verify?,"verify_hostname":?verifyhost?,"sni":"?host?"},"tcp":{"reuse_port":true}})";
std::string base_vless =
    R"({ "log": {"access":"logs/v2ray-access.log","error":"logs/v2ray-error.log","loglevel":"debug"}, "inbounds": [ { "listen": "127.0.0.1", "port": ?localport?, "protocol": "socks", "settings": { "auth": "noauth", "udp": true }, "sniffing": { "enabled": true, "destOverride": ["http","tls"], "routeOnly": true } } ], "outbounds": [ { "protocol": "vless", "settings": { "vnext": [ { "address": "?add?", "port": ?port?, "users": [ { "id": "?id?", "encryption": "none", "flow": "?flow?" } ] } ] }, "streamSettings": { "network": "?net?", "security": "?security?", "tlsSettings": ?tlsset?, "realitySettings": ?realset?, "tcpSettings": ?tcpset?, "wsSettings": ?wsset?, "kcpSettings": ?kcpset?, "quicSettings": ?quicset?, "httpSettings": ?h2set?, "grpcSettings": ?grpcset? }, "mux": ?mux? } ], "routing": { "domainStrategy": "IPIfNonMatch" } })";
std::string tlsset_vless =
    R"({"serverName":"?serverName?","allowInsecure":?verify?,"allowInsecureCiphers":true,"alpn":?alpn?,"fingerprint":"?fp?"})";
std::string xh2set_vless = R"({"path":"?path?","host":[?host?]})";
std::string realset_vless =
    R"({"serverName":"?serverName?","publicKey":"?pbk?","shortId":"?sid?","fingerprint":"?fp?","alpn":?alpn?,"spiderX":"/"})";
std::string quicset_vless =
    R"({"security":"?host?","key":"?path?","header":{"type":"?type?"}})";
std::string base_ss_v2ray =
    R"({"log":{"loglevel":"warning"},"inbounds":[{"port":?localport?,"listen":"127.0.0.1","protocol":"socks","settings":{"udp":true,"auth":"noauth"}}],"outbounds":[{"tag":"proxy","protocol":"shadowsocks","settings":{"servers":[{"address":"?server?","port":?port?,"method":"?method?","password":"?password?","level":0}]}}],"routing":{"domainStrategy":"AsIs"}})";

std::string base_trojan_v2ray =
    R"({"log":{"loglevel":"warning"},"inbounds":[{"port":?localport?,"listen":"127.0.0.1","protocol":"socks","settings":{"udp":true,"auth":"noauth"}}],"outbounds":[{"tag":"proxy","protocol":"trojan","settings":{"servers":[{"address":"?server?","port":?port?,"password":"?password?","level":0}]},"streamSettings":{"network":"tcp","security":"tls","tlsSettings":{"serverName":"?sni?","allowInsecure":?allowInsecure?}}}],"routing":{"domainStrategy":"AsIs"}})";

int explodeLog(const std::string &log, std::vector<nodeInfo> &nodes) {
  INIReader ini;
  std::vector<std::string> nodeList, vArray;
  std::string strTemp;
  nodeInfo node;

  // 空串防护：来自外部输入时，空输入直接返回错误
  if (log.empty())
    return -1;

  if (!startsWith(log, "[Basic]"))
    return -1;

  ini.Parse(log);

  if (!ini.SectionExist("Basic") || !ini.ItemExist("Basic", "GenerationTime") ||
      !ini.ItemExist("Basic", "Tester"))
    return -1;

  nodeList = ini.GetSections();
  node.proxyStr = "LOG";
  for (auto &x : nodeList) {
    if (x == "Basic")
      continue;
    ini.EnterSection(x);
    vArray = split(x, "^");
    node.group = vArray[0];
    node.remarks = vArray[1];
    node.avgPing = ini.Get("AvgPing");
    node.avgSpeed = ini.Get("AvgSpeed");
    node.groupID = ini.GetNumber<int>("GroupID");
    node.id = ini.GetNumber<int>("ID");
    node.maxSpeed = ini.Get("MaxSpeed");
    node.online = ini.GetBool("Online");
    node.pkLoss = ini.Get("PkLoss");
    ini.GetNumberArray<int>("RawPing", ",", node.rawPing);
    ini.GetNumberArray<int>("RawSitePing", ",", node.rawSitePing);
    ini.GetNumberArray<unsigned long long>("RawSpeed", ",", node.rawSpeed);
    node.sitePing = ini.Get("SitePing");
    node.totalRecvBytes = ini.GetNumber<unsigned long long>("UsedTraffic");
    node.ulSpeed = ini.Get("ULSpeed");
    nodes.push_back(node);
  }

  return 0;
}

std::string replace_first(std::string str, const std::string &old_value,
                          const std::string &new_value) {
  string_size pos = str.find(old_value);
  if (pos == str.npos)
    return str;
  return str.replace(pos, old_value.size(), new_value);
}

// 通用占位符替换：支持 "?key?" / "? key ?" / "?   key   ?" 等形式
static void replace_placeholder_all(std::string &s, const std::string &key,
                                    const std::string &value) {
  size_t pos = 0;
  while (true) {
    size_t q1 = s.find('?', pos);
    if (q1 == std::string::npos)
      break;
    size_t q2 = s.find('?', q1 + 1);
    if (q2 == std::string::npos)
      break;

    // 取出 ?...?
    std::string inside = s.substr(q1 + 1, q2 - q1 - 1);
    if (trim(inside) == key) {
      s.replace(q1, q2 - q1 + 1, value);
      pos = q1 + value.size();
    } else {
      pos = q1 + 1;
    }
  }
}

// 同一位置可能存在两种误拼写/别名的兼容占位符，批量替换
static void replace_placeholder_all_either(std::string &s,
                                           const std::string &keyA,
                                           const std::string &keyB,
                                           const std::string &value) {
  replace_placeholder_all(s, keyA, value);
  replace_placeholder_all(s, keyB, value);
}
std::string vmessConstruct(const std::string &group, const std::string &remarks,
                           const std::string &add, const std::string &port,
                           const std::string &type, const std::string &id,
                           const std::string &aid, const std::string &net,
                           const std::string &cipher, const std::string &path,
                           const std::string &host, const std::string &edge,
                           const std::string &tls, tribool udp, tribool tfo,
                           tribool scv, tribool tls13) {
  std::string base = base_vmess;
  base = replace_first(base, "?localport?", std::to_string(socksport));
  base = replace_first(base, "?add?", add);
  base = replace_first(base, "?port?", port);
  base = replace_first(base, "?id?", id);
  base = replace_first(base, "?aid?", aid.empty() ? "0" : aid);
  base = replace_first(base, "?net?", net.empty() ? "tcp" : net);
  base = replace_first(base, "?cipher?", cipher);
  switch (hash_(net)) {
  case "ws"_hash: {
    std::string wsset = wsset_vmess;
    wsset = replace_first(
        wsset, "?host?",
        (host.empty() && !isIPv4(add) && !isIPv6(add)) ? add : trim(host));
    wsset = replace_first(wsset, "?path?", path.empty() ? "/" : path);
    wsset = replace_first(wsset, "?edge?",
                          edge.empty() ? "" : ",\"Edge\":\"" + edge + "\"");
    base = replace_first(base, "?wsset?", wsset);
    break;
  }
  case "kcp"_hash: {
    std::string kcpset = kcpset_vmess;
    kcpset = replace_first(kcpset, "?type?", type);
    base = replace_first(base, "?kcpset?", kcpset);
    break;
  }
  case "h2"_hash:
  case "http"_hash: {
    std::string h2set = h2set_vmess;
    h2set = replace_first(h2set, "?path?", path);

    // 兼容 host 为空：若 add 非 IP 且 host 为空，则用 add；否则允许为空（输出
    // []）
    std::string effectiveHost = trim(host);
    if (effectiveHost.empty() && !isIPv4(add) && !isIPv6(add)) {
      effectiveHost = add;
    }

    std::string hostList;
    if (effectiveHost.empty()) {
      // 空列表 -> "host":[]
      hostList = "";
    } else {
      // 支持逗号分隔的多个 host
      auto hostItems = split(effectiveHost, ",");
      if (hostItems.empty()) {
        hostList = "";
      } else {
        hostList =
            std::accumulate(std::next(hostItems.begin()), hostItems.end(),
                            std::string("\"") + hostItems[0] + "\"",
                            [](auto before, auto current) {
                              return before + ",\"" + current + "\"";
                            });
      }
    }

    h2set = replace_first(h2set, "?host?", hostList);
    base = replace_first(base, "?h2set?", h2set);
    break;
  }
  case "quic"_hash: {
    std::string quicset = quicset_vmess;
    quicset = replace_first(quicset, "?host?", host);
    quicset = replace_first(quicset, "?path?", path);
    quicset = replace_first(quicset, "?type?", type);
    base = replace_first(base, "?quicset?", quicset);
    break;
  }
  case "grpc"_hash: { // 新增：仅保留 serviceName，不输出 multiMode
    std::string grpcset = R"({"serviceName":"?service?"})";
    grpcset = replace_first(grpcset, "?service?", path);
    base = replace_first(base, "?grpcset?", grpcset);
    break;
  }
  case "tcp"_hash:
    break;
  }
  if (type == "http") {
    std::string tcpset = tcpset_vmess;
    tcpset = replace_first(
        tcpset, "?host?",
        (host.empty() && !isIPv4(add) && !isIPv6(add)) ? add : trim(host));
    tcpset = replace_first(tcpset, "?type?", type);
    tcpset = replace_first(tcpset, "?path?", path.empty() ? "/" : path);
    base = replace_first(base, "?tcpset?", tcpset);
  }
  if (host.size()) {
    std::string tlsset = tlsset_vmess;
    tlsset = replace_first(tlsset, "?serverName?", host);
    scv.define(true);
    tlsset = replace_first(tlsset, "?verify?", scv ? "true" : "false");
    base = replace_first(base, "?tlsset?", tlsset);
  }

  // 规范 security 值为 tls/none，避免出现空串等非法值
  {
    std::string tlsv = toLower(trim(tls)) == "tls" ? "tls" : "none";
    base = replace_first(base, "?tls?", tlsv);
  }
  base = replace_first(base, "?tcpset?", "null");
  base = replace_first(base, "?wsset?", "null");
  base = replace_first(base, "?tlsset?", "null");
  base = replace_first(base, "?kcpset?", "null");
  base = replace_first(base, "?h2set?", "null");
  base = replace_first(base, "?quicset?", "null");
  base = replace_first(base, "?grpcset?", "null"); // 新增：收尾置空

  return base;
}
// 在 vlessConvlessConstruct 之后新增：公开 API 的薄封装，统一指向实现体
std::string vlessConstruct(const std::string &add, const std::string &port,
                           const std::string &type, const std::string &id,
                           const std::string &net, const std::string &path,
                           const std::string &host, const std::string &edge,
                           const std::string &tls, tribool scv,
                           const std::string &alpn, const std::string &fp,
                           const std::string &flow, const std::string &pbk,
                           const std::string &sid,
                           const std::string &mux_json) {
  std::string base = base_vless;

  // 1) 基本替换
  replace_placeholder_all(base, "localport", std::to_string(socksport));
  replace_placeholder_all(base, "add", add);
  replace_placeholder_all(base, "port", port);
  replace_placeholder_all(base, "id", id);
  replace_placeholder_all(base, "flow", flow); // 初填 flow，稍后按条件清理

  // 2) network
  std::string netv = net.empty() ? "tcp" : toLower(trim(net));
  // 兼容 xhttp/httpupgrade：走 httpSettings 分支
  if (netv == "xhttp" || netv == "httpupgrade")
    netv = "http";
  replace_placeholder_all(base, "net", netv);

  // 3) 安全层：none/tls/reality
  const std::string tl = toLower(trim(tls));
  const bool use_tls = (tl == "tls");
  const bool use_reality = (tl == "reality");
  replace_placeholder_all_either(base, "reality", "security",
                                 use_reality ? "reality"
                                             : (use_tls ? "tls" : "none"));

  // 4) 传输设置
  if (hash_(netv) == "ws"_hash) {
    std::string wsset = wsset_vmess;
    wsset = replace_first(wsset, "?host?",
                          (trim(host).empty() && !isIPv4(add) && !isIPv6(add))
                              ? add
                              : trim(host));
    wsset = replace_first(wsset, "?path?", path.empty() ? "/" : path);
    wsset = replace_first(wsset, "?edge?", "");
    replace_placeholder_all(base, "wsset", wsset);
  } else if (hash_(netv) == "kcp"_hash) {
    std::string kcpset = kcpset_vmess;
    kcpset = replace_first(kcpset, "?type?", type.empty() ? "none" : type);
    replace_placeholder_all(base, "kcpset", kcpset);
  } else if (hash_(netv) == "h2"_hash || hash_(netv) == "http"_hash) {
    std::string h2set = h2set_vmess;
    h2set = replace_first(h2set, "?path?", path.empty() ? "/" : path);
    // host 数组
    std::string hostJoined;
    if (!trim(host).empty()) {
      string_array hosts = split(host, ",");
      hostJoined = "\"" + trim(hosts[0]) + "\"";
      for (size_t i = 1; i < hosts.size(); ++i)
        hostJoined += ",\"" + trim(hosts[i]) + "\"";
    } else {
      hostJoined =
          (!isIPv4(add) && !isIPv6(add)) ? ("\"" + add + "\"") : "\"\"";
    }
    h2set = replace_first(h2set, "?host?", hostJoined);
    replace_placeholder_all(base, "h2set", h2set);
  } else if (hash_(netv) == "quic"_hash) {
    std::string quicset = quicset_vless;
    quicset = replace_first(quicset, "?host?", host);
    quicset = replace_first(quicset, "?path?", path);
    quicset = replace_first(quicset, "?type?", type.empty() ? "none" : type);
    replace_placeholder_all(base, "quicset", quicset);
  }
  if (toLower(trim(type)) == "http") {
    std::string tcpset = tcpset_vmess;
    tcpset = replace_first(tcpset, "?host?",
                           (trim(host).empty() && !isIPv4(add) && !isIPv6(add))
                               ? add
                               : trim(host));
    tcpset = replace_first(tcpset, "?type?", "http");
    tcpset = replace_first(tcpset, "?path?", path.empty() ? "/" : path);
    replace_placeholder_all(base, "tcpset", tcpset);
  }
  if (hash_(netv) == "grpc"_hash) {
    // 移除 multiMode，仅保留 serviceName
    std::string grpcset = R"({"serviceName":"?service?"})";
    grpcset = replace_first(grpcset, "?service?", path);
    replace_placeholder_all_either(base, "gprcset", "grpcset", grpcset);
  }

  // 5) alpn 组装为 JSON 数组
  auto make_alpn_json = [&](const std::string &in) -> std::string {
    std::string a = trim(in);
    if (a.empty())
      return "null";
    string_array toks = split(a, ",");
    std::string j = "[\"" + trim(toks[0]) + "\"";
    for (size_t i = 1; i < toks.size(); ++i)
      j += ",\"" + trim(toks[i]) + "\"";
    j += "]";
    return j;
  };
  std::string alpn_json = make_alpn_json(alpn);

  // 6) TLS/Reality
  // SNI：edge(=URL sni) > host > add(域名)
  std::string sni =
      !trim(edge).empty()
          ? trim(edge)
          : (!trim(host).empty() ? trim(host)
                                 : ((!isIPv4(add) && !isIPv6(add)) ? add : ""));
  if (use_reality) {
    std::string realset = realset_vless;
    realset = replace_first(realset, "?serverName?", sni);
    realset = replace_first(realset, "?pbk?", pbk);
    realset = replace_first(realset, "?sid?", sid);
    realset = replace_first(realset, "?fp?", fp);
    realset = replace_first(realset, "?alpn?", alpn_json);
    replace_placeholder_all(base, "realset", realset);
    // reality 下不需要 tlsSettings（占位符收尾阶段会置 null）
  } else if (use_tls) {
    std::string tlsset = tlsset_vless;
    tlsset = replace_first(tlsset, "?serverName?", sni);
    scv.define(true);
    tlsset = replace_first(tlsset, "?verify?", scv ? "true" : "false");
    tlsset = replace_first(tlsset, "?alpn?", alpn_json);
    tlsset = replace_first(tlsset, "?fp?", fp);
    replace_placeholder_all(base, "tlsset", tlsset);
  }

  // 7) 清理模板里可能遗留的 "tls" 等误占位
  replace_placeholder_all(base, "tls", "null");

  // 8) mux：外部传空 => 写入 null；否则写入对象
  replace_placeholder_all(base, "mux", mux_json.empty() ? "null" : mux_json);

  // 9) flow 清理：仅 Reality+Vision 保留，其余情况移除 "flow" 键
  {
    const bool flow_empty = trim(flow).empty();
    const bool keep_flow = use_reality && !flow_empty;
    if (!keep_flow) {
      base = regReplace(base, ",\\s*\\\"flow\\\"\\s*:\\s*\\\"[^\\\"]*\\\"", "");
    }
  }

  // 10) 收尾：仅将“仍未被填充的占位符”置为 null
  replace_placeholder_all(base, "tcpset", "null");
  replace_placeholder_all(base, "wsset", "null");
  replace_placeholder_all(base, "tlsset", "null");
  replace_placeholder_all(base, "realset", "null");
  replace_placeholder_all(base, "kcpset", "null");
  replace_placeholder_all(base, "h2set", "null");
  replace_placeholder_all(base, "quicset", "null");
  replace_placeholder_all_either(base, "gprcset", "grpcset", "null");
  replace_placeholder_all(base, "xh2set", "null");

  return base;
}
std::string ssrConstruct(const std::string &group, const std::string &remarks,
                         const std::string &remarks_base64,
                         const std::string &server, const std::string &port,
                         const std::string &protocol, const std::string &method,
                         const std::string &obfs, const std::string &password,
                         const std::string &obfsparam,
                         const std::string &protoparam, bool libev, tribool udp,
                         tribool tfo, tribool scv) {
  std::string base = base_ssr_win;
  std::string config = config_ssr_win;
  std::string config_libev = config_ssr_libev;
  if (libev == true)
    config = config_libev;
  config = replace_first(config, "?group?", group);
  config = replace_first(config, "?remarks?", remarks);
  if (remarks_base64.empty())
    config = replace_first(config, "?remarks_base64?", base64_encode(remarks));
  else
    config = replace_first(config, "?remarks_base64?", remarks_base64);
  config = replace_first(config, "?server?",
                         isIPv6(server) ? "[" + server + "]" : server);
  config = replace_first(config, "?port?", port);
  config = replace_first(config, "?protocol?", protocol);
  config = replace_first(config, "?method?", method);
  config = replace_first(config, "?obfs?", obfs);
  config = replace_first(config, "?password?", password);
  config = replace_first(config, "?obfsparam?", obfsparam);
  config = replace_first(config, "?protoparam?", protoparam);
  if (libev == true)
    base = config;
  else
    base = replace_first(base, "?config?", config);
  base = replace_first(base, "?localport?", std::to_string(socksport));

  return base;
}

std::string ssConstruct(const std::string &group, const std::string &remarks,
                        const std::string &server, const std::string &port,
                        const std::string &password, const std::string &method,
                        const std::string &plugin,
                        const std::string &pluginopts, bool libev, tribool udp,
                        tribool tfo, tribool scv, tribool tls13) {
  std::string base = base_ss_win;
  std::string config = config_ss_win;
  std::string config_libev = config_ss_libev;
  if (libev == true)
    config = config_libev;
  config = replace_first(config, "?server?",
                         isIPv6(server) ? "[" + server + "]" : server);
  config = replace_first(config, "?port?", port);
  config = replace_first(config, "?password?", password);
  config = replace_first(config, "?method?", method);
  config = replace_first(
      config, "?plugin?",
      plugin.size() ? "./" + (plugin == "obfs-local" ? "simple-obfs" : plugin)
                    : "");
  config = replace_first(config, "?plugin_opts?", pluginopts);
  config = replace_first(config, "?remarks?", remarks);
  if (libev == true)
    base = config;
  else
    base = replace_first(base, "?config?", config);
  base = replace_first(base, "?localport?", std::to_string(socksport));

  return base;
}

std::string socksConstruct(const std::string &group, const std::string &remarks,
                           const std::string &server, const std::string &port,
                           const std::string &username,
                           const std::string &password, tribool udp,
                           tribool tfo, tribool scv) {
  return "user=" + username + "&pass=" + password;
}

std::string httpConstruct(const std::string &group, const std::string &remarks,
                          const std::string &server, const std::string &port,
                          const std::string &username,
                          const std::string &password, bool tls, tribool tfo,
                          tribool scv, tribool tls13) {
  return "user=" + username + "&pass=" + password;
}

std::string trojanConstruct(const std::string &group,
                            const std::string &remarks,
                            const std::string &server, const std::string &port,
                            const std::string &password,
                            const std::string &host, bool tlssecure,
                            tribool udp, tribool tfo, tribool scv,
                            tribool tls13) {
  std::string base = base_trojan;
  scv.define(true);
  base = replace_first(base, "?server?", server);
  base = replace_first(base, "?port?", port);
  base = replace_first(base, "?password?", password);
  base = replace_first(base, "?verify?", scv ? "false" : "true");
  base = replace_first(base, "?verifyhost?", scv ? "false" : "true");
  base = replace_first(base, "?host?", host);
  base = replace_first(base, "?localport?", std::to_string(socksport));
  return base;
}

std::string snellConstruct(const std::string &group, const std::string &remarks,
                           const std::string &server, const std::string &port,
                           const std::string &password, const std::string &obfs,
                           const std::string &host, tribool udp, tribool tfo,
                           tribool scv) {
  // no clients available, ignore
  return std::string();
}
std::string
ssV2RayConstruct(const std::string &group, const std::string &remarks,
                 const std::string &server, const std::string &port,
                 const std::string &password, const std::string &method,
                 const std::string &plugin, const std::string &pluginopts,
                 bool libev, tribool udp, tribool tfo, tribool scv,
                 tribool tls13) {
  std::string base = base_ss_v2ray;
  base = replace_first(base, "?localport?", std::to_string(socksport));
  base = replace_first(base, "?server?",
                       isIPv6(server) ? "[" + server + "]" : server);
  base = replace_first(base, "?port?", port);
  base = replace_first(base, "?method?", method);
  base = replace_first(base, "?password?", password);
  return base;
}

std::string
trojanV2RayConstruct(const std::string &group, const std::string &remarks,
                     const std::string &server, const std::string &port,
                     const std::string &password, const std::string &host,
                     bool tlssecure, tribool udp, tribool tfo, tribool scv,
                     tribool tls13) {
  std::string base = base_trojan_v2ray;
  std::string sni = trim(host).size()
                        ? trim(host)
                        : (!isIPv4(server) && !isIPv6(server) ? server : "");
  scv.define(true);
  base = replace_first(base, "?localport?", std::to_string(socksport));
  base = replace_first(base, "?server?", server);
  base = replace_first(base, "?port?", port);
  base = replace_first(base, "?password?", password);
  base = replace_first(base, "?sni?", sni);
  base = replace_first(base, "?allowInsecure?", scv ? "true" : "false");
  return base;
}
