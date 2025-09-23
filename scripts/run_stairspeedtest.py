import os
import time
import glob
import subprocess
from typing import Optional, Tuple, List, Dict

WORK_DIR = r"e:\_cpp_work\StairSpeedTest"
EXE_PATH = os.path.join(WORK_DIR, "stairspeedtest.exe")
LOGS_DIR = os.path.join(WORK_DIR, "logs")
RESULTS_DIR = os.path.join(WORK_DIR, "results")
URL_LIST_FILE = os.path.join(WORK_DIR, "urls.txt")  # 你的 5000 行 URL 文件

def snapshot_dir(dir_path: str) -> set:
    """返回目录快照（文件名集合）。"""
    if not os.path.isdir(dir_path):
        return set()
    return set(os.path.basename(p) for p in glob.glob(os.path.join(dir_path, "*.log")))

def list_new_files(before: set, dir_path: str) -> List[str]:
    """列出退出后新增的文件（按修改时间从新到旧排序）。"""
    if not os.path.isdir(dir_path):
        return []
    after = glob.glob(os.path.join(dir_path, "*.log"))
    new_paths = [p for p in after if os.path.basename(p) not in before]
    new_paths.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return new_paths

def verify_log_eof(log_path: str) -> bool:
    """检查日志是否包含 --EOF--（正常结束时会写入）。"""
    try:
        with open(log_path, "rb") as f:
            data = f.read()
        return b"--EOF--" in data
    except OSError:
        return False

def correlate_results_by_name(log_paths: List[str]) -> Dict[str, str]:
    """基于同名文件在 results 目录找对应的结果日志。"""
    mapping = {}
    for lp in log_paths:
        name = os.path.basename(lp)
        rp = os.path.join(RESULTS_DIR, name)
        if os.path.exists(rp):
            mapping[lp] = rp
    return mapping

def run_stairspeedtest_once(url: str, timeout_sec: int = 3600) -> Tuple[str, List[str], Dict[str, str], str, int]:
    """
    运行一次 stairspeedtest.exe /u <url> 并等待退出。
    返回：status, new_logs, log->result 映射, stdout_tail, returncode
    status: "completed"（含 EOF）或 "crashed"（无 EOF，但可能有结果）
    """
    # 退出前的目录快照
    logs_before = snapshot_dir(LOGS_DIR)
    results_before = snapshot_dir(RESULTS_DIR)

    start_time = time.time()
    try:
        proc = subprocess.run(
            [EXE_PATH, "/u", url],
            cwd=WORK_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            text=True,
            encoding="utf-8",
            errors="ignore",
        )
    except subprocess.TimeoutExpired as e:
        # 超时视为异常，返回空结果
        return "crashed", [], {}, "", -1

    # 退出后列出新增文件
    new_logs = list_new_files(logs_before, LOGS_DIR)
    new_results = list_new_files(results_before, RESULTS_DIR)

    # 关联同名结果
    mapping = correlate_results_by_name(new_logs)

    # 判定是否正常结束（最新日志有 EOF）
    status = "crashed"
    if new_logs:
        latest_log = new_logs[0]
        if verify_log_eof(latest_log):
            status = "completed"

    # 返回 stdout 尾部辅助排查
    stdout_tail = (proc.stdout or "").splitlines()[-50:]
    stdout_tail_str = "\n".join(stdout_tail)

    return status, new_logs, mapping, stdout_tail_str, proc.returncode

def analyze_one_round(url: str):
    print(f"[RUN] {url}")
    status, new_logs, mapping, stdout_tail, rc = run_stairspeedtest_once(url)
    print(f"[EXIT] returncode={rc} status={status}")
    if stdout_tail:
        print("[STDOUT TAIL]")
        print(stdout_tail)

    if not new_logs:
        print("[WARN] 未检测到新增日志，可能程序在写日志前即退出。")
        return

    # 遍历本次新增的日志与结果
    for log_path in new_logs:
        print(f"[LOG ] {log_path} {'(EOF)' if verify_log_eof(log_path) else '(NO EOF)'}")
        result_path = mapping.get(log_path)
        if result_path:
            print(f"[RES ] {result_path}")
        else:
            print("[RES ] 未发现同名结果日志（可能异常退出或未产生结果）。")

        # TODO: 在这里做你的“日志/结果分析”
        # with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        #     content = f.read()
        #     # 你的分析逻辑...
        # if result_path and os.path.exists(result_path):
        #     with open(result_path, "r", encoding="utf-8", errors="ignore") as f:
        #         res_content = f.read()
        #         # 你的分析逻辑...

def main():
    # 批量读取 URL 列表文件并逐个执行
    if not os.path.exists(URL_LIST_FILE):
        print(f"[ERROR] 找不到 URL 列表文件: {URL_LIST_FILE}")
        return
    with open(URL_LIST_FILE, "r", encoding="utf-8", errors="ignore") as f:
        for idx, line in enumerate(f, 1):
            url = line.strip()
            if not url or url.startswith("#"):
                continue
            print(f"==== [{idx}] ====")
            analyze_one_round(url)
    print("[DONE] 全部处理完成。")

if __name__ == "__main__":
    main()