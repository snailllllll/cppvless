#!/usr/bin/env bash
# =============================================================================
# build.sh — 固化构建：Docker 容器内编译静态链接二进制
#
# 为什么用 Docker 而非本地裸编译：
#   本机 g++15 产物带 GCC15 libstdc++ 依赖，且存在宿主环境注入库干扰；
#   用 ubuntu:22.04 + g++-12 与 CI 完全一致，产物干净、跨发行版兼容。
#
# 静态化策略：
#   -static-libstdc++ / -static-libgcc 消除 GCC 运行时依赖
#   删除动态 liburing.so 强制 -luring 命中静态库 liburing.a
#   最终产物仅依赖 glibc + OpenSSL3（目标机普遍自带）
#
# 产物：
#   build-docker/src/vless_server
#   build-docker/src/vless_client
#
# 可覆盖环境变量：
#   JOBS  并行编译数（默认 nproc）
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

IMAGE="ubuntu:22.04"
BUILD_DIR="build-docker"
JOBS="${JOBS:-$(nproc)}"

echo "==> 拉取基础镜像 ${IMAGE}"
docker pull "${IMAGE}" >/dev/null 2>&1 || true

echo "==> 容器内编译（静态链接，-j${JOBS}）"
docker run --rm \
  -v "$PWD:/work" -w /work \
  "${IMAGE}" bash -c "
    set -e
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      build-essential g++-12 cmake liburing-dev libssl-dev >/dev/null
    rm -f /usr/lib/x86_64-linux-gnu/liburing.so* /lib/x86_64-linux-gnu/liburing.so*
    rm -rf ${BUILD_DIR}
    cmake -B ${BUILD_DIR} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++-12 \
      -DCMAKE_EXE_LINKER_FLAGS=\"-static-libstdc++ -static-libgcc\" >/dev/null
    cmake --build ${BUILD_DIR} -j${JOBS}
  "

echo
echo "==> 产物"
ls -lh "${BUILD_DIR}/src/vless_server" "${BUILD_DIR}/src/vless_client"

echo
echo "==> 动态依赖（应只剩 glibc + OpenSSL3，无 libstdc++/liburing）"
readelf -d "${BUILD_DIR}/src/vless_server" | grep -E 'NEEDED|RPATH|RUNPATH' || true
echo '---'
readelf -d "${BUILD_DIR}/src/vless_client" | grep -E 'NEEDED|RPATH|RUNPATH' || true
