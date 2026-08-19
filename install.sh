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
# 行为：
#   1) 获取服务端二进制：
#       默认 VMESS_SOURCE=binary —— 从 GitHub Releases 下载预编译产物
#       （amd64/arm64，静态链接，仅依赖 glibc + OpenSSL3），SHA256 校验
#       可选 VMESS_SOURCE=source —— 现场编译（Docker 静态构建或系统工具链）
#   2) 可选启用 TCP BBR（参考 doc/22-server-ops-tuning.md），默认开启
#   3) 生成 /etc/vmess/config.json（幂等：首次生成随机 UUID，重复安装复用）
#   4) 启动服务（Docker host 网络 + privileged；无 Docker 用 systemd / nohup）
#   5) 打印 VLESS 连接信息与客户端用法
#
# 可覆盖环境变量：
#   VMESS_SOURCE      binary（默认，下载预编译产物）/ source（现场编译）
#   VMESS_ENABLE_BBR  1（默认，启用 TCP BBR）/ 0（跳过）
#   VMESS_PORT        明文端口（默认 1080）
#   VMESS_TLS_PORT    TLS 端口（默认 8848，自签证书保底）
#   VMESS_LOG_LEVEL   日志等级（默认 info）
#   VMESS_USE_DOCKER  auto / 1 / 0（默认 auto：有可用 Docker 则用容器启动）
#   VMESS_PUBLIC_HOST 公网地址（域名/IP）；生成配置文件 host 字段，
#                     部署后服务端日志输出 vless:// 分享链接与二维码
#   VMESS_WORKDIR     源码 clone 目录（默认 /opt/cppvless，仅 source 模式用）
# =============================================================================
set -euo pipefail

REPO_URL="https://github.com/snailllllll/cppvless.git"
REPO_BRANCH="main"
RELEASE_BASE="https://github.com/snailllllll/cppvless/releases/latest/download"

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
SOURCE_MODE="${VMESS_SOURCE:-binary}"
ENABLE_BBR="${VMESS_ENABLE_BBR:-1}"

info() { echo -e "\033[1;32m[install]\033[0m $*"; }
warn() { echo -e "\033[1;33m[warn]\033[0m $*"; }
err()  { echo -e "\033[1;31m[error]\033[0m $*" >&2; }

# 统一清理临时目录（EXIT 时执行，避免 trap 覆盖导致残留）
_CLEANUP_DIRS=()
trap 'for d in "${_CLEANUP_DIRS[@]:-}"; do rm -rf "$d"; done' EXIT

# ── 0. root 检查 ────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
  err "请以 root 运行：sudo bash install.sh"
  exit 1
fi

# ── 1. 启用 TCP BBR（可选，默认开启）──────────────────────────────────────
enable_bbr() {
  [ "${ENABLE_BBR}" != "1" ] && { info "跳过 BBR 优化（VMESS_ENABLE_BBR=0）"; return; }
  info "启用 TCP BBR 拥塞控制..."
  # 内核支持检查（modprobe 失败不中断，模块可能已内置）
  if ! modprobe tcp_bbr 2>/dev/null; then
    if ! grep -qw bbr /proc/sys/net/ipv4/tcp_available_congestion_control 2>/dev/null; then
      warn "内核不支持 tcp_bbr，跳过 BBR 优化（不影响安装）"
      return
    fi
  fi
  sysctl -w net.ipv4.tcp_congestion_control=bbr >/dev/null 2>&1 \
    || { warn "设置 BBR 失败，跳过（不影响安装）"; return; }
  sysctl -w net.core.default_qdisc=fq >/dev/null 2>&1 || true
  # 持久化（重启仍生效）
  cat > /etc/sysctl.d/99-bbr.conf <<'EOF'
net.ipv4.tcp_congestion_control=bbr
net.core.default_qdisc=fq
EOF
  info "BBR 已启用并持久化（/etc/sysctl.d/99-bbr.conf）"
}
enable_bbr

# ── 2. 获取二进制 ───────────────────────────────────────────────────────────
case "${SOURCE_MODE}" in
  binary)
    # 检测架构 → 下载预编译产物
    case "$(uname -m)" in
      x86_64|amd64) DL_ARCH="amd64" ;;
      aarch64|arm64) DL_ARCH="arm64" ;;
      *)
        err "不支持的架构 $(uname -m)：仅发布 amd64/arm64 预编译产物，请改用 VMESS_SOURCE=source"
        exit 1
        ;;
    esac
    TARBALL="cppvless-linux-${DL_ARCH}.tar.gz"
    TMP_DL="$(mktemp -d)"
    _CLEANUP_DIRS+=("${TMP_DL}")
    info "下载预编译产物（${DL_ARCH}）: ${RELEASE_BASE}/${TARBALL}"
    command -v curl >/dev/null 2>&1 || { err "缺少 curl，请先安装"; exit 1; }
    curl -fsSL -o "${TMP_DL}/${TARBALL}" "${RELEASE_BASE}/${TARBALL}" \
      || { err "下载失败，请确认仓库已发布 v* tag 的 Release；或改用 VMESS_SOURCE=source"; exit 1; }
    curl -fsSL -o "${TMP_DL}/${TARBALL}.sha256" "${RELEASE_BASE}/${TARBALL}.sha256" \
      || { err "下载校验文件失败"; exit 1; }
    # SHA256 校验
    EXPECTED="$(cat "${TMP_DL}/${TARBALL}.sha256")"
    ACTUAL="$(sha256sum "${TMP_DL}/${TARBALL}" | awk '{print $1}')"
    if [ "${EXPECTED}" != "${ACTUAL}" ]; then
      err "SHA256 校验失败（期望 ${EXPECTED}，实际 ${ACTUAL}），中止安装"
      exit 1
    fi
    tar -xzf "${TMP_DL}/${TARBALL}" -C "${TMP_DL}"
    SERVER_BIN="${TMP_DL}/vmess_server"
    CLIENT_BIN="${TMP_DL}/vmess_client"
    info "预编译产物校验通过"
    ;;

  source)
    # 现场编译
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

    if [ "${USE_DOCKER}" -eq 1 ]; then
      info "使用 Docker 内静态构建（与 CI 一致，产物跨发行版兼容）"
      bash ./build.sh
      SERVER_BIN="build-docker/src/vmess_server"
      CLIENT_BIN="build-docker/src/vmess_client"
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
      CLIENT_BIN="build/src/vmess_client"
    fi
    ;;
  *)
    err "未知 VMESS_SOURCE=${SOURCE_MODE}（可选 binary / source）"
    exit 1
    ;;
