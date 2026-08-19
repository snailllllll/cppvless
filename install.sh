#!/usr/bin/env bash
# =============================================================================
# cppvless — 一键安装 / 部署脚本
#
# 用法：
#   方式一（推荐）：clone 仓库后在仓库内运行
#     git clone https://github.com/snailllllll/cppvless.git
#     cd cppvless
#     sudo ./install.sh
#
#   方式二（单独下载脚本运行）：
#     curl -fsSL https://raw.githubusercontent.com/snailllllll/cppvless/main/install.sh -o install.sh
#     sudo bash install.sh
#
# 行为（参考测试环境部署链路：Docker 静态构建 → 安装二进制 → 生成配置 → 启动）：
#   1) 准备源码（已在仓库内则复用当前目录；否则 clone 到 /opt/cppvless）
#   2) 编译服务端（优先 Docker 内静态构建，产物仅依赖 glibc+OpenSSL3；
#      无 Docker 时用系统包管理器装工具链后本地编译）
#   3) 生成 /etc/vmess/config.json（幂等：首次生成随机 UUID，重复安装复用）
#   4) 启动服务（Docker host 网络 + privileged；无 Docker 用 systemd / nohup）
#   5) 打印 VLESS 连接信息与客户端用法
#
# 可覆盖环境变量：
#   VMESS_PORT        明文端口（默认 1080）
#   VMESS_TLS_PORT    TLS 端口（默认 8848，自签证书保底）
#   VMESS_LOG_LEVEL   日志等级（默认 info）
#   VMESS_USE_DOCKER  auto / 1 / 0（默认 auto：有可用 Docker 则用）
#   VMESS_WORKDIR     源码 clone 目录（默认 /opt/cppvless）
# =============================================================================
set -euo pipefail

REPO_URL="https://github.com/snailllllll/cppvless.git"
REPO_BRANCH="main"

WORKDIR="${VMESS_WORKDIR:-/opt/cppvless}"
BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/vmess"
CONFIG_FILE="${CONFIG_DIR}/config.json"
CERT_DIR="/var/lib/vmess/certs"
LOG_FILE="/var/log/vmess.log"
SERVICE_NAME="vmess_server"

PORT="${VMESS_PORT:-1080}"
TLS_PORT="${VMESS_TLS_PORT:-8848}"
LOG_LEVEL="${VMESS_LOG_LEVEL:-info}"

info() { echo -e "\033[1;32m[install]\033[0m $*"; }
warn() { echo -e "\033[1;33m[warn]\033[0m $*"; }
err()  { echo -e "\033[1;31m[error]\033[0m $*" >&2; }

# ── 0. root 检查 ────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
  err "请以 root 运行：sudo bash install.sh"
  exit 1
fi

# ── 1. 准备源码 ─────────────────────────────────────────────────────────────
if [ -f CMakeLists.txt ] && grep -q "project(vmess" CMakeLists.txt 2>/dev/null; then
  SRC_DIR="$(pwd)"
  info "检测到已在 cppvless 源码目录内：${SRC_DIR}"
else
  SRC_DIR="${WORKDIR}/cppvless"
  if [ ! -d "${SRC_DIR}" ]; then
    info "clone 源码：${REPO_URL}"
    command -v git >/dev/null 2>&1 || { err "缺少 git，请先安装"; exit 1; }
    git clone --depth 1 -b "${REPO_BRANCH}" "${REPO_URL}" "${SRC_DIR}"
  else
    info "复用已有源码：${SRC_DIR}"
  fi
fi
cd "${SRC_DIR}"

# ── 2. 确定构建方式 ────────────────────────────────────────────────────────
HAS_DOCKER=0
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  HAS_DOCKER=1
fi
USE_DOCKER=0
case "${VMESS_USE_DOCKER:-auto}" in
  1) USE_DOCKER=1 ;;
  0) USE_DOCKER=0 ;;
  auto) [ "${HAS_DOCKER}" -eq 1 ] && USE_DOCKER=1 ;;
esac

# ── 3. 构建 ─────────────────────────────────────────────────────────────────
if [ "${USE_DOCKER}" -eq 1 ]; then
  info "使用 Docker 内静态构建（与 CI 一致，产物跨发行版兼容）"
  bash ./build.sh
  SERVER_BIN="build-docker/src/vmess_server"
else
  info "本地编译（Docker 不可用，使用系统工具链）"
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq
    apt-get install -y --no-install-recommends \
      build-essential g++-12 cmake liburing-dev libssl-dev pkg-config >/dev/null
    CXX_CMD="g++-12"
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y gcc gcc-c++ cmake make liburing-devel openssl-devel
    CXX_CMD="g++"
  elif command -v yum >/dev/null 2>&1; then
    yum install -y gcc gcc-c++ cmake make liburing-devel openssl-devel
    CXX_CMD="g++"
  else
    err "不支持的包管理器，请手动安装 cmake/g++/liburing-dev/libssl-dev 后重试"
    exit 1
  fi
  if ! command -v "${CXX_CMD}" >/dev/null 2>&1; then
    CXX_CMD="g++"
  fi
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX_CMD}"
  cmake --build build -j"$(nproc)"
  SERVER_BIN="build/src/vmess_server"
