#!/usr/bin/env python3
"""cpp-vless vs go(v2ray) 服务端压测脚本（经 Xray SOCKS5 出口）

用法:
  python3 bench_socks5.py --proxy 127.0.0.1:10881 --name cpp-tls --conns 200 --duration 10
  python3 bench_socks5.py --proxy 127.0.0.1:10882 --name go-plain --conns 200 --duration 10

测量:
  1. 连接建立速率 (conn/s): 并发建立 SOCKS5+VLESS 隧道并立即关闭
  2. 端到端延迟 RTT: 通过隧道 GET 小目标 (httpbin 或自建后端)
  3. 并发吞吐: 高并发请求小页面
  4. 服务器 CPU: 在测试期间采样远端 CPU
"""
import argparse
import socket
import struct
import sys
import threading
import time

def socks5_connect(proxy_host, proxy_port, target_host, target_port, timeout=10):
    """建立 SOCKS5 隧道并返回 socket"""
    s = socket.create_connection((proxy_host, proxy_port), timeout=timeout)
    # 握手
    s.sendall(b'\x05\x01\x00')
    resp = s.recv(2)
    if resp != b'\x05\x00':
        s.close()
        raise ConnectionError(f"socks5 handshake failed: {resp.hex()}")
    # CONNECT 请求（IPv4 目标）
    try:
        ip = socket.inet_aton(target_host)
        addr_type = 0x01
    except OSError:
        addr_type = 0x03  # domain
    req = struct.pack('>BBB', 0x05, 0x01, 0x00) + struct.pack('>B', addr_type)
    if addr_type == 0x01:
        req += ip
    else:
        host_bytes = target_host.encode()
        req += struct.pack('>B', len(host_bytes)) + host_bytes
    req += struct.pack('>H', target_port)
    s.sendall(req)
    resp = s.recv(4)
    if len(resp) < 4 or resp[1] != 0x00:
        s.close()
        raise ConnectionError(f"socks5 connect failed: {resp.hex()}")
    # 吃掉剩余地址
    s.recv(6)
    return s

def bench_conn_rate(proxy, conns, duration, results, idx):
    """并发建立连接，测量成功数/耗时"""
    host, port = proxy
    deadline = time.time() + duration
    ok, fail = 0, 0
    while time.time() < deadline:
        try:
            s = socks5_connect(host, port, "127.0.0.1", 9)  # 目标端口9 (discard) 只要隧道建立
            s.close()
            ok += 1
        except Exception:
            fail += 1
    results[idx] = (ok, fail)

def bench_http(proxy, conns, duration, target, results, idx):
    """并发 HTTP 请求，测量吞吐/延迟"""
    host, port = proxy
    thost, tport = target
    deadline = time.time() + duration
    ok, fail = 0, 0
    latencies = []
    while time.time() < deadline:
        t0 = time.time()
        try:
            s = socks5_connect(host, port, thost, tport)
            req = (f"GET / HTTP/1.1\r\nHost: {thost}\r\nConnection: close\r\n\r\n").encode()
            s.sendall(req)
            data = s.recv(512)
            if data:
                ok += 1
                latencies.append((time.time() - t0) * 1000)
            else:
                fail += 1
            s.close()
        except Exception:
            fail += 1
    results[idx] = (ok, fail, latencies)

def sample_cpu(host, duration, results, idx):
    """经 SSH 采样远端 CPU（在压测机执行，连远程 shell）"""
    import subprocess
    import os
    key = os.environ.get("SSH_KEY", "/root/.ssh/id_ed25519_1panel")
    # 用 lighthouse 不方便，这里留 hook：压测后手动采集
    results[idx] = None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy", required=True, help="SOCKS5 代理 host:port")
    ap.add_argument("--name", required=True, help="场景名")
    ap.add_argument("--conns", type=int, default=100)
    ap.add_argument("--duration", type=int, default=10)
    ap.add_argument("--mode", choices=["conn", "http"], default="http")
    ap.add_argument("--target", default="43.133.180.80:8080", help="HTTP 目标 host:port（东京后端）")
    args = ap.parse_args()

    phost, pport = args.proxy.split(":")
    pport = int(pport)
    thost, tport = args.target.split(":")
    tport = int(tport)

    print(f"[{args.name}] proxy={args.proxy} conns={args.conns} dur={args.duration}s mode={args.mode} target={args.target}")
    sys.stdout.flush()

    threads = []
    results = [None] * args.conns
    for i in range(args.conns):
        if args.mode == "conn":
            t = threading.Thread(target=bench_conn_rate, args=((phost, pport), args.conns, args.duration, results, i))
        else:
            t = threading.Thread(target=bench_http, args=((phost, pport), args.conns, args.duration, (thost, tport), results, i))
        threads.append(t)

    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.time() - t0

    # 汇总
    total_ok = sum(r[0] for r in results if r)
    total_fail = sum(r[1] for r in results if r)
    print(f"[{args.name}] 总完成: ok={total_ok} fail={total_fail} 耗时={elapsed:.1f}s")
    if total_ok > 0:
        print(f"[{args.name}] 完成速率: {total_ok/elapsed:.1f} ops/s")
    if args.mode == "http":
        lats = sorted([l for r in results if r for l in r[2]])
        if lats:
            n = len(lats)
            print(f"[{args.name}] RTT: p50={lats[n//2]:.1f}ms p95={lats[int(n*0.95)]:.1f}ms p99={lats[int(n*0.99)]:.1f}ms max={lats[-1]:.1f}ms")

if __name__ == "__main__":
    main()
