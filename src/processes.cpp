#include "misc.h"
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
// #include <spawn.h>
#include <sys/wait.h>
#endif // _WIN32
#include "processes.h"
#ifndef _WIN32
typedef pid_t HANDLE;
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif // _WIN32
// Runner runner;
std::queue<HANDLE> handles;
#ifdef _WIN32
HANDLE job = 0;
#else
FILE *pPipe;
#endif // _WIN32

int chkProgram(std::string command) {
  /*
  char psBuffer[128];
  FILE *pPipe;

  if((pPipe = _popen(command.data(),"rt")) == NULL) return -1;

  // Read pipe until end of file, or an error occurs.

  while(fgets(psBuffer, 128, pPipe));//return value has no use, just dump them

  // Close pipe and print return value of pPipe.
  if(feof(pPipe))
  {
      _pclose(pPipe);
      return 0;
  }
  else
      return -2;
  */
  return 0;
}

bool runProgram(std::string command, std::string runpath, bool wait) {
#ifdef _WIN32
  try {
    BOOL retval = FALSE;
    STARTUPINFO si = {};
    si.cb = sizeof(STARTUPINFO);
    PROCESS_INFORMATION pi = {};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits = {};
    char curdir[512] = {};
    std::string path;

    job = CreateJobObject(NULL, NULL);
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (strFind(runpath, ":")) // 绝对路径
    {
      path = runpath;
    } else // 相对路径
    {
      GetCurrentDirectory(512, curdir);
      path = std::string(curdir) + "\\";
      if (runpath.size())
        path += runpath + "\\";
    }

    char *cmdstr = const_cast<char *>(command.c_str());
    char *pathstr = const_cast<char *>(path.c_str());
    retval = CreateProcess(NULL, cmdstr, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL,
                           pathstr, &si, &pi);
    if (retval == FALSE) {
      if (job) {
        CloseHandle(job);
        job = 0;
      }

      // removeInvalidNodesAndStartAgain(shard);

      return false;
    }

    // 将进程加入作业对象（保持原行为）
    AssignProcessToJobObject(job, pi.hProcess);
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_limits,
                            sizeof(job_limits));

    // 保存进程句柄到队列（保持原行为：即使 wait==true 也 push）
    handles.push(pi.hProcess);

    if (wait) {
      WaitForSingleObject(pi.hProcess, INFINITE);
      CloseHandle(pi.hProcess);
    }

    if (pi.hThread)
      CloseHandle(pi.hThread);

    // 保持原有返回语义：只以 CreateProcess 结果为准
    return retval != FALSE;
  } catch (...) {
    return false;
  }
#else
  try {
    command = command + " > /dev/null 2>&1";
    HANDLE pid;
    int status;
    switch (pid = fork()) {
    case -1: // fork 失败
      return false;
    case 0: // 子进程
    {
      setpgid(0, 0);
      if (!runpath.empty())
        chdir(runpath.c_str());
      execlp("sh", "sh", "-c", command.c_str(), (char *)NULL);
      _exit(127);
    }
    default: // 父进程
      if (wait)
        waitpid(pid, &status, 0);
      else
        handles.emplace(pid);
    }
    return true;
  } catch (...) {
    return false;
  }
#endif // _WIN32
}

void killByHandle() {
  while (!handles.empty()) {
    HANDLE hProc = handles.front();
#ifdef _WIN32
    if (hProc != NULL) {
      if (TerminateProcess(hProc, 0))
        CloseHandle(hProc);
    }
#else
    if (hProc != 0)
      kill(-hProc, SIGINT); // kill process group
#endif // _WIN32
    handles.pop();
  }
}
bool killProgram(std::string program) {
#ifdef _WIN32
  try {
    // 使用系统命令结束指定进程名及其子进程，避免 tlhelp32 兼容性问题
    std::string cmd = "taskkill /f /t /im \"" + program + "\" >nul 2>nul";
    int rc = system(cmd.c_str());
    return rc == 0;
  } catch (...) {
    return false;
  }
#else
  // if(!feof(pPipe))
  // pclose(pPipe);
  program = "pkill -f " + program;
  system(program.data());
  return true;
#endif
}

