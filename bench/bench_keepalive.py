#!/usr/bin/env python3
"""连接规模梯度：并发建立并保持 N 条 SOCKS5+VLESS 隧道，观测被测端内存/fd
用法: python3 bench_keepalive.py <socks端口> <名称> <连接数> <保持秒>"""
import socket, struct, time, threading, sys

def socks5_connect(proxy_port, target_host="10.206.0.4", target_port=80, timeout=10):
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

def main():
    port = int(sys.argv[1]); name = sys.argv[2]
    total = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    hold = int(sys.argv[4]) if len(sys.argv) > 4 else 15
    sockets = []
    ok = 0
    t0 = time.time()
    for i in range(total):
        try:
            s = socks5_connect(port)
            sockets.append(s); ok += 1
        except Exception:
            break
    dt = time.time() - t0
    print(f"{name}: established={ok}/{total} in {dt:.1f}s ({ok/dt:.0f} conn/s)", flush=True)
    print(f"KEEP_ALIVE_HOLD {name} {ok}", flush=True)
    time.sleep(hold)
    for s in sockets:
        try: s.close()
        except Exception: pass
    print(f"{name}: closed", flush=True)

if __name__ == "__main__":
    main()
