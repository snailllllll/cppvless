#!/usr/bin/env python3
"""VLESS+TLS 端到端测试：TLS 封装 VLESS 请求，验证服务器内置 TLS 终结"""
import os
import socket
import ssl
import struct
import sys
import time


def build_vless_request(target_host, target_port):
    req = bytearray()
    req.append(0x00)  # version
    # UUID: 从环境变量 VLESS_TEST_UUID 读取（CI 动态注入），缺省用测试值
    uuid_str = os.environ.get("VLESS_TEST_UUID", "e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b")
    req.extend(bytes.fromhex(uuid_str.replace('-', '')))
    req.append(0x00)  # addons len
    req.append(0x01)  # cmd TCP
    req.extend(struct.pack('>H', target_port))
    req.append(0x01)  # IPv4
    req.extend(socket.inet_aton(socket.gethostbyname(target_host)))
    return bytes(req)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <tls-server:port> <target:port>")
        return 1
    server = sys.argv[1].rsplit(":", 1)
    target = sys.argv[2].rsplit(":", 1)
    host, port = server[0], int(server[1])
    t_host, t_port = target[0], int(target[1])

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE  # 自签证书，客户端不校验（对应 iOS allowInsecure）

    sock = socket.create_connection((host, port), timeout=5)
    tls = ctx.wrap_socket(sock, server_hostname=host)
    print(f"TLS version: {tls.version()}, cipher: {tls.cipher()}")

    tls.sendall(build_vless_request(t_host, t_port))
    resp = tls.recv(2)
    if len(resp) < 2:
        print("FAIL: short vless response")
        return 1
    version, addons = resp[0], resp[1]
    if version != 0:
        print(f"FAIL: wrong version {version}")
        return 1
    if addons > 0:
        tls.recv(addons)

    http_req = (f"GET / HTTP/1.1\r\nHost: {t_host}:{t_port}\r\n"
                f"Connection: close\r\n\r\n").encode()
    tls.sendall(http_req)
    data = b""
    try:
        while True:
            chunk = tls.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass

    ok = b"hello-vmess-stream-abstract" in data
    print(f"{'PASS' if ok else 'FAIL'}: got {len(data)} bytes, "
          f"content_match={ok}")
    tls.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
