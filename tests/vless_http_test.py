#!/usr/bin/env python3
"""VLESS 端到端测试：握手 + 发送 HTTP 请求，支持多连接并发"""
import os
import socket
import struct
import sys
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed


def uuid_to_bytes(uuid_str: str) -> bytes:
    """将 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx' 转成 16 字节"""
    return bytes.fromhex(uuid_str.replace('-', ''))


def build_vless_request(target_host: str, target_port: int, uuid_bytes: bytes = None) -> bytes:
    """按 Xray-core encoding.go 构造 VLESS 请求头"""
    req = bytearray()
    # 1. Version
    req.append(0x00)
    # 2. UUID: 从环境变量 VLESS_TEST_UUID 读取（CI 经 Gitea Secrets 注入），缺省用测试值
    if uuid_bytes is None:
        uuid_str = os.environ.get("VLESS_TEST_UUID", "e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b")
        uuid_bytes = uuid_to_bytes(uuid_str)
    req.extend(uuid_bytes)
    # 3. Addons length = 0
    req.append(0x00)
    # 4. Command = TCP
    req.append(0x01)
    # 5. Port (big-endian)
    req.extend(struct.pack('>H', target_port))
    # 6. Address
    ip = socket.getaddrinfo(target_host, None, socket.AF_INET)
    if ip:
        ipv4 = ip[0][4][0]
        req.append(0x01)  # IPv4
        req.extend(socket.inet_aton(ipv4))
    else:
        req.append(0x02)  # Domain
        req.append(len(target_host))
        req.extend(target_host.encode())
    return bytes(req)


def test_vless(vless_host: str, vless_port: int,
               target_host: str, target_port: int,
               conn_id: int = 0) -> dict:
    """单连接测试，返回结果字典"""
    result = {
        'id': conn_id,
        'success': False,
        'error': None,
        'response_size': 0,
        'elapsed': 0,
    }
    start = time.monotonic()

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((vless_host, vless_port))

        # 发送 VLESS 请求头
        req = build_vless_request(target_host, target_port)
        sock.sendall(req)

        # 读取响应头
        resp = sock.recv(2)
        if len(resp) < 2:
            result['error'] = 'short response'
            return result
        version, addons_len = resp[0], resp[1]
        if version != 0:
            result['error'] = f'wrong version {version}'
            return result
        if addons_len > 0:
            sock.recv(addons_len)

        # 发送 HTTP GET 请求
        http_req = (
            f"GET / HTTP/1.1\r\n"
            f"Host: {target_host}:{target_port}\r\n"
            f"User-Agent: VLESS-Test/1.0\r\n"
            f"Connection: close\r\n\r\n"
        ).encode()
        sock.sendall(http_req)

        # 读取 HTTP 响应
        sock.settimeout(5)
        response = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                response += chunk
        except socket.timeout:
            pass

        result['response_size'] = len(response)
        result['success'] = True

        # 只在单连接模式下打印详情
        if conn_id == 0:
            preview = response[:300].decode('utf-8', errors='replace')
            print(f"[Conn {conn_id}] Response preview:\n{preview[:200]}")

    except Exception as e:
        result['error'] = str(e)
    finally:
        try:
            sock.close()
        except Exception:
            pass
        result['elapsed'] = time.monotonic() - start

    return result


def run_concurrent_test(vless_host, vless_port, target_host, target_port,
                        num_connections, max_workers=None):
    """并发多连接测试"""
    if max_workers is None:
        max_workers = min(num_connections, 20)

    print(f"\n{'='*60}")
    print(f"  Concurrent Test: {num_connections} connections, {max_workers} workers")
    print(f"  VLESS: {vless_host}:{vless_port} → Target: {target_host}:{target_port}")
    print(f"{'='*60}\n")

    results = []
    start = time.monotonic()

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {}
        for i in range(num_connections):
            f = pool.submit(test_vless, vless_host, vless_port,
                            target_host, target_port, conn_id=i+1)
            futures[f] = i + 1

        for f in as_completed(futures):
            conn_id = futures[f]
            try:
                r = f.result()
                results.append(r)
                status = "OK" if r['success'] else f"FAIL({r['error']})"
                print(f"  [Conn {conn_id:3d}] {status}  "
                      f"{r['response_size']:6d} bytes  "
                      f"{r['elapsed']:.2f}s")
            except Exception as e:
                results.append({'id': conn_id, 'success': False,
                                'error': str(e), 'response_size': 0,
                                'elapsed': 0})
                print(f"  [Conn {conn_id:3d}] EXCEPTION: {e}")

    total_elapsed = time.monotonic() - start

    # 汇总
    ok_count = sum(1 for r in results if r['success'])
    fail_count = len(results) - ok_count
    avg_time = sum(r['elapsed'] for r in results) / len(results) if results else 0
    total_bytes = sum(r['response_size'] for r in results)

    print(f"\n{'='*60}")
    print(f"  Results: {ok_count}/{len(results)} succeeded, {fail_count} failed")
    print(f"  Total time: {total_elapsed:.2f}s, Avg per conn: {avg_time:.2f}s")
    print(f"  Total data: {total_bytes} bytes")
    if fail_count > 0:
        print(f"  Failed connections:")
        for r in results:
            if not r['success']:
                print(f"    Conn {r['id']}: {r['error']}")
    print(f"{'='*60}\n")

    return fail_count == 0


def run_sequential_test(vless_host, vless_port, target_host, target_port,
                        num_connections):
    """顺序多连接测试（用于对比）"""
    print(f"\n{'='*60}")
    print(f"  Sequential Test: {num_connections} connections")
    print(f"  VLESS: {vless_host}:{vless_port} → Target: {target_host}:{target_port}")
    print(f"{'='*60}\n")

    results = []
    start = time.monotonic()

    for i in range(num_connections):
        r = test_vless(vless_host, vless_port, target_host, target_port, conn_id=i+1)
        results.append(r)
        status = "OK" if r['success'] else f"FAIL({r['error']})"
        print(f"  [Conn {i+1:3d}] {status}  "
              f"{r['response_size']:6d} bytes  "
              f"{r['elapsed']:.2f}s")

    total_elapsed = time.monotonic() - start

    ok_count = sum(1 for r in results if r['success'])
    fail_count = len(results) - ok_count

    print(f"\n{'='*60}")
    print(f"  Results: {ok_count}/{len(results)} succeeded, {fail_count} failed")
    print(f"  Total time: {total_elapsed:.2f}s")
    print(f"{'='*60}\n")

    return fail_count == 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <vless-server:port> <target:port> [num_connections] [mode]")
        print(f"  mode: concurrent (default) | sequential | both")
        sys.exit(1)

    vless = sys.argv[1].rsplit(":", 1)
    target = sys.argv[2].rsplit(":", 1)
    num_conn = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    mode = sys.argv[4] if len(sys.argv) > 4 else "concurrent"

    vless_host, vless_port = vless[0], int(vless[1])
    target_host, target_port = target[0], int(target[1])

    if mode == "sequential":
        ok = run_sequential_test(vless_host, vless_port, target_host, target_port, num_conn)
    elif mode == "both":
        ok1 = run_sequential_test(vless_host, vless_port, target_host, target_port, num_conn)
        ok2 = run_concurrent_test(vless_host, vless_port, target_host, target_port, num_conn)
        ok = ok1 and ok2
    else:
        ok = run_concurrent_test(vless_host, vless_port, target_host, target_port, num_conn)

    sys.exit(0 if ok else 1)
