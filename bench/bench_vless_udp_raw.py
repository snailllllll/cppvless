#!/usr/bin/env python3
"""直接构造 VLESS UDP 请求连 cpp server（绕过 Xray），验证服务端 UDP 路径泄漏
用法: python3 bench_vless_udp_raw.py <server_ip> <port> <uuid> <轮数> <保持秒> <名称>
协议: Version(1)=0 + UUID(16) + AddonLen(1)=0 + Cmd(1)=0x02(UDP) + Port(2) + ATYP+ADDR + payload"""
import socket, struct, time, sys

def vless_udp_connect(host, port, uuid, target, payload, timeout=8):
    s = socket.create_connection((host, port), timeout=timeout)
    ver = b'\x00'
    uuid_b = bytes.fromhex(uuid.replace('-', ''))
    addon_len = b'\x00'
    cmd = b'\x02'  # UDP
    thost, tport = target
    tport_b = struct.pack('>H', tport)
    try:
        tip = socket.inet_aton(thost); atyp = b'\x01'
        addr = atyp + tip
    except OSError:
        atyp = b'\x03'
        tb = thost.encode()
        addr = atyp + bytes([len(tb)]) + tb
    header = ver + uuid_b + addon_len + cmd + tport_b + addr
    s.sendall(header + payload)
    return s

def main():
    server = sys.argv[1]; port = int(sys.argv[2]); uuid = sys.argv[3]
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 50
    hold = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    name = sys.argv[6] if len(sys.argv) > 6 else "RAW"
    target = ("10.206.0.4", 5201)
    ok = fail = 0
    for i in range(rounds):
        try:
            s = vless_udp_connect(server, port, uuid, target, b'ping' * 100)
            time.sleep(hold)
            s.close()
            ok += 1
        except Exception as e:
            fail += 1
            if fail <= 5:
                print(f"  round {i} fail: {e}", flush=True)
    print(f"{name}: ok={ok} fail={fail} (rounds={rounds} hold={hold}s)", flush=True)

if __name__ == "__main__":
    main()
