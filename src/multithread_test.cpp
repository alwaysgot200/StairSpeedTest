#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "logger.h"
#include "misc.h"
#include "nodeinfo.h"
#include "printout.h"
#include "socket.h"
#include "webget.h"

using namespace std::chrono;

std::queue<SOCKET> opened_socket;

#define MAX_FILE_SIZE 512 * 1024 * 1024
// for use of site ping
const int times_to_ping = 10, fail_limit = 2;

// for use of multi-thread socket test
typedef std::lock_guard<std::mutex> guarded_mutex;
std::mutex opened_socket_mutex;
std::atomic_ullong received_bytes = 0;
std::atomic_int launched = 0, still_running = 0;
std::atomic_bool EXIT_FLAG = false;

void push_socket(const SOCKET &s) {
  guarded_mutex guard(opened_socket_mutex);
  opened_socket.push(s);
}

static inline void draw_progress_dl(int progress, int this_bytes) {
  std::cerr << "\r[";
  for (int j = 0; j < progress; j++) {
    std::cerr << "=";
  }
  if (progress < 20)
    std::cerr << ">";
  else
    std::cerr << "]";
  std::cerr << " " << progress * 5 << "% " << speedCalc(this_bytes);
}

static inline void draw_progress_icon(int progress) {
  std::cerr << "\r ";
  switch (progress % 4) {
  case 1:
    std::cerr << "\\";
    break;
  case 2:
    std::cerr << "|";
    break;
  case 3:
    std::cerr << "/";
    break;
  default:
    std::cerr << "-";
    break;
  }
}

static inline void draw_progress_ul(int progress, int this_bytes) {
  draw_progress_icon(progress);
  std::cerr << " " << speedCalc(this_bytes);
}

static inline void draw_progress_gping(int progress, int *values) {
  std::cerr << "\r[";
  for (int i = 0; i <= progress; i++) {
    std::cerr << (values[i] == 0 ? "*" : "-");
  }
  if (progress == times_to_ping - 1) {
    std::cerr << "]";
  }
  std::cerr << " " << progress + 1 << "/" << times_to_ping << " "
            << values[progress] << "ms";
}

static void SSL_Library_init() {
  static bool init = false;
  if (!init)
    init = true;
  else
    return;
  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_algorithms();
}

