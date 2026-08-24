#!/usr/bin/env python3
"""UDP 泄漏验证 v2：ASSOCIATE 后发真实 UDP 数据（确保隧道建立），保持后断开
用法: python3 bench_udp_leak2.py <socks端口> <名称> <轮数> <保持秒>
UDP 数据发到 socks 端口返回的 BND (127.0.0.1:<socks端口>)，目标 10.206.0.4:5201(UDP)
"""
import socket, struct, time, sys

def recvn(s, n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c:
            break
        d += c
    return d

def socks5_udp_associate(proxy_port, timeout=8):
    s = socket.create_connection(("127.0.0.1", proxy_port), timeout=timeout)
    s.sendall(b'\x05\x01\x00')
    if recvn(s, 2) != b'\x05\x00':
        s.close(); raise ConnectionError('hs')
    req = struct.pack('>BBBB', 5, 3, 0, 1) + socket.inet_aton('0.0.0.0') + struct.pack('>H', 0)
    s.sendall(req)
    r = recvn(s, 10)  # 05000001 + BND.ADDR(4) + BND.PORT(2)
    if len(r) < 10 or r[1] != 0:
        s.close(); raise ConnectionError(f'udp associate failed: {r.hex()}')
    bnd_ip = socket.inet_ntoa(r[5:9]); bnd_port = struct.unpack('>H', r[8:10])[0]
    return s, (bnd_ip, bnd_port)

def main():
    port = int(sys.argv[1]); name = sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    hold = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    target = ("10.206.0.4", 5201)
    ok = fail = 0
    for i in range(rounds):
        try:
            s, bnd = socks5_udp_associate(port)
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.settimeout(1)
            hdr = struct.pack('>HBB', 0, 0, 1) + socket.inet_aton(target[0]) + struct.pack('>H', target[1])
            udp.sendto(hdr + b'ping' * 100, bnd)
            time.sleep(hold)
            udp.close(); s.close()
            ok += 1
        except Exception as e:
            fail += 1
            if fail <= 5:
                print(f"  round {i} fail: {e}", flush=True)
    print(f"{name}: ok={ok} fail={fail} (rounds={rounds} hold={hold}s)", flush=True)

if __name__ == "__main__":
    main()