bool testV2RayConfigFile(const std::string &config_path,
                         const std::string &runpath) {
#ifdef _WIN32
  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  DWORD creationFlags = CREATE_NO_WINDOW;
  LPCSTR currentDir = runpath.empty() ? nullptr : runpath.c_str();

  auto try_start_and_wait = [&](const std::string &cmd) -> bool {
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        creationFlags, nullptr, currentDir, &si, &pi)) {
      return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (pi.hThread)
      CloseHandle(pi.hThread);
    if (pi.hProcess)
      CloseHandle(pi.hProcess);
    return code == 0;
  };

  // 依次尝试几种常见路径
  if (try_start_and_wait("tools\\clients\\v2ray.exe -test -config " +
                         config_path))
    return true;
  if (!runpath.empty()) {
    std::string cmd = runpath;
    if (!cmd.empty() && cmd.back() != '\\' && cmd.back() != '/')
      cmd += "\\";
    cmd += "v2ray.exe -test -config " + config_path;
    if (try_start_and_wait(cmd))
      return true;
  }
  if (try_start_and_wait(".\\v2ray.exe -test -config " + config_path))
    return true;
  if (try_start_and_wait(".\\tools\\clients\\v2ray.exe -test -config " +
                         config_path))
    return true;

  return false;
#else
  // 非 Windows：fork + exec，waitpid 获取退出码
  auto try_exec_wait = [&](const char *path, const char *arg0) -> bool {
    pid_t pid = fork();
    if (pid < 0)
      return false;
    if (pid == 0) {
      if (!runpath.empty())
        (void)chdir(runpath.c_str());
      execl(path, arg0, "-test", "-config", config_path.c_str(),
            (char *)nullptr);
      _exit(127);
    } else {
      int status = 0;
      waitpid(pid, &status, 0);
      return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
    }
  };
  if (try_exec_wait("v2ray", "v2ray"))
    return true;
  if (try_exec_wait("./v2ray", "v2ray"))
    return true;
  if (try_exec_wait("./base/tools/clients/v2ray/v2ray", "v2ray"))
    return true;
  return false;
#endif
}

bool testV2RayConfigStdin(const std::string &config,
                          const std::string &runpath) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = nullptr;

  HANDLE hRead = nullptr;
  HANDLE hWrite = nullptr;

  if (!CreatePipe(&hRead, &hWrite, &saAttr, 0)) {
    return false;
  }
  SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hRead;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  DWORD creationFlags = CREATE_NO_WINDOW;
  LPCSTR currentDir = runpath.empty() ? nullptr : runpath.c_str();

  auto try_start = [&](const std::string &cmd) -> bool {
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    return CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                          creationFlags, nullptr, currentDir, &si,
                          &pi) != FALSE;
  };

  bool started = false;
  if (try_start("tools\\clients\\v2ray.exe -test -config stdin:"))
    started = true;
  else if (!runpath.empty()) {
    std::string cmd = runpath;
    if (!cmd.empty() && cmd.back() != '\\' && cmd.back() != '/')
      cmd += "\\";
    cmd += "v2ray.exe -test -config stdin:";
    started = try_start(cmd);
  } else if (try_start(".\\v2ray.exe -test -config stdin:"))
    started = true;
  else if (try_start(".\\tools\\clients\\v2ray.exe -test -config stdin:"))
    started = true;

  CloseHandle(hRead);

  if (!started) {
    CloseHandle(hWrite);
    return false;
  }

  // 写入配置
  size_t total = 0;
  const char *data = config.data();
  const size_t len = config.size();
  while (total < len) {
    DWORD dwWritten = 0;
    BOOL ok = WriteFile(hWrite, data + total, static_cast<DWORD>(len - total),
                        &dwWritten, nullptr);
    if (!ok || dwWritten == 0)
      break;
    total += dwWritten;
  }
  CloseHandle(hWrite);

  // 等待退出并检查退出码
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);

  if (pi.hThread)
    CloseHandle(pi.hThread);
  if (pi.hProcess)
    CloseHandle(pi.hProcess);

  return (total == len) && (code == 0);