int _thread_download(std::string host, int port, std::string uri,
                     std::string localaddr, int localport, std::string username,
                     std::string password, bool useTLS /* = false */,
                     int node_id) {
  launched++;
  still_running++;
  set_breadcrumb(node_id, "filedl_thread:start"); // 新增：线程启动
  defer(still_running--;) char bufRecv[BUF_SIZE];
  int retVal, cur_len /*, recv_len = 0*/;
  SOCKET sHost;
  std::string request = "GET " + uri +
                        " HTTP/1.1\r\n"
                        "Host: " +
                        host +
                        "\r\n"
                        "Connection: close\r\n"
                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/72.0.3626.121 Safari/537.36\r\n\r\n";

  sHost = initSocket(getNetworkType(localaddr), SOCK_STREAM, IPPROTO_TCP);
  if (INVALID_SOCKET == sHost) {
    set_breadcrumb(node_id, "filedl_thread:socket_init_fail"); // 新增
    writeLog(LOG_TYPE_FILEDL, "Socket init failed.");
    return -1;
  }
  set_breadcrumb(node_id, "filedl_thread:socket_init_ok"); // 新增
  push_socket(sHost);
  // defer(closesocket(sHost);) // close socket in main thread
  setTimeout(sHost, 5000);
  if (startConnect(sHost, localaddr, localport) == SOCKET_ERROR) {
    set_breadcrumb(node_id, "filedl_thread:startConnect_fail"); // 新增
    writeLog(LOG_TYPE_FILEDL, "startConnect failed.");
    return -1;
  } else {
    set_breadcrumb(node_id, "filedl_thread:startConnect_ok"); // 新增
  }
  if (connectSocks5(sHost, username, password) == -1) {
    set_breadcrumb(node_id, "filedl_thread:socks5_fail"); // 新增
    writeLog(LOG_TYPE_FILEDL, "Socks5 auth/connect failed.");
    return -1;
  } else {
    set_breadcrumb(node_id, "filedl_thread:socks5_ok"); // 新增
  }
  if (connectThruSocks(sHost, host, port) == -1) {
    set_breadcrumb(node_id, "filedl_thread:socks_tunnel_fail"); // 新增
    writeLog(LOG_TYPE_FILEDL, "Connect thru socks failed.");
    return -1;
  } else {
    set_breadcrumb(node_id, "filedl_thread:socks_tunnel_ok"); // 新增
  }

  bool first_data = false; // 新增：首次数据标记
  if (useTLS) {
    SSL_CTX *ctx;
    SSL *ssl;

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
      set_breadcrumb(node_id, "filedl_thread:ssl_ctx_new_fail"); // 新增
      ERR_print_errors_fp(stderr);
      return -1;
    }
    defer(SSL_CTX_free(ctx);)

        ssl = SSL_new(ctx);
    defer(SSL_free(ssl);) SSL_set_fd(ssl, sHost);

    // New: set SNI and auto-retry for TLS
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

    if (SSL_connect(ssl) != 1) {
      set_breadcrumb(node_id, "filedl_thread:ssl_connect_fail"); // 新增
      ERR_print_errors_fp(stderr);
    } else {
      set_breadcrumb(node_id, "filedl_thread:ssl_connect_ok"); // 新增
      retVal = SSL_write(ssl, request.data(), request.size());
      if (retVal == SOCKET_ERROR) {
        set_breadcrumb(node_id, "filedl_thread:ssl_write_fail"); // 新增
        return -1;
      }
      while (1) {
        cur_len = SSL_read(ssl, bufRecv, BUF_SIZE - 1);
        if (cur_len < 0) {
          if (errno == EWOULDBLOCK || errno == EAGAIN) {
            continue;
          } else {
            break;
          }
        }
        if (cur_len == 0)
          break;
        received_bytes += cur_len;
        if (!first_data) {
          first_data = true;
          set_breadcrumb(node_id,
                         "filedl_thread:first_data"); // 新增：首次收到数据
        }
        if (EXIT_FLAG)
          break;
      }
    }
  } else {
    retVal = Send(sHost, request.data(), request.size(), 0);
    if (SOCKET_ERROR == retVal) {
      set_breadcrumb(node_id, "filedl_thread:send_fail"); // 新增
      return -1;
    }
    set_breadcrumb(node_id, "filedl_thread:send_ok"); // 新增
    while (1) {
      cur_len = Recv(sHost, bufRecv, BUF_SIZE - 1, 0);
      if (cur_len < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
          continue;
        } else {
          break;
        }
      }
      if (cur_len == 0)
        break;
      received_bytes += cur_len;
      if (!first_data) {
        first_data = true;
        set_breadcrumb(node_id,
                       "filedl_thread:first_data"); // 新增：首次收到数据
      }
      if (EXIT_FLAG)
        break;
    }
  }
  set_breadcrumb(node_id, "filedl_thread:exit_ok"); // 新增：线程自然退出
  return 0;
}

int _thread_upload(std::string host, int port, std::string uri,
                   std::string localaddr, int localport, std::string username,
                   std::string password, bool useTLS /* = false */) {
  launched++;
  still_running++;
  defer(still_running--;) int retVal, cur_len;
  SOCKET sHost;
  std::string request = "POST " + uri +
                        " HTTP/1.1\r\n"
                        "Connection: close\r\n"
                        "Content-Length: 134217728\r\n"
                        "Host: " +
                        host + "\r\n\r\n";
  std::string post_data;

  sHost = initSocket(getNetworkType(localaddr), SOCK_STREAM, IPPROTO_TCP);
  if (INVALID_SOCKET == sHost)
    return -1;
  push_socket(sHost);
  // defer(closesocket(sHost);) // close socket on main thread
  setTimeout(sHost, 5000);
  if (startConnect(sHost, localaddr, localport) == SOCKET_ERROR ||
      connectSocks5(sHost, username, password) == -1 ||
      connectThruSocks(sHost, host, port) == -1)
    return -1;

  if (useTLS) {
    SSL_CTX *ctx;
    SSL *ssl;

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
      ERR_print_errors_fp(stderr);
      return -1;
    }
    defer(SSL_CTX_free(ctx);)

        ssl = SSL_new(ctx);
    defer(SSL_free(ssl);) SSL_set_fd(ssl, sHost);

    // New: set SNI and auto-retry for TLS
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

    if (SSL_connect(ssl) != 1) {
      ERR_print_errors_fp(stderr);
    } else {
      SSL_write(ssl, request.data(), request.size());
      while (1) {
        post_data = rand_str(128);
        cur_len = SSL_write(ssl, post_data.data(), post_data.size());
        if (cur_len == SOCKET_ERROR) {
          break;
        }
        received_bytes += cur_len;
        if (EXIT_FLAG)
          break;
      }
    }
  } else {
    retVal = Send(sHost, request.data(), request.size(), 0);
    if (SOCKET_ERROR == retVal)
      return -1;
    while (1) {
      post_data = rand_str(128);
      cur_len = Send(sHost, post_data.data(), post_data.size(), 0);
      if (cur_len == SOCKET_ERROR) {
        break;
      }
      received_bytes += cur_len;
      if (EXIT_FLAG)
        break;
    }
  }
  return 0;
}

