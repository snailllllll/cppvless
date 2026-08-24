#!/usr/bin/env python3
"""多轮交替压测编排脚本（本地运行，经 SSH 控制压测端+被测端）

功能:
  - L1 连接建立速率: bench_conn.py 连 cpp-plain(10883)/go-plain(10882)，ABAB 交替 N 轮
  - L2 转发吞吐: iperf3 经 proxychains(切 10883/10882) 到被测端 iperf3 server，交替 N 轮
  - 压测期间同步经 SSH 在被测端 pidstat 采集被测进程 CPU
  - 结果落盘 CSV + 终端汇总表（取中位数）

用法:
  python3 bench_multi.py --rounds 5 --conns 50 --duration 10 --mode l1
  python3 bench_multi.py --rounds 5 --duration 8  --mode l2
  python3 bench_multi.py --rounds 5 --conns 50 --duration 10 --mode all
"""
import argparse, csv, json, os, re, statistics, subprocess, sys, time

KEY = os.environ.get("BENCH_KEY", "/data/workspace/tencentcloud/vless.pem")
PRESS_HOST = "ubuntu@119.45.147.80"    # 压测端 (8c16g, ins-9od8tycs, 10.206.0.10)
SUT_HOST = "ubuntu@119.45.39.36"       # 被测端 (8c16g, ins-pjpl5zpi, 10.206.0.4)
SUT_INNER = "10.206.0.4"               # 被测端内网 IP

PORTS = {"cpp_plain": 10883, "go_plain": 10882, "cpp_tls": 10881}
PROXYCHAIN_CONF = "/etc/proxychains4.conf"


