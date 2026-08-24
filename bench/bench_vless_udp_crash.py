#!/usr/bin/env python3
"""客户端异常消失模拟：建立 UDP 隧道后进程直接 os._exit（TCP 连接变 RST/悬挂）
验证服务端对异常断开的回收能力（是否泄漏 fd）
用法: python3 bench_vless_udp_crash.py <server_ip> <port> <uuid> <会话数> <名称>
每个子进程建立隧道后立即 os._exit(0)，模拟客户端崩溃"""
import socket, struct, time, sys, os

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
    s.sendall(header)
    return s

def main():
    server = sys.argv[1]; port = int(sys.argv[2]); uuid = sys.argv[3]
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 100
    name = sys.argv[5] if len(sys.argv) > 5 else "CRASH"
    target = ("10.206.0.4", 5201)
    ok = 0
    for i in range(count):
        pid = os.fork()
        if pid == 0:
            # 子进程：建隧道后立即崩溃（不 close，模拟客户端进程异常退出）
            try:
                s = vless_udp_connect(server, port, uuid, target)
                os._exit(0)  # 不 close socket → 内核发 RST（若有未读数据）或悬挂
            except Exception:
                os._exit(1)
        else:
            os.waitpid(pid, 0)
            ok += 1
            time.sleep(0.02)
    print(f"{name}: forked={ok} sessions (each crashed)", flush=True)

if __name__ == "__main__":
    main()