struct thread_args {
  std::string host;
  int port = 0;
  std::string uri;
  std::string localaddr;
  int localport;
  std::string username;
  std::string password;
  bool useTLS = false;
  int node_id = -1; // 新增：用于面包屑定位具体节点
};

void *_thread_download_caller(void *arg) {
  thread_args *args = (thread_args *)arg;
  set_breadcrumb(args->node_id, "filedl_thread:caller"); // 新增：进入 caller
  _thread_download(args->host, args->port, args->uri, args->localaddr,
                   args->localport, args->username, args->password,
                   args->useTLS, args->node_id); // 新增：透传 node_id
  return 0;
}

void *_thread_upload_caller(void *arg) {
  thread_args *args = (thread_args *)arg;
  _thread_upload(args->host, args->port, args->uri, args->localaddr,
                 args->localport, args->username, args->password, args->useTLS);
  return 0;
}

int _thread_upload_curl(nodeInfo *node, std::string url, std::string proxy) {
  launched++;
  still_running++;
  // long retVal = webPost(url, rand_str(8388608), proxy);
  // node->ulSpeed = speedCalc(retVal * 1.0);
  still_running--;
  return 0;
}

int perform_test(nodeInfo &node, std::string localaddr, int localport,
                 std::string username, std::string password, int thread_count) {
  writeLog(LOG_TYPE_FILEDL, "Multi-thread download test started.");
  set_breadcrumb(node, "filedl:begin"); // 新增
  // prep up vars first
  std::string host, uri, testfile = node.testFile;
  int port = 0, i;
  bool useTLS = false;

  writeLog(LOG_TYPE_FILEDL, "Fetch target: " + testfile);
  urlParse(testfile, host, uri, port, useTLS);
  received_bytes = 0;
  EXIT_FLAG = false;

  if (useTLS) {
    writeLog(LOG_TYPE_FILEDL, "Found HTTPS URL. Initializing OpenSSL library.");
    SSL_Library_init();
  } else {
    writeLog(LOG_TYPE_FILEDL, "Found HTTP URL.");
  }
  thread_args args = {host,     port,     uri,    localaddr, localport,
                      username, password, useTLS, node.id}; // 新增 node.id

  std::vector<pthread_t> threads(thread_count);
  launched = 0;
  for (i = 0; i != thread_count; i++) {
    writeLog(LOG_TYPE_FILEDL,
             "Starting up thread #" + std::to_string(i + 1) + ".");

    pthread_create(&threads[i], NULL, _thread_download_caller, &args);
  }
  while (!launched)
    sleep(20); // wait until any one of the threads start up

  set_breadcrumb(node, "filedl:threads_launched"); // 新增
  writeLog(LOG_TYPE_FILEDL, "All threads launched. Start accumulating data.");
  auto start = steady_clock::now();
  unsigned long long transferred_bytes = 0, last_bytes = 0, this_bytes = 0,
                     cur_recv_bytes = 0, max_speed = 0;

  // 新增：下载测试的可调限制（后续可改为从 ini 读取）
  static const int DL_INTERVAL_MS = 500; // 采样间隔
  static const int DL_TICKS = 10;        // 最多采样次数（10*0.5s≈5s）
  static const unsigned long long DL_MAX_TOTAL_BYTES =
      2ULL * 1024 * 1024;                   // 达到上限即停（~2MiB）
  static const int DL_STABLE_WINDOW = 4;    // 连续 N 点用来判定“稳定”
  static const double DL_STABLE_TOL = 0.08; // 稳定阈值：±8%
  static const int DL_EARLY_ABORT_TICKS =
      6; // 低速早停的最小采样数（6*0.5s≈3s）
  static const unsigned long long DL_EARLY_ABORT_BYTES =
      64ULL * 1024; // 低速早停的累计阈值

  // 计算最多可写入 rawSpeed 的槽数，避免越界
  int max_slots = sizeof(node.rawSpeed) / sizeof(node.rawSpeed[0]);
  int tick_limit = DL_TICKS > max_slots ? max_slots : DL_TICKS;

  // 简单的滑动窗口（固定数组）用于“速度稳定提前停”判断
  unsigned long long recent[8] = {0};
  int recent_size = 0;

  for (i = 1; i <= tick_limit; i++) {
    sleep(DL_INTERVAL_MS); // 按设定间隔采样

    cur_recv_bytes = received_bytes;
    unsigned long long delta_bytes = cur_recv_bytes - transferred_bytes;
    // 统一换算为“每秒字节数”
    this_bytes = (delta_bytes * 1000) / DL_INTERVAL_MS;
    transferred_bytes = cur_recv_bytes;

    node.rawSpeed[i - 1] = this_bytes;
    if (i % 2 == 0) {
      max_speed = std::max(max_speed, (this_bytes + last_bytes) /
                                          2); // 两点平均再取最大，降低抖动
    } else {
      last_bytes = this_bytes;
    }

    int running = still_running;
    writeLog(
        LOG_TYPE_FILEDL,
        "Running threads: " + std::to_string(running) +
            ", total received bytes: " + std::to_string(transferred_bytes) +
            ", current received bytes: " + std::to_string(this_bytes) + ".");
    if (!running) {
      set_breadcrumb(node,
                     "filedl:no_running_threads"); // 新增：线程已全部退出
      break;
    }
    draw_progress_dl(i, static_cast<int>(this_bytes));

    // 1) 达到总流量上限，提前结束
    if (DL_MAX_TOTAL_BYTES > 0 && transferred_bytes >= DL_MAX_TOTAL_BYTES) {
      set_breadcrumb(node, "filedl:early_stop:max_total_bytes"); // 新增
      writeLog(LOG_TYPE_FILEDL, "Early stop: reached max total bytes limit.");
      break;
    }

    // 2) 低速早停：一定时间仍几乎无进展
    if (i >= DL_EARLY_ABORT_TICKS && transferred_bytes < DL_EARLY_ABORT_BYTES) {
      set_breadcrumb(node, "filedl:early_stop:too_slow"); // 新增
      writeLog(LOG_TYPE_FILEDL, "Early stop: too slow (low total bytes).");
      break;
    }

    // 3) 速度稳定提前停：最近 N 个点波动在阈值内
    if (DL_STABLE_WINDOW > 0) {
      if (recent_size < DL_STABLE_WINDOW) {
        recent[recent_size++] = this_bytes;
      } else {
        // 左移一位，丢弃最老，加入最新
        for (int k = 1; k < DL_STABLE_WINDOW; ++k) {
          recent[k - 1] = recent[k];
        }
        recent[DL_STABLE_WINDOW - 1] = this_bytes;
      }
      if (recent_size == DL_STABLE_WINDOW) {
        unsigned long long vmin = recent[0], vmax = recent[0];
        for (int k = 1; k < DL_STABLE_WINDOW; ++k) {
          if (recent[k] < vmin)
            vmin = recent[k];
          if (recent[k] > vmax)
            vmax = recent[k];
        }
        // 以 vmax 作为尺度，波动范围不超过 8% 视为稳定
        if (vmax > 0 && (vmax - vmin) <= static_cast<unsigned long long>(
                                             vmax * DL_STABLE_TOL)) {
          set_breadcrumb(node, "filedl:early_stop:stabilized"); // 新增
          writeLog(LOG_TYPE_FILEDL, "Early stop: speed stabilized.");
          break;
        }
      }
    }
  }

  std::cerr << std::endl;
  writeLog(LOG_TYPE_FILEDL, "Test completed. Terminate all threads.");
  EXIT_FLAG = true;              // terminate all threads right now
  while (!opened_socket.empty()) // close all sockets
  {
    shutdown(opened_socket.front(), SD_BOTH);
    closesocket(opened_socket.front());
    opened_socket.pop();
  }
  cur_recv_bytes = received_bytes; // save current received byte
  auto end = steady_clock::now();
  auto duration = duration_cast<milliseconds>(end - start);
  int deltatime = duration.count() + 1; // add 1 to prevent some error
  node.totalRecvBytes = cur_recv_bytes;
  node.avgSpeed = speedCalc(cur_recv_bytes * 1000.0 / deltatime);
  node.maxSpeed = speedCalc(max_speed);
  if (node.avgSpeed == "0.00B") {
    node.avgSpeed = "N/A";
    node.maxSpeed = "N/A";
  }
  // writeLog(LOG_TYPE_FILEDL, "Downloaded " + std::to_string(received_bytes) +
  // " bytes in " + std::to_string(deltatime) + " milliseconds.");
  writeLog(LOG_TYPE_FILEDL, "Downloaded " + std::to_string(cur_recv_bytes) +
                                " bytes in " + std::to_string(deltatime) +
                                " milliseconds.");
  for (int i = 0; i < thread_count; i++) {
    pthread_cancel(threads[i]);
    pthread_join(threads[i], NULL);
    writeLog(LOG_TYPE_FILEDL,
             "Thread #" + std::to_string(i + 1) + " has exited.");
  }
  set_breadcrumb(node, "filedl:completed"); // 新增：下载测试完成
  writeLog(LOG_TYPE_FILEDL, "Multi-thread download test completed.");
  return 0;
}