def ssh(host, cmd, timeout=180):
    r = subprocess.run(
        ["ssh", "-i", KEY, "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no",
         "-o", "ConnectTimeout=10", host, cmd],
        capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        return ""
    return r.stdout.strip()


def get_server_pids():
    """被测端: 返回 (vmess_pid, v2ray_pid)。用 pgrep -x 精确匹配二进制名，避免匹配到 ssh 的 bash 自身"""
    vmess = ssh(SUT_HOST, "pgrep -x vmess_server | head -1")
    v2ray = ssh(SUT_HOST, "pgrep -x v2ray | head -1")
    return vmess, v2ray


def switch_proxychains(port):
    ssh(PRESS_HOST, f"sudo sed -i 's/socks5 127.0.0.1 [0-9]*/socks5 127.0.0.1 {port}/' {PROXYCHAIN_CONF}")
    cur = ssh(PRESS_HOST, f"grep -E '^socks5' {PROXYCHAIN_CONF}")
    assert str(port) in cur, f"proxychains 切换失败: {cur}"


# ---------- L1 连接建立速率 ----------
def run_l1_round(side, port, conns, duration):
    """在压测端跑一轮 bench_conn.py，返回 (rate, ok, fail)"""
    out = ssh(PRESS_HOST, f"python3 /opt/bench/bench_conn.py {port} {side} {conns} {duration}",
              timeout=duration + 30)
    m = re.search(r"rate=(\d+) conn/s", out)
    ok = re.search(r"conn_ok=(\d+)", out)
    fail = re.search(r"fail=(\d+)", out)
    if not m:
        print(f"  [warn] bench_conn 解析失败: {out}", file=sys.stderr)
        return (0, 0, 0)
    return (int(m.group(1)), int(ok.group(1)) if ok else 0, int(fail.group(1)) if fail else 0)


def sample_cpu_during(duration, pids, out_key):
    """后台线程: 在被测端 pidstat 采样进程 CPU，存全局 dict。pids 为 (vmess_pid, v2ray_pid)"""
    global CPU_RESULTS
    vmess_pid, v2ray_pid = pids
    out = ssh(SUT_HOST,
              f"pidstat -p {vmess_pid} -p {v2ray_pid} 1 {max(int(duration), 1)} 2>&1 | "
              f"grep -E 'vmess_server|v2ray'",
              timeout=duration + 30)
    rows = {"vmess": [], "v2ray": []}
    for line in out.splitlines():
        parts = line.split()
        # 跳过表头和 Average 行；%CPU 为第 9 列 (index 8)
        if len(parts) >= 11 and "Average" not in line and parts[0] != "Linux":
            try:
                cpu = float(parts[8])
            except ValueError:
                continue
            if "vmess_server" in line:
                rows["vmess"].append(cpu)
            elif "v2ray" in line:
                rows["v2ray"].append(cpu)
    CPU_RESULTS[out_key] = rows


def sample_press_cpu_during(duration, out_key):
    """后台线程: 在压测端采样 xray 进程总 CPU，判断瓶颈是否在压测端"""
    global CPU_RESULTS
    out = ssh(PRESS_HOST,
              f"pidstat -p $(pgrep -x xray | paste -sd, -) 1 {max(int(duration), 1)} 2>&1 | "
              f"grep -E 'xray' | grep -v Average",
              timeout=duration + 30)
    vals = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 9:
            try:
                vals.append(float(parts[8]))
            except ValueError:
                continue
    if out_key not in CPU_RESULTS:
        CPU_RESULTS[out_key] = {}
    CPU_RESULTS[out_key]["press"] = vals


def run_l1(rounds, conns, duration):
    """ABAB 交替: cpp_plain / go_plain，每轮各跑一次"""
    global CPU_RESULTS
    results = {}
    order = [("cpp_plain", PORTS["cpp_plain"]), ("go_plain", PORTS["go_plain"])]
    vmess_pid, v2ray_pid = get_server_pids()
    print(f"[L1] rounds={rounds} conns={conns} dur={duration}s pids: vmess={vmess_pid} v2ray={v2ray_pid}")
    for rnd in range(1, rounds + 1):
        for side, port in order:
            # 只采被测方 CPU（cpp_plain 采 vmess，go_plain 采 v2ray）
            pid = vmess_pid if side == "cpp_plain" else v2ray_pid
            cpu_key = f"r{rnd}_{side}"
            CPU_RESULTS = {}
            th = threading.Thread(target=sample_cpu_during, args=(duration, (vmess_pid, v2ray_pid), cpu_key))
            th.start()
            rate, ok, fail = run_l1_round(side, port, conns, duration)
            th.join(timeout=duration + 40)
            results.setdefault(side, []).append({"round": rnd, "rate": rate, "ok": ok, "fail": fail,
                                                 "cpu": CPU_RESULTS.get(cpu_key, {})})
            print(f"  r{rnd} {side}: rate={rate} conn/s ok={ok} fail={fail}")
    return results


# ---------- L2 转发吞吐 ----------
def run_l2_round(side, port, duration, parallel=1):
    """切换 proxychains 后跑一轮 iperf3（文本模式），返回 (bits_per_sec, retr)。
    parallel>1 时输出含每流行 + SUM 汇总行，取 SUM 行"""
    switch_proxychains(port)
    p = f" -P {parallel}" if parallel > 1 else ""
    out = ssh(PRESS_HOST,
              f"timeout {duration + 15} proxychains4 -q iperf3 -c {SUT_INNER} -p 5201 -t {duration}{p}",
              timeout=duration + 40)
    # 优先 SUM 行（多流汇总），无 SUM 时取单 receiver 行
    best = None
    for line in out.splitlines():
        if "receiver" in line and "Gbits/sec" in line:
            m = re.search(r"(\d+\.\d+)\s+Gbits/sec", line)
            if not m:
                continue
            bits = float(m.group(1)) * 1e9
            r = re.search(r"Retr\s+(\d+)", line)
            retr = int(r.group(1)) if r else 0
            if "SUM" in line:  # 多流汇总行优先
                return bits, retr
            if best is None:   # 单流时的普通行
                best = (bits, retr)
    if best:
        return best
    print(f"  [warn] iperf3 输出解析失败:\n{out[-400:]}", file=sys.stderr)
    return 0, 0


def run_l2(rounds, duration, parallel=1):
    global CPU_RESULTS
    results = {}
    order = [("cpp_plain", PORTS["cpp_plain"]), ("go_plain", PORTS["go_plain"])]
    vmess_pid, v2ray_pid = get_server_pids()
    print(f"[L2] rounds={rounds} dur={duration}s parallel={parallel} pids: vmess={vmess_pid} v2ray={v2ray_pid}")
    for rnd in range(1, rounds + 1):
        for side, port in order:
            cpu_key = f"r{rnd}_{side}"
            CPU_RESULTS = {}
            th_sut = threading.Thread(target=sample_cpu_during, args=(duration, (vmess_pid, v2ray_pid), cpu_key))
            th_press = threading.Thread(target=sample_press_cpu_during, args=(duration, cpu_key))
            th_sut.start()
            th_press.start()
            bits, retr = run_l2_round(side, port, duration, parallel)
            th_sut.join(timeout=duration + 40)
            th_press.join(timeout=duration + 40)
            gbps = bits / 1e9
            results.setdefault(side, []).append({"round": rnd, "gbps": gbps, "retr": retr,
                                                 "cpu": CPU_RESULTS.get(cpu_key, {}),
                                                 "parallel": parallel})
            print(f"  r{rnd} {side}: {gbps:.3f} Gbps retr={retr}")
    return results


# ---------- 汇总 ----------
def summarize(results, metric="rate", is_l1=True):
    print("\n===== 汇总（各轮 + 中位数） =====")
    rows = []
    for side, items in results.items():
        vals = [i[metric] for i in items if i[metric] > 0]
        med = statistics.median(vals) if vals else 0
        cpus = []
        for i in items:
            c = i.get("cpu", {})
            key = "vmess" if side == "cpp_plain" else "v2ray"
            vals_c = c.get(key, [])
            if vals_c:
                cpus.append(max(vals_c))
        med_cpu = statistics.median(cpus) if cpus else 0
        unit = "conn/s" if is_l1 else "Gbps"
        rows.append((side, items, med, med_cpu, unit))
        print(f"  {side}: {metric}={med:.0f}{unit}" if is_l1 else f"  {side}: {metric}={med:.3f}{unit}",
              end="")
        if cpus:
            print(f"  cpu_max_med={med_cpu:.1f}%")
        else:
            print()
    return rows


def save_csv(results, outpath, mode):
    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["mode", "side", "round", "metric", "cpu_max_pct"])
        for side, items in results.items():
            for i in items:
                # side 形如 l1_cpp_plain / l2_go_plain，取最后一段判断进程名
                base = side.split("_", 1)[1] if "_" in side else side
                key = "vmess" if base == "cpp_plain" else "v2ray"
                c = i.get("cpu", {}).get(key, [])
                metric = i["rate"] if mode == "l1" else i["gbps"]
                w.writerow([mode, side, i["round"], metric, max(c) if c else ""])
    print(f"\n结果已写入: {outpath}")