esac

# ── 3. 安装二进制 ──────────────────────────────────────────────────────────
info "安装 vmess_server → ${BIN_DIR}/"
install -m 0755 "${SERVER_BIN}" "${BIN_DIR}/vmess_server"
if [ -f "${CLIENT_BIN}" ]; then
  install -m 0755 "${CLIENT_BIN}" "${BIN_DIR}/vmess_client"
  info "安装 vmess_client → ${BIN_DIR}/"
fi

# ── 4. 生成 / 复用配置（幂等）──────────────────────────────────────────────
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
  "host": "${VMESS_PUBLIC_HOST:-}",
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
  if [ -n "${VMESS_PUBLIC_HOST:-}" ]; then
    info "已设置公网地址 host=${VMESS_PUBLIC_HOST}，启动日志将输出分享链接与二维码"
  fi
else
  info "复用已有配置：${CONFIG_FILE}"
fi
chmod 600 "${CONFIG_FILE}"

# ── 5. 启动服务 ─────────────────────────────────────────────────────────────
# 二进制模式无源码上下文，用临时目录 + 内嵌 Dockerfile 构建镜像
USE_DOCKER=0
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  case "${VMESS_USE_DOCKER:-auto}" in
    1) USE_DOCKER=1 ;;
    0) USE_DOCKER=0 ;;
    auto) USE_DOCKER=1 ;;
  esac
fi

if [ "${USE_DOCKER}" -eq 1 ]; then
  info "以 Docker 启动（host 网络 + privileged，io_uring 需要）"
  TMP_CTX="$(mktemp -d)"
  _CLEANUP_DIRS+=("${TMP_CTX}")
  cp "${BIN_DIR}/vmess_server" "${TMP_CTX}/"
  cat > "${TMP_CTX}/Dockerfile" <<'EOF'
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    liburing2 \
    && rm -rf /var/lib/apt/lists/*
COPY vmess_server /usr/local/bin/vmess_server
RUN chmod +x /usr/local/bin/vmess_server
VOLUME ["/etc/vmess"]
EXPOSE 1080/tcp 8848/tcp
ENTRYPOINT ["/usr/local/bin/vmess_server"]
CMD ["--config", "/etc/vmess/config.json"]
EOF
  docker build -t cppvless:latest "${TMP_CTX}" >/dev/null
  docker rm -f "${SERVICE_NAME}" >/dev/null 2>&1 || true
  docker run -d --name "${SERVICE_NAME}" \
    --network host \
    --privileged \
    --restart unless-stopped \
    -v "${CONFIG_DIR}:/etc/vmess" \
    -v "${CERT_DIR}:${CERT_DIR}" \
    cppvless:latest >/dev/null
  info "容器 ${SERVICE_NAME} 已启动"
elif command -v systemctl >/dev/null 2>&1; then
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

# ── 6. 健康检查 ─────────────────────────────────────────────────────────────
sleep 2
PORT_OK=$(ss -tln 2>/dev/null | grep -c ":${PORT} " || true)
TLS_OK=$(ss -tln 2>/dev/null | grep -c ":${TLS_PORT} " || true)
[ "${PORT_OK}" -ge 1 ] && info "明文端口 ${PORT} 监听正常" || warn "明文端口 ${PORT} 未监听（查看日志：${LOG_FILE}）"
[ "${TLS_OK}" -ge 1 ] && info "TLS 端口 ${TLS_PORT} 监听正常" || warn "TLS 端口 ${TLS_PORT} 未监听（查看日志：${LOG_FILE}）"

# ── 7. 输出连接信息 ─────────────────────────────────────────────────────────
UUID="$(sed -n 's/.*"uuid": *"\([^"]*\)".*/\1/p' "${CONFIG_FILE}" | head -1)"
PUBLIC_HOST="$(sed -n 's/.*"host": *"\([^"]*\)".*/\1/p' "${CONFIG_FILE}" | head -1)"
echo
info "════════════ 部署完成 ════════════"
info "服务器:    ${BIN_DIR}/vmess_server"
info "配置文件:  ${CONFIG_FILE}"
info "明文端口:  ${PORT}（VLESS）"
info "TLS 端口:  ${TLS_PORT}（VLESS + TLS，自签证书）"
info "UUID:      ${UUID}"
if [ -n "${PUBLIC_HOST}" ]; then
  info "分享链接（客户端扫码/粘贴导入，服务端日志亦会输出 + 二维码）："
  echo
  echo "  vless://${UUID}@${PUBLIC_HOST}:${TLS_PORT}?encryption=none&security=tls&type=tcp&headerType=none&allowInsecure=1#cppvless"
  echo
fi
info "本机客户端启动（SOCKS5 → VLESS）："
echo
echo "  ${BIN_DIR}/vmess_client --remote ${PUBLIC_HOST:-SERVER_IP}:${TLS_PORT} --uuid ${UUID} --socks5-port 1080"
echo
info "══════════════════════════════════"