int upload_test(nodeInfo &node, std::string localaddr, int localport,
                std::string username, std::string password) {
  writeLog(LOG_TYPE_FILEUL, "Upload test started.");
  // prep up vars first
  std::string host, uri, testfile = node.ulTarget;
  int port = 0, i, running;
  bool useTLS = false;

  writeLog(LOG_TYPE_FILEUL, "Upload destination: " + testfile);
  urlParse(testfile, host, uri, port, useTLS);
  received_bytes = 0;
  EXIT_FLAG = false;

  if (useTLS) {
    writeLog(LOG_TYPE_FILEUL, "Found HTTPS URL. Initializing OpenSSL library.");
    SSL_Library_init();
  } else {
    writeLog(LOG_TYPE_FILEUL, "Found HTTP URL.");
  }

  // std::thread workers[2];
  pthread_t workers[2];
  thread_args args = {host,      port,     uri,      localaddr,
                      localport, username, password, useTLS};
  launched = 0;
  for (i = 0; i < 1; i++) {
    writeLog(LOG_TYPE_FILEUL,
             "Starting up worker thread #" + std::to_string(i + 1) + ".");
    // workers[i] = std::thread(_thread_upload, host, port, uri, localaddr,
    // localport, username, password, useTLS);
    pthread_create(&workers[i], NULL, _thread_upload_caller, &args);
  }
  while (!launched)
    sleep(20); // wait until worker thread starts up

  writeLog(LOG_TYPE_FILEUL,
           "Worker threads launched. Start accumulating data.");
  auto start = steady_clock::now();
  unsigned long long transferred_bytes = 0, this_bytes = 0, cur_sent_bytes = 0;
  for (i = 1; i < 11; i++) {
    sleep(1000); // accumulate data
    cur_sent_bytes = received_bytes;
    this_bytes = cur_sent_bytes - transferred_bytes;
    transferred_bytes = cur_sent_bytes;
    sleep(1); // slow down to prevent some problem
    running = still_running;
    writeLog(LOG_TYPE_FILEUL,
             "Running worker threads: " + std::to_string(running) +
                 ", total sent bytes: " + std::to_string(transferred_bytes) +
                 ", current sent bytes: " + std::to_string(this_bytes) + ".");
    if (!running)
      break;
    draw_progress_ul(i, this_bytes);
  }
  std::cerr << std::endl;
  writeLog(LOG_TYPE_FILEUL, "Test completed. Terminate worker threads.");
  EXIT_FLAG = true;              // terminate worker thread right now
  while (!opened_socket.empty()) // close all sockets
  {
    shutdown(opened_socket.front(), SD_BOTH);
    closesocket(opened_socket.front());
    opened_socket.pop();
  }
  this_bytes = received_bytes; // save current uploaded data
  auto end = steady_clock::now();
  auto duration = duration_cast<milliseconds>(end - start);
  int deltatime = duration.count() + 1; // add 1 to prevent some problem
  sleep(5);                             // slow down to prevent some problem
  node.ulSpeed = speedCalc(this_bytes * 1000.0 / deltatime);
  if (node.ulSpeed == "0.00B") {
    node.ulSpeed = "N/A";
  }
  writeLog(LOG_TYPE_FILEUL, "Uploaded " + std::to_string(this_bytes) +
                                " bytes in " + std::to_string(deltatime) +
                                " milliseconds.");
  node.totalRecvBytes += this_bytes;
  /*
  for(auto &x : workers)
      if(x.joinable())
          x.join();//wait until worker thread has exited
  */
  for (int i = 0; i < 1; i++) {
    /*
#ifdef _WIN32
    pthread_kill(workers[i], SIGINT);
#else
    pthread_kill(workers[i], SIGUSR1);
#endif // _WIN32
    */
    pthread_cancel(workers[i]);
    pthread_join(workers[i], NULL);
    writeLog(LOG_TYPE_FILEUL,
             "Thread #" + std::to_string(i + 1) + " has exited.");
  }
  writeLog(LOG_TYPE_FILEUL, "Upload test completed.");
  return 0;
}