fi

# ── 4. 安装二进制 ──────────────────────────────────────────────────────────
info "安装二进制 → ${BIN_DIR}/vmess_server"
install -m 0755 "${SERVER_BIN}" "${BIN_DIR}/vmess_server"

# ── 5. 生成 / 复用配置（幂等）──────────────────────────────────────────────
mkdir -p "${CONFIG_DIR}" "${CERT_DIR}"
gen_uuid() {
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import uuid; print(uuid.uuid4())'
  elif [ -r /proc/sys/kernel/random/uuid ]; then
    cat /proc/sys/kernel/random/uuid
  else
    od -An -N16 -tx1 /dev/urandom | tr -d ' \n' | \
      sed -E 's/^(.{8})(.{4})(.{4})(.{4})(.{12})$/\1-\2-\3-\4-\5/'
  fi
}

if [ ! -f "${CONFIG_FILE}" ]; then
  UUID="$(gen_uuid)"
  info "生成配置文件：${CONFIG_FILE}（UUID=${UUID}）"
  cat > "${CONFIG_FILE}" <<EOF
{
  "port": ${PORT},
  "log_level": "${LOG_LEVEL}",
  "workers": 0,
  "tls": {
    "enabled": true,
    "port": ${TLS_PORT},
    "cert_file": "",
    "key_file": "",
    "cert_dir": "${CERT_DIR}",
    "cert_days": 365
  },
  "users": [
    {"uuid": "${UUID}", "name": "default"}
  ]
}
EOF
else
  info "复用已有配置：${CONFIG_FILE}"
fi
chmod 600 "${CONFIG_FILE}"

# ── 6. 启动服务 ─────────────────────────────────────────────────────────────
if [ "${USE_DOCKER}" -eq 1 ]; then
  info "以 Docker 启动（host 网络 + privileged，io_uring 需要）"
  cp "${SERVER_BIN}" ./vmess_server
  docker build -t cppvless:latest . >/dev/null
  rm -f ./vmess_server
  docker rm -f "${SERVICE_NAME}" >/dev/null 2>&1 || true
  docker run -d --name "${SERVICE_NAME}" \
    --network host \
    --privileged \
    --restart unless-stopped \
    -v "${CONFIG_DIR}:/etc/vmess" \
    -v "${CERT_DIR}:${CERT_DIR}" \
    cppvless:latest >/dev/null
  info "容器 ${SERVICE_NAME} 已启动"
else
  if command -v systemctl >/dev/null 2>&1; then
    info "以 systemd 服务启动 ${SERVICE_NAME}"
    cat > "/etc/systemd/system/${SERVICE_NAME}.service" <<EOF
[Unit]
Description=CPPVLESS VLESS Server
After=network-online.target

[Service]
ExecStart=${BIN_DIR}/vmess_server --config ${CONFIG_FILE} --log-file ${LOG_FILE}
Restart=on-failure
RestartSec=3
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --now "${SERVICE_NAME}" >/dev/null 2>&1 || true
    systemctl restart "${SERVICE_NAME}"
  else
    info "以 nohup 后台启动"
    pkill -x vmess_server 2>/dev/null || true
    sleep 1
    setsid "${BIN_DIR}/vmess_server" --config "${CONFIG_FILE}" --log-file "${LOG_FILE}" \
      >/tmp/vmess.out 2>&1 </dev/null &
  fi
fi

# ── 7. 健康检查 ─────────────────────────────────────────────────────────────
sleep 2
PORT_OK=$(ss -tln 2>/dev/null | grep -c ":${PORT} " || true)
TLS_OK=$(ss -tln 2>/dev/null | grep -c ":${TLS_PORT} " || true)
[ "${PORT_OK}" -ge 1 ] && info "明文端口 ${PORT} 监听正常" || warn "明文端口 ${PORT} 未监听（查看日志：${LOG_FILE}）"
[ "${TLS_OK}" -ge 1 ] && info "TLS 端口 ${TLS_PORT} 监听正常" || warn "TLS 端口 ${TLS_PORT} 未监听（查看日志：${LOG_FILE}）"

# ── 8. 输出连接信息 ─────────────────────────────────────────────────────────
UUID="$(sed -n 's/.*"uuid": *"\([^"]*\)".*/\1/p' "${CONFIG_FILE}" | head -1)"
echo
info "════════════ 部署完成 ════════════"
info "服务器:    ${BIN_DIR}/vmess_server"
info "配置文件:  ${CONFIG_FILE}"
info "明文端口:  ${PORT}（VLESS）"
info "TLS 端口:  ${TLS_PORT}（VLESS + TLS，自签证书）"
info "UUID:      ${UUID}"
info ""
info "VLESS 订阅链接（把 SERVER_IP 换成你的服务器公网 IP）："
echo
echo "  vless://${UUID}@SERVER_IP:${TLS_PORT}?encryption=none&security=tls&type=tcp#cppvless"
echo
info "本机客户端启动（SOCKS5 → VLESS）："
echo
echo "  ${BIN_DIR}/vmess_client --remote SERVER_IP:${TLS_PORT} --uuid ${UUID} --socks5-port 1080"
echo
info "══════════════════════════════════"
