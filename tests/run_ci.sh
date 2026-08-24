#!/usr/bin/env bash
# CI 功能测试入口：构建 + 起服务 + 三个测试（在 ubuntu:22.04 容器内执行）
# 用法: bash tests/run_ci.sh
set -e
export DEBIAN_FRONTEND=noninteractive

echo "== install deps =="
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential g++-12 cmake liburing-dev libssl-dev python3 >/dev/null

echo "== build =="
# 强制静态链接 liburing（避免运行期符号名冲突），复用 build.sh 方法
rm -f /usr/lib/x86_64-linux-gnu/liburing.so* /lib/x86_64-linux-gnu/liburing.so*
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" >/dev/null
cmake --build build -j"$(nproc)"
ls -la build/src/vmess_server build/src/vmess_client build/tests/test_socket_echo

echo "== start target HTTP service =="
python3 tests/target_http.py 18080 &
TARGET_PID=$!
sleep 1

echo "== start vmess server (plain 1080 + tls 8848) =="
UUID=$(cat /proc/sys/kernel/random/uuid)
echo "Test UUID: $UUID"
mkdir -p /tmp/vmess
cat > /tmp/vmess/config.json <<CFG
{
  "port": 1080,
  "host": "127.0.0.1",
  "log_level": "warn",
  "workers": 0,
  "tls": {"enabled": true, "port": 8848, "cert_file": "", "key_file": "", "cert_days": 30},
  "users": [{"uuid": "${UUID}", "name": "test"}]
}
CFG
./build/src/vmess_server --config /tmp/vmess/config.json &
SERVER_PID=$!
sleep 2

echo "== test 1: socket echo =="
./build/tests/test_socket_echo

echo "== test 2: vless http (plain, direct VLESS) =="
VLESS_TEST_UUID=$UUID python3 tests/vless_http_test.py \
  127.0.0.1:1080 127.0.0.1:18080 20 concurrent

echo "== test 3: vless tls =="
VLESS_TEST_UUID=$UUID python3 tests/vless_tls_test.py \
  127.0.0.1:8848 127.0.0.1:18080

echo "== ALL TESTS PASSED =="
kill "$SERVER_PID" "$TARGET_PID"