int upload_test_curl(nodeInfo &node, std::string localaddr, int localport,
                     std::string username, std::string password) {
  writeLog(LOG_TYPE_FILEUL, "Upload test started.");
  std::string url = node.ulTarget;
  std::string proxy =
      buildSocks5ProxyString(localaddr, localport, username, password);
  writeLog(LOG_TYPE_FILEUL, "Starting up worker thread.");
  launched = 0;
  std::thread worker = std::thread(_thread_upload_curl, &node, url, proxy);
  while (!launched)
    sleep(20); // wait until worker thread starts up

  writeLog(LOG_TYPE_FILEUL, "Worker thread launched. Wait for it to exit.");
  int progress = 0;
  while (true) {
    sleep(200);
    if (!still_running)
      break;
    draw_progress_icon(progress);
    progress++;
  }
  std::cerr << std::endl;

  writeLog(LOG_TYPE_FILEUL, "Reported upload speed: " + node.ulSpeed);
  /*
  if(worker.joinable())
      worker.join();//wait until worker thread has exited
  */
  /// don't for threads to exit, killing the client will make connect/send/recv
  /// fail and stop
  writeLog(LOG_TYPE_FILEUL, "Upload test completed.");
  return 0;
}

int sitePing(nodeInfo &node, std::string localaddr, int localport,
             std::string username, std::string password, std::string target) {
  char bufRecv[BUF_SIZE];
  int retVal, cur_len;
  SOCKET sHost;
  std::string host, uri;
  int port = 0, rawSitePing[10] = {};
  bool useTLS = false;
  urlParse(target, host, uri, port, useTLS);
  std::string request = "GET " + uri +
                        " HTTP/1.1\r\n"
                        "Host: " +
                        host +
                        "\r\n"
                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/72.0.3626.121 Safari/537.36\r\n\r\n";

  writeLog(LOG_TYPE_GPING, "Website ping started. Target: '" + target +
                               "' . Proxy: '" + localaddr + ":" +
                               std::to_string(localport) + "' .");
  int loopcounter = 0, succeedcounter = 0, failcounter = 0, totduration = 0;
  bool failed = true;
  while (loopcounter < times_to_ping) {
    failed = true;
    rawSitePing[loopcounter] = 0;
    if (failcounter >= fail_limit) {
      writeLog(LOG_TYPE_GPING, "Fail limit exceeded. Stop now.");
      break;
    }
    defer(loopcounter++;)
        //// clang-format off / on
        defer(draw_progress_gping(loopcounter, rawSitePing);)
            time_point<steady_clock>
                start = steady_clock::now(),
                end;
    milliseconds lapse;
    int deltatime = 0;
    defer(
        end = steady_clock::now();
        lapse = duration_cast<milliseconds>(end - start);
        deltatime = lapse.count(); if (failed) {
          failcounter++;
          writeLog(LOG_TYPE_GPING, "Accessing '" + target + "' - Fail - time=" +
                                       std::to_string(deltatime) + "ms");
        } else {
          succeedcounter++;
          rawSitePing[loopcounter] = deltatime;
          totduration += deltatime;
          writeLog(LOG_TYPE_GPING, "Accessing '" + target +
                                       "' - Success - time=" +
                                       std::to_string(deltatime) + "ms");
        }) sHost =
        initSocket(getNetworkType(localaddr), SOCK_STREAM, IPPROTO_TCP);
    if (INVALID_SOCKET == sHost) {
      writeLog(LOG_TYPE_GPING, "ERROR: Could not create socket.");
      continue;
    }
    defer(closesocket(sHost);) if (startConnect(sHost, localaddr, localport) ==
                                   SOCKET_ERROR) {
      writeLog(LOG_TYPE_GPING, "ERROR: Connect to SOCKS5 server " + localaddr +
                                   ":" + std::to_string(localport) +
                                   " failed.");
      continue;
    }
    setTimeout(sHost, 2000);
    if (connectSocks5(sHost, username, password) == -1) {
      writeLog(LOG_TYPE_GPING, "ERROR: SOCKS5 server authentication failed.");
      continue;
    }
    if (connectThruSocks(sHost, host, port) == -1) {
      writeLog(LOG_TYPE_GPING, "ERROR: Connect to " + host + ":" +
                                   std::to_string(port) +
                                   " through SOCKS5 server failed.");
      continue;
    }

    if (useTLS) {
      SSL_CTX *ctx;
      SSL *ssl;

      ctx = SSL_CTX_new(TLS_client_method());
      if (ctx == NULL)
        writeLog(LOG_TYPE_GPING, "OpenSSL: " + std::string(ERR_error_string(
                                                   ERR_get_error(), NULL)));
      else {
        defer(SSL_CTX_free(ctx);) ssl = SSL_new(ctx);
        defer(SSL_free(ssl);) SSL_set_fd(ssl, sHost);

        // 为 HTTPS 目标设置 SNI，使用解析得到的 host
        if (!SSL_set_tlsext_host_name(ssl, host.c_str())) {
          writeLog(LOG_TYPE_GPING,
                   "OpenSSL: Failed to set SNI for host: " + host);
        }

        if (SSL_connect(ssl) != 1)
          writeLog(LOG_TYPE_GPING, "ERROR: Connect to " + host + ":" +
                                       std::to_string(port) +
                                       " through SOCKS5 server failed.");
        else {
          SSL_write(ssl, request.data(), request.size());
          cur_len = SSL_read(ssl, bufRecv, BUF_SIZE - 1);
          failed = cur_len <= 0;
        }
      }
    } else {
      retVal = Send(sHost, request.data(), request.size(), 0);
      if (SOCKET_ERROR == retVal)
        writeLog(LOG_TYPE_GPING, "ERROR: Connect to " + host + ":" +
                                     std::to_string(port) +
                                     " through SOCKS5 server failed.");
      cur_len = Recv(sHost, bufRecv, BUF_SIZE - 1, 0);
      failed = cur_len <= 0;
    }
  }
  std::cerr << std::endl;
  std::move(std::begin(rawSitePing), std::end(rawSitePing), node.rawSitePing);
  float pingval = 0.0;
  if (succeedcounter > 0)
    pingval = totduration * 1.0 / succeedcounter;
  char strtmp[16] = {};
  snprintf(strtmp, sizeof(strtmp), "%0.2f", pingval);
  node.sitePing.assign(strtmp);
  writeLog(LOG_TYPE_GPING, "Ping statistics of target " + target + " : " +
                               std::to_string(loopcounter) + " probes sent, " +
                               std::to_string(succeedcounter) +
                               " successful, " + std::to_string(failcounter) +
                               " failed. ");
  writeLog(LOG_TYPE_GPING, "Website ping completed. Leaving.");
  return SPEEDTEST_MESSAGE_GOTGPING;
}
