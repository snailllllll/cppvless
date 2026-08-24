#!/usr/bin/env python3
"""UDP 会话泄漏验证：循环建立 SOCKS5 UDP ASSOCIATE → VLESS UDP 隧道，保持后断开
观测被测端 fd 是否随循环增长（泄漏检测）
用法: python3 bench_udp_leak.py <socks端口> <名称> <轮数> <保持秒>"""
import socket, struct, time, sys

def socks5_udp_associate(proxy_port, timeout=8):
    """建立 SOCKS5 连接并发 UDP ASSOCIATE 请求"""
    s = socket.create_connection(("127.0.0.1", proxy_port), timeout=timeout)
    s.sendall(b'\x05\x01\x00')
    if s.recv(2) != b'\x05\x00':
        s.close(); raise ConnectionError('hs')
    # UDP ASSOCIATE: VER=5 CMD=3 RSV=0 ATYP=1(IPv4) 0.0.0.0:0
    req = struct.pack('>BBBB', 5, 3, 0, 1) + socket.inet_aton('0.0.0.0') + struct.pack('>H', 0)
    s.sendall(req)
    resp = s.recv(4)
    if len(resp) < 4 or resp[1] != 0:
        s.close(); raise ConnectionError(f'udp associate failed: {resp.hex()}')
    bnd = s.recv(6)  # BND.ADDR + BND.PORT
    return s, bnd

def main():
    port = int(sys.argv[1]); name = sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    hold = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    ok = fail = 0
    for i in range(rounds):
        try:
            s, bnd = socks5_udp_associate(port)
            time.sleep(hold)
            s.close()
            ok += 1
        except Exception as e:
            fail += 1
            if fail <= 3:
                print(f"  round {i} fail: {e}", flush=True)
    print(f"{name}: ok={ok} fail={fail} (rounds={rounds} hold={hold}s)", flush=True)

if __name__ == "__main__":
    main()
