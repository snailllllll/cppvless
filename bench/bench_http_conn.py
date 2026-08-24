#!/usr/bin/env python3
"""高并发 HTTP 延迟分位压测：经 Xray SOCKS5 -> VLESS -> 内网 nginx
测量: 完成速率 (req/s)、RTT p50/p95/p99/max
用法: python3 bench_http_conn.py <socks端口> <名称> <并发> <时长> [目标host:port]"""
import socket, struct, time, threading, sys

DEFAULT_TARGET = ("10.206.0.4", 80)

def socks5_connect(proxy_port, target, timeout=10):
    s = socket.create_connection(("127.0.0.1", proxy_port), timeout=timeout)
    s.sendall(b'\x05\x01\x00')
    if s.recv(2) != b'\x05\x00':
        s.close(); raise ConnectionError('hs')
    thost, tport = target
    try:
        ip = socket.inet_aton(thost); at = 0x01
    except OSError:
        at = 0x03
    req = struct.pack('>BBB', 5, 1, 0) + struct.pack('>B', at)
    if at == 0x01:
        req += ip
    else:
        hb = thost.encode()
        req += struct.pack('>B', len(hb)) + hb
    req += struct.pack('>H', tport)
    s.sendall(req)
    resp = s.recv(4)
    if len(resp) < 4 or resp[1] != 0:
        s.close(); raise ConnectionError('conn')
    s.recv(6)
    return s

def worker(port, target, results, idx, dur):
    ok = fail = 0; lats = []
    deadline = time.time() + dur
    while time.time() < deadline:
        t0 = time.time()
        try:
            s = socks5_connect(port, target)
            s.sendall(b'GET / HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n')
            data = b''
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            if data:
                ok += 1; lats.append((time.time()-t0)*1000)
            else:
                fail += 1
            s.close()
        except Exception:
            fail += 1
    results[idx] = (ok, fail, lats)

def run(port, name, conns, dur, target):
    results = [None]*conns; ts = []
    t0 = time.time()
    for i in range(conns):
        t = threading.Thread(target=worker, args=(port, target, results, i, dur))
        ts.append(t); t.start()
    for t in ts: t.join()
    el = time.time()-t0
    ok = sum(r[0] for r in results if r); fail = sum(r[1] for r in results if r)
    lats = sorted(l for r in results if r for l in r[2])
    print(f"{name}: ok={ok} fail={fail} rate={ok/el:.0f} req/s", flush=True)
    if lats:
        n = len(lats)
        print(f"  RTT p50={lats[n//2]:.2f} p90={lats[int(n*0.90)]:.2f} p95={lats[int(n*0.95)]:.2f} p99={lats[int(n*0.99)]:.2f} max={lats[-1]:.2f} ms", flush=True)

if __name__ == "__main__":
    port = int(sys.argv[1]); name = sys.argv[2]
    conns = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    dur = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    tgt = DEFAULT_TARGET
    if len(sys.argv) > 5:
        h, p = sys.argv[5].split(":")
        tgt = (h, int(p))
    run(port, name, conns, dur, tgt)
