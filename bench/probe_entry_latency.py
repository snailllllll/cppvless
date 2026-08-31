#!/usr/bin/env python3
"""
入口节点延迟/链路质量探测工具

用途：从本机（深圳）实测到各候选入口节点的链路质量，
      用数据回答"物理距离近 ≠ 延迟低"的问题。

测量项：
  1. RTT   — ICMP ping（平均/最小/最大/抖动/丢包）
  2. 路由  — traceroute 逐跳，识别是否绕路
  3. TCP   — TCP 建连延迟（更接近真实代理握手开销）

用法：
  python3 bench/probe_entry_latency.py                # 用内置默认目标
  python3 bench/probe_entry_latency.py 1.2.3.4:1080 ...
"""
import subprocess
import sys
import re
import socket
import time
import statistics
from concurrent.futures import ThreadPoolExecutor

# 内置候选探测目标（按需修改）
DEFAULT_TARGETS = [
    ("东京lighthouse(已有)", "43.133.180.80", 1080),
    # 香港地域可参考腾讯云香港地域的公共服务 IP（示例，建议替换为自己的实例）
    # ("香港-示例", "x.x.x.x", 1080),
    # ("上海lighthouse", "x.x.x.x", 1080),
]

PING_COUNT = 20


def ping_host(host: str, count: int = PING_COUNT):
    """ICMP ping，返回统计字典。失败返回 None。"""
    try:
        out = subprocess.run(
            ["ping", "-c", str(count), "-W", "2", "-i", "0.2", host],
            capture_output=True, text=True, timeout=count * 3 + 10
        ).stdout
    except Exception:
        return None

    times = [float(m) for m in re.findall(r"time[=<]\s*([\d.]+)\s*ms", out)]
    loss_m = re.search(r"([\d.]+)% packet loss", out)
    if not times:
        return {"loss": 100.0, "error": "无回包（可能禁 ICMP）"}

    return {
        "loss": float(loss_m.group(1)) if loss_m else 0.0,
        "min": min(times),
        "avg": statistics.mean(times),
        "max": max(times),
        "mdev": statistics.pstdev(times) if len(times) > 1 else 0.0,
        "samples": len(times),
    }


def tcp_connect_latency(host: str, port: int, rounds: int = 10):
    """TCP 建连延迟（更贴近真实 VLESS 握手成本）。返回毫秒列表。"""
    latencies = []
    for _ in range(rounds):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        t0 = time.perf_counter()
        try:
            s.connect((host, port))
            latencies.append((time.perf_counter() - t0) * 1000)
        except Exception:
            pass
        finally:
            s.close()
        time.sleep(0.1)
    return latencies


def traceroute_host(host: str, max_hops: int = 20):
    """traceroute，用于识别跨境绕路。"""
    try:
        out = subprocess.run(
            ["traceroute", "-n", "-w", "2", "-q", "1", "-m", str(max_hops), host],
            capture_output=True, text=True, timeout=90
        ).stdout
    except Exception:
        return []
    hops = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+)\s+(\S+)", line)
        if m:
            hops.append((int(m.group(1)), m.group(2)))
    return hops


def probe(name, host, port):
    print(f"\n{'='*66}")
    print(f"  目标: {name}  ({host})")
    print(f"{'='*66}")

    # 1. ICMP
    print("\n[1] ICMP RTT")
    pr = ping_host(host)
    if pr and "error" not in pr:
        print(f"    平均 {pr['avg']:.2f} ms | 最小 {pr['min']:.2f} | "
              f"最大 {pr['max']:.2f} | 抖动 {pr['mdev']:.2f} | 丢包 {pr['loss']:.1f}%")
    else:
        print(f"    ICMP 不可用（{pr.get('error', '失败') if pr else '超时'}）—— 防火墙可能禁 ICMP，看 TCP 结果")

    # 2. TCP
    print(f"\n[2] TCP 建连延迟 (port {port})")
    lats = tcp_connect_latency(host, port)
    if lats:
        print(f"    平均 {statistics.mean(lats):.2f} ms | "
              f"最小 {min(lats):.2f} | 最大 {max(lats):.2f} | 成功 {len(lats)}/10")
    else:
        print("    全部失败（端口未开放或被墙）—— 注意：此结果不代表链路质量")

    # 3. 路由
    print(f"\n[3] 路由路径（前 {15} 跳，观察是否绕路）")
    hops = traceroute_host(host)
    if hops:
        for idx, ip in hops[:15]:
            print(f"    {idx:2d}  {ip}")
        if len(hops) > 15:
            print(f"    ... 共 {len(hops)} 跳")
    else:
        print("    traceroute 无结果（可能被过滤）")

    return {"name": name, "host": host, "ping": pr, "tcp": lats, "hops": hops}


def main():
    targets = DEFAULT_TARGETS
    if len(sys.argv) > 1:
        # 命令行形式: name=ip:port  或  ip:port
        targets = []
        for arg in sys.argv[1:]:
            if "=" in arg:
                nm, addr = arg.split("=", 1)
            else:
                nm, addr = arg, arg
            if ":" in addr:
                h, p = addr.rsplit(":", 1)
                targets.append((nm, h, int(p)))
            else:
                targets.append((nm, addr, 1080))

    print("入口节点链路质量探测")
    print("提示：物理距离 ≠ 网络延迟，跨境链路常绕路。本工具用实测数据决策。")

    results = []
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = [ex.submit(probe, n, h, p) for n, h, p in targets]
        for f in futs:
            try:
                results.append(f.result())
            except Exception as e:
                print(f"\n探测出错: {e}")

    # 汇总
    print(f"\n\n{'='*66}")
    print("  汇总对比")
    print(f"{'='*66}")
    print(f"{'节点':<26} {'ICMP均值':>10} {'丢包':>8} {'TCP建连':>10}")
    print(f"{'-'*66}")
    for r in results:
        p = r.get("ping") or {}
        avg = f"{p['avg']:.1f} ms" if p and "avg" in p else "  n/a"
        loss = f"{p.get('loss', 0):.1f}%" if p else "n/a"
        t = r.get("tcp") or []
        tcp = f"{statistics.mean(t):.1f} ms" if t else "  n/a"
        print(f"{r['name'][:24]:<26} {avg:>10} {loss:>8} {tcp:>10}")
    print(f"{'='*66}")


if __name__ == "__main__":
    main()
