# 目标与方案
# - 目标：通过rpc用户，无阻塞实现多次自动高效调用stairspeedtest与结果收集。
# - 方案：采用 RPC 模式运行 stairspeedtest.exe ，脚本流式读取标准输出，检测 {"info":"eof"} 自动结束；并在每次运行后自动定位并解析最新的 results/*.log INI 文件。
# 已完成改造
# CLI 模式避免按键：若你坚持 CLI 而非 RPC，可在 pref.ini 的 [advanced] 里设置 pause_on_done=false 。但 RPC 已做到无阻塞，推荐优先使用。
# - 使用 /rpc /u <url> 调用，避免 _getch() 阻塞，自动化无人工干预。
# - 流式读取 stdout，识别 {"info":"eof"} 作为结束信号；同时保留尾部日志输出用于排查。
# - 运行前后分别对 logs 与 results 目录做快照，结束后识别本次新增的 results/*.log ，并解析 INI 内容（使用 Python configparser ，保持键大小写）。
# - 支持批量处理 urls.txt 多行订阅地址，逐条运行并输出每次结果文件与解析条目统计。
# - 加入超时、返回码与尾部日志（最后 50 行）输出，提升容错与可观测性。
# 如何使用
# - 将订阅地址逐行写入 e:\_cpp_work\StairSpeedTest\urls.txt ，支持注释行（以 # 起始）。
# - 执行命令： python e:\_cpp_work\StairSpeedTest\scripts\run_stairspeedtest.py
# - 每个 URL 会：
#   - 流式运行 stairspeedtest.exe /rpc /u <url>
#   - 检测 EOF 自动结束，无需人工按键
#   - 输出新增 logs/*.log 和 results/*.log 路径
#   - 解析结果 INI 并统计 section 数量（每个节点一节）
#   - 输出尾部日志用于排查
# 脚本关键路径
# - 可执行： e:\_cpp_work\StairSpeedTest\stairspeedtest.exe
# - 日志目录： e:\_cpp_work\StairSpeedTest\logs
# - 结果目录： e:\_cpp_work\StairSpeedTest\results
# - URL 列表： e:\_cpp_work\StairSpeedTest\urls.txt
# - 脚本： e:\_cpp_work\StairSpeedTest\scripts\run_stairspeedtest.py
# 说明与细节
# - 结果定位策略：不再按“同名日志”关联（日志与结果初始化时间不同步，文件名常不同），而是识别“新增的 results/*.log ”，按修改时间排序取最新。
# - 结果解析：忽略 Basic 节，其他 section 逐条生成字典；解析常用数值与数组键如 RawPing, RawSitePing, RawSpeed 。你可直接对 parsed_results 做 map-reduce。
# - 尾部日志：捕获最后 50 行便捷排错；若需完整日志可将 stdout_tail 改为全量存储。
# - 超时：默认 1800 秒（30 分），可按需要在 run_stairspeedtest_once(url, timeout_sec=...) 调整。
# - 性能参数：如需提升解析速度，可传 extra_args 注入 --parse-threads N --parse-threshold M 等参数（如果你的 C++ 程序支持这些开关）。
# 可选增强

# - CLI 模式避免按键：若你坚持 CLI 而非 RPC，可在 pref.ini 的 [advanced] 里设置 pause_on_done=false 。但 RPC 已做到无阻塞，推荐优先使用。
# - 服务模式：若要极致效率和持续运行，可考虑 stairspeedtest.exe /web 作为持久服务，由 Python 调用 HTTP/RPC 接口触发测试并拉取结果。此模式适合大量高频任务。
# 如需把解析后的结构直接输出到 JSON 或写入数据库、做并发批跑、或增加“失败重试/健康度加权”等 map-reduce 逻辑，我可以继续完善脚本。

import os
import glob
import time
import json
import subprocess
import configparser
from typing import List, Dict, Optional

WORK_DIR = r"e:\_cpp_work\StairSpeedTest"
EXE_PATH = os.path.join(WORK_DIR, "stairspeedtest.exe")
LOGS_DIR = os.path.join(WORK_DIR, "logs")
RESULTS_DIR = os.path.join(WORK_DIR, "results")
URL_LIST_FILE = os.path.join(WORK_DIR, "urls.txt")


