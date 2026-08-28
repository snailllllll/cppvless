#!/usr/bin/env python3
"""CI/压测用目标 HTTP 服务：任何 GET 返回特征串，便于 e2e 断言。

用法: python3 target_http.py [port]   (默认 18080)
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

BODY = b'hello-vless-stream-abstract'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    HTTPServer(('127.0.0.1', port), Handler).serve_forever()
