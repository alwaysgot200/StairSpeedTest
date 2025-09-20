#include <algorithm>
#include <iostream>
#include <queue>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include "misc.h"

#ifdef _WIN32
#include <windows.h>
#else
// #include <spawn.h>
#include <sys/wait.h>
#endif // _WIN32

#include "processes.h"

#ifndef _WIN32
typedef pid_t HANDLE;
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

/*
void runprogram(std::string command, std::string runpath, bool wait)
{
#ifdef _WIN32
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si,sizeof(si));
    ZeroMemory(&pi,sizeof(pi));
    char curdir[512]= {},*cmdstr= {},*pathstr= {};
    command = "/c " + command;
    si.wShowWindow=true;
    cmdstr=const_cast<char*>(command.data());
    GetCurrentDirectory(512,curdir);
    runpath=std::string(curdir) + "\\"+runpath+"\\";
    pathstr=const_cast<char*>(runpath.data());
    SHELLEXECUTEINFO ShExecInfo = { 0 };
    ShExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    ShExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    ShExecInfo.hwnd = NULL;
    ShExecInfo.lpVerb = "open";
    ShExecInfo.lpFile = "cmd.exe";
    ShExecInfo.lpParameters = cmdstr;
    ShExecInfo.lpDirectory = pathstr;
    ShExecInfo.nShow = SW_HIDE;
    ShExecInfo.hInstApp = NULL;
    ShellExecuteEx(&ShExecInfo);
    if(wait)
    {
        WaitForSingleObject(ShExecInfo.hProcess, INFINITE);
        CloseHandle(hProc);
    }
#else
    //wait for Linux codes
#endif // _WIN32
    return;
}
*/

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
