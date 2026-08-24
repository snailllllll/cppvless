#!/usr/bin/env python3
"""cpvs go server bench via Xray socks5"""
import socket, struct, time, threading, sys

# 同 VPC 内网 nginx（后端目标），不再走公网
TARGET_HOST = "10.206.0.4"
TARGET_PORT = 80

def socks5_connect(proxy_port, timeout=10):
    s = socket.create_connection(("127.0.0.1", proxy_port), timeout=timeout)
    s.sendall(b'\x05\x01\x00')
    if s.recv(2) != b'\x05\x00':
        s.close(); raise ConnectionError('hs')
    try:
        ip = socket.inet_aton(TARGET_HOST); at = 0x01
    except OSError:
        at = 0x03
    req = struct.pack('>BBB', 5, 1, 0) + struct.pack('>B', at)
    if at == 0x01:
        req += ip
    else:
        hb = TARGET_HOST.encode()
        req += struct.pack('>B', len(hb)) + hb
    req += struct.pack('>H', TARGET_PORT)
    s.sendall(req)
    resp = s.recv(4)
    if len(resp) < 4 or resp[1] != 0:
        s.close(); raise ConnectionError('conn')
    s.recv(6)
    return s

def worker(port, results, idx, dur):
    ok = fail = 0; lats = []
    deadline = time.time() + dur
    while time.time() < deadline:
        t0 = time.time()
        try:
            s = socks5_connect(port)
            s.sendall(b'GET / HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n')
            if s.recv(512):
                ok += 1; lats.append((time.time()-t0)*1000)
            else:
                fail += 1
            s.close()
        except Exception:
            fail += 1
    results[idx] = (ok, fail, lats)

def run(port, name, conns, dur):
    results = [None]*conns; ts = []
    t0 = time.time()
    for i in range(conns):
        t = threading.Thread(target=worker, args=(port, results, i, dur))
        ts.append(t); t.start()
    for t in ts: t.join()
    el = time.time()-t0
    ok = sum(r[0] for r in results if r); fail = sum(r[1] for r in results if r)
    lats = sorted(l for r in results if r for l in r[2])
    print(f"{name}: ok={ok} fail={fail} rate={ok/el:.0f}/s", flush=True)
    if lats:
        n = len(lats)
        print(f"  RTT p50={lats[n//2]:.1f} p95={lats[int(n*0.95)]:.1f} p99={lats[int(n*0.99)]:.1f} max={lats[-1]:.1f} ms", flush=True)

if __name__ == "__main__":
    port = int(sys.argv[1]); name = sys.argv[2]
    conns = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    dur = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    run(port, name, conns, dur)
