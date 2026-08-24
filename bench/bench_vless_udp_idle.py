#!/usr/bin/env python3
"""僵尸 UDP 会话验证：建立 UDP 隧道后静默保持（不发送不关闭），模拟无 idle timeout 场景
观测被测端 fd 是否随会话数线性累积（资源耗尽风险）
用法: python3 bench_vless_udp_idle.py <server_ip> <port> <uuid> <会话数> <保持秒> <名称>"""
import socket, struct, time, sys

def vless_udp_connect(host, port, uuid, target, timeout=8):
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
    s.sendall(header)  # 只发 header 建立隧道，不发数据（静默）
    return s

def main():
    server = sys.argv[1]; port = int(sys.argv[2]); uuid = sys.argv[3]
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 100
    hold = float(sys.argv[5]) if len(sys.argv) > 5 else 30.0
    name = sys.argv[6] if len(sys.argv) > 6 else "IDLE"
    target = ("10.206.0.4", 5201)
    socks = []
    ok = fail = 0
    for i in range(count):
        try:
            s = vless_udp_connect(server, port, uuid, target)
            socks.append(s); ok += 1
        except Exception as e:
            fail += 1
            if fail <= 3:
                print(f"  {i} fail: {e}", flush=True)
    print(f"{name}: established={ok}/{count} fail={fail}", flush=True)
    print("HOLDING", flush=True)
    time.sleep(hold)
    for s in socks:
        try: s.close()
        except Exception: pass
    print("CLOSED", flush=True)

if __name__ == "__main__":
    main()
