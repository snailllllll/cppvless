#!/usr/bin/env python3
"""连接建立速率压测：只建 SOCKS5+VLESS 隧道，立即关闭。测服务端 accept+握手+隧道建立能力"""
import socket, struct, time, threading, sys

def socks5_connect(proxy_port, target_host, target_port, timeout=8):
    s = socket.create_connection(("127.0.0.1", proxy_port), timeout=timeout)
    s.sendall(b'\x05\x01\x00')
    if s.recv(2) != b'\x05\x00':
        s.close(); raise ConnectionError('hs')
    try:
        ip = socket.inet_aton(target_host); at = 0x01
    except OSError:
        at = 0x03
    req = struct.pack('>BBB', 5, 1, 0) + struct.pack('>B', at)
    if at == 0x01:
        req += ip
    else:
        hb = target_host.encode()
        req += struct.pack('>B', len(hb)) + hb
    req += struct.pack('>H', target_port)
    s.sendall(req)
    resp = s.recv(4)
    if len(resp) < 4 or resp[1] != 0:
        s.close(); raise ConnectionError('conn')
    s.recv(6)
    return s

def worker(port, results, idx, dur):
    ok = fail = 0
    deadline = time.time() + dur
    while time.time() < deadline:
        try:
            s = socks5_connect(port, "10.255.0.1", 1)  # 不可达内网目标，只测隧道建立
            s.close()
            ok += 1
        except Exception:
            fail += 1
    results[idx] = (ok, fail)

def run(port, name, conns, dur):
    results = [None]*conns; ts = []
    t0 = time.time()
    for i in range(conns):
        t = threading.Thread(target=worker, args=(port, results, i, dur))
        ts.append(t); t.start()
    for t in ts: t.join()
    el = time.time()-t0
    ok = sum(r[0] for r in results if r); fail = sum(r[1] for r in results if r)
    print(f"{name}: conn_ok={ok} fail={fail} rate={ok/el:.0f} conn/s (elapsed={el:.1f}s)", flush=True)

if __name__ == "__main__":
    port = int(sys.argv[1]); name = sys.argv[2]
    conns = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    dur = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    run(port, name, conns, dur)