#else
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (!runpath.empty())
      (void)chdir(runpath.c_str());
    if (dup2(pipefd[0], STDIN_FILENO) == -1) {
      _exit(127);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    execlp("v2ray", "v2ray", "-test", "-config", "stdin:", (char *)nullptr);
    execl("./v2ray", "v2ray", "-test", "-config", "stdin:", (char *)nullptr);
    execl("./base/tools/clients/v2ray/v2ray", "v2ray", "-test", "-config",
          "stdin:", (char *)nullptr);
    _exit(127);
  } else {
    close(pipefd[0]);
    const char *buf = config.data();
    const size_t len = config.size();
    size_t total = 0;
    while (total < len) {
      ssize_t w = write(pipefd[1], buf + total, len - total);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (w == 0)
        break;
      total += static_cast<size_t>(w);
    }
    close(pipefd[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return (total == len) && WIFEXITED(status) && (WEXITSTATUS(status) == 0);
  }
#endif
}

bool runV2RayWithConfigStdin(const std::string &config_json,
                             const std::string &runpath) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = nullptr;

  HANDLE hRead = nullptr;
  HANDLE hWrite = nullptr;

  if (!CreatePipe(&hRead, &hWrite, &saAttr, 0)) {
    // fallback: write to file and run normally
    fileWrite("config.json", config_json, true);
    std::string cmd;
    if (fileExist(".\\tools\\clients\\v2ray.exe"))
      cmd = ".tools\\clients\\v2ray.exe -config config.json";
    else if (fileExist("tools\\clients\\v2ray.exe"))
      cmd = "tools\\clients\\v2ray.exe -config config.json";
    else
      cmd = "tools\\clients\\v2ray.exe -config config.json";
    return runProgram(cmd, runpath, false);
  }
  SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hRead;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  DWORD creationFlags = CREATE_NO_WINDOW;
  LPCSTR currentDir = runpath.empty() ? nullptr : runpath.c_str();

  auto try_start = [&](const std::string &cmd) -> bool {
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    return CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                          creationFlags, nullptr, currentDir, &si,
                          &pi) != FALSE;
  };

  bool started = false;
  if (try_start("tools\\clients\\v2ray.exe -config stdin:"))
    started = true;
  else if (!runpath.empty()) {
    std::string cmd = runpath;
    if (!cmd.empty() && cmd.back() != '\\' && cmd.back() != '/')
      cmd += "\\";
    cmd += "tools\\clients\\v2ray.exe -config stdin:";
    started = try_start(cmd);
  } else if (try_start(".\\v2ray.exe -config stdin:"))
    started = true;
  else if (try_start(".\\tools\\clients\\v2ray.exe -config stdin:"))
    started = true;

  CloseHandle(hRead);

  if (!started) {
    CloseHandle(hWrite);
    // fallback: write to file and run normally
    fileWrite("config.json", config_json, true);
    std::string cmd;
    if (fileExist(".\\tools\\clients\\v2ray.exe"))
      cmd = ".\\tools\\clients\\v2ray.exe -config config.json";
    else if (fileExist("tools\\clients\\v2ray.exe"))
      cmd = "tools\\clients\\v2ray.exe -config config.json";
    else
      cmd = "tools\\clients\\v2ray.exe -config config.json";
    return runProgram(cmd, runpath, false);
  }

  // 写入配置到子进程 stdin
  size_t total = 0;
  const char *data = config_json.data();
  const size_t len = config_json.size();
  while (total < len) {
    DWORD dwWritten = 0;
    BOOL ok = WriteFile(hWrite, data + total, static_cast<DWORD>(len - total),
                        &dwWritten, nullptr);
    if (!ok || dwWritten == 0)
      break;
    total += dwWritten;
  }
  CloseHandle(hWrite);

  // 不等待退出：保持进程运行以供后续端口就绪检测
  if (pi.hThread)
    CloseHandle(pi.hThread);
  // 将进程句柄入队，便于 terminateClient 使用 killByHandle 兜底
  handles.push(pi.hProcess);

  return (total == len);

#else
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    // fallback: write to file and run normally
    fileWrite("config.json", config_json, true);
    std::string cmd;
    if (fileExist("./tools/clients/v2ray"))
      cmd = "./tools/clients/v2ray -config config.json";
    else if (fileExist("./v2ray"))
      cmd = "./v2ray -config config.json";
    else
      cmd = "v2ray -config config.json";
    return runProgram(cmd, runpath, false);
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    fileWrite("config.json", config_json, true);
    std::string cmd;
    if (fileExist("./tools/clients/v2ray"))
      cmd = "./tools/clients/v2ray -config config.json";
    else if (fileExist("./v2ray"))
      cmd = "./v2ray -config config.json";
    else
      cmd = "v2ray -config config.json";
    return runProgram(cmd, runpath, false);
  }

  if (pid == 0) {
    if (!runpath.empty())
      (void)chdir(runpath.c_str());
    if (dup2(pipefd[0], STDIN_FILENO) == -1) {
      _exit(127);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    execlp("v2ray", "v2ray", "-config", "stdin:", (char *)nullptr);
    execl("./v2ray", "v2ray", "-config", "stdin:", (char *)nullptr);
    execl("./tools/clients/v2ray", "v2ray", "-config",
          "stdin:", (char *)nullptr);
    _exit(127);
  } else {
    close(pipefd[0]);
    const char *buf = config_json.data();
    const size_t len = config_json.size();
    size_t total = 0;
    while (total < len) {
      ssize_t w = write(pipefd[1], buf + total, len - total);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (w == 0)
        break;
      total += static_cast<size_t>(w);
    }
    close(pipefd[1]);

    // 不等待子进程退出，记录 pid 以便后续 killByHandle
    handles.emplace(pid);
    return (total == len);
  }
#endif
}