def snapshot(dir_path: str, pattern: str = "*.log") -> set:
    if not os.path.isdir(dir_path):
        return set()
    return set(os.path.basename(p) for p in glob.glob(os.path.join(dir_path, pattern)))


def new_files(before: set, dir_path: str, pattern: str = "*.log") -> List[str]:
    if not os.path.isdir(dir_path):
        return []
    paths = glob.glob(os.path.join(dir_path, pattern))
    ret = [p for p in paths if os.path.basename(p) not in before]
    ret.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return ret


def parse_result_ini(path: str) -> List[Dict]:
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.optionxform = str  # keep case
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        cfg.read_file(f)
    out: List[Dict] = []
    for sec in cfg.sections():
        if sec == "Basic":
            continue
        d = cfg[sec]
        node: Dict = {"section": sec}
        parts = sec.split("|")
        if len(parts) == 3:
            node["proto"], node["server"], node["port"] = parts[0], parts[1], int(parts[2])
        node["Remarks"] = d.get("Remarks", "")
        node["AvgPing"] = d.get("AvgPing", "")
        node["PkLoss"] = d.get("PkLoss", "")
        node["SitePing"] = d.get("SitePing", "")
        node["AvgSpeed"] = d.get("AvgSpeed", "")
        node["MaxSpeed"] = d.get("MaxSpeed", "")
        node["ULSpeed"] = d.get("ULSpeed", "")
        node["UsedTraffic"] = int(d.get("UsedTraffic", "0") or "0")
        node["GroupID"] = int(d.get("GroupID", "0") or "0")
        node["ID"] = int(d.get("ID", "-1") or "-1")
        node["Online"] = d.get("Online", "false").lower() == "true"
        for key in ("RawPing", "RawSitePing", "RawSpeed"):
            val = d.get(key, "")
            node[key] = [int(x) for x in val.split(",") if x.strip().isdigit()] if val else []
        node["OutboundCountryCode"] = d.get("OutboundCountryCode", "")
        out.append(node)
    return out


def run_stairspeedtest_once(url: str, timeout_sec: int = 1800, extra_args: Optional[List[str]] = None) -> Dict:
    logs_before = snapshot(LOGS_DIR)
    results_before = snapshot(RESULTS_DIR)

    args = [EXE_PATH, "/rpc", "/u", url]
    if extra_args:
        args.extend(extra_args)

    proc = subprocess.Popen(
        args,
        cwd=WORK_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="ignore",
        bufsize=1,
    )

    lines: List[str] = []
    completed = False
    start = time.time()

    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                if time.time() - start > timeout_sec:
                    proc.kill()
                    break
                continue
            line = line.rstrip("\r\n")
            lines.append(line)
            # detect RPC EOF
            try:
                j = json.loads(line)
                if j.get("info") == "eof":
                    completed = True
            except Exception:
                pass
    finally:
        try:
            rc = proc.wait(timeout=5)
        except Exception:
            rc = -1

    new_logs = new_files(logs_before, LOGS_DIR)
    new_results = new_files(results_before, RESULTS_DIR)
    result_file = new_results[0] if new_results else None
    parsed = parse_result_ini(result_file) if result_file else []

    return {
        "returncode": rc,
        "completed": completed,
        "stdout_tail": "\n".join(lines[-50:]),
        "new_logs": new_logs,
        "new_results": new_results,
        "result_file": result_file,
        "parsed_results": parsed,
    }


def analyze_one_round(url: str):
    print(f"[RUN] {url}")
    info = run_stairspeedtest_once(url)
    print(f"[EXIT] returncode={info['returncode']} completed={info['completed']}")
    if info["stdout_tail"]:
        print("[STDOUT TAIL]")
        print(info["stdout_tail"])
    for lp in info["new_logs"]:
        print(f"[LOG ] {lp}")
    if info["result_file"]:
        print(f"[RES ] {info['result_file']} sections={len(info['parsed_results'])}")
    else:
        print("[RES ] 未产生结果文件（单节点或错误）。")


def main():
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