if __name__ == "__main__":
    import threading
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--conns", type=int, default=50, help="L1 并发")
    ap.add_argument("--duration", type=int, default=10, help="每轮秒数 (L1) / iperf3 时长 (L2)")
    ap.add_argument("--parallel", type=int, default=1, help="L2 iperf3 并发流数 -P")
    ap.add_argument("--mode", choices=["l1", "l2", "all"], default="all")
    ap.add_argument("--out", default="/tmp/bench_results.csv")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    global CPU_RESULTS
    CPU_RESULTS = {}

    t0 = time.time()
    if args.mode in ("l1", "all"):
        r1 = run_l1(args.rounds, args.conns, args.duration)
        summarize(r1, "rate", is_l1=True)
    if args.mode in ("l2", "all"):
        r2 = run_l2(args.rounds, args.duration, args.parallel)
        summarize(r2, "gbps", is_l1=False)
        # 打印压测端 CPU（判断瓶颈端）
        press_vals = [max(i.get("cpu", {}).get("press", [0])) for side in r2 for i in r2[side]]
        if press_vals:
            print(f"  压测端 xray CPU 峰值中位数: {statistics.median(press_vals):.1f}%")

    # 合并落盘
    combined = {}
    if args.mode in ("l1", "all"):
        combined.update({f"l1_{k}": v for k, v in r1.items()})
    if args.mode in ("l2", "all"):
        combined.update({f"l2_{k}": v for k, v in r2.items()})
    if combined:
        save_csv(combined, args.out, args.mode)
    print(f"总耗时 {time.time()-t0:.0f}s")
