# cppvless

C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）。

- **服务端**：VLESS 明文 / 内置 TLS 双端口监听，基于 io_uring 的事件循环 + C++20 协程状态机
- **客户端**：本地 SOCKS5 → 远端 VLESS，用于本机代理转发
- 多线程 worker 模型（SO_REUSEPORT），对用户透明，无需第三方代理框架依赖

## 特性

- **io_uring + 协程**：异步 I/O（TCP/UDP 中继）、协程式读写流、半关闭（copyStream）
- **VLESS 协议**：UUID 认证（多用户）、Vision、X25519+AEAD 加密会话
- **内置 TLS**：正式证书或自签证书保底（自动生成、落盘复用、到期自动重签）
- **配置化**：从文件读取，首次启动自动生成（含随机 UUID）
- **分享链接**：启动日志输出 vless:// 链接与终端二维码，可直接扫码导入客户端

## 快速开始

安装脚本默认从 GitHub Releases 下载预编译产物（amd64/arm64，无需编译工具链），自动启用 TCP BBR：

```bash
git clone https://github.com/snailllllll/cppvless.git
cd cppvless
sudo ./install.sh
# 或单独下载脚本：
curl -fsSL https://raw.githubusercontent.com/snailllllll/cppvless/main/install.sh -o install.sh
sudo bash install.sh
```

安装后即可用服务端日志中输出的分享链接/二维码接入客户端。常用环境变量：`VMESS_PORT`、`VMESS_TLS_PORT`、`VMESS_PUBLIC_HOST`（公网地址，输出分享链接）、`VMESS_ENABLE_BBR=0`（关闭 BBR）、`VMESS_SOURCE=source`（现场编译）。

## 配置

服务端从 `/etc/vmess/config.json` 读取配置（`--config <path>` 或 `VLESS_CONFIG` 覆盖）。**首次启动若文件不存在，自动生成并写入随机 UUID**：

```json
{
  "port": 1080,
  "host": "your.server.com",
  "log_level": "info",
  "workers": 0,
  "tls": {
    "enabled": true,
    "port": 8848,
    "cert_file": "",
    "key_file": "",
    "cert_dir": "/var/lib/vmess/certs",
    "cert_days": 365
  },
  "users": [
    { "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", "name": "default" }
  ]
}
```

配置优先级：命令行参数 > 环境变量（`VLESS_USERS` 追加用户）> 配置文件。命令行选项：`--tls-port/--cert/--key/--cert-dir/--cert-days/--public-host/--log-file`，位置参数 `port loglevel workers`。

### 手动启动服务端

```bash
# 明文 + TLS 双端口（自签证书保底）
vmess_server --config /etc/vmess/config.json --log-file /var/log/vmess.log

# 使用正式证书
vmess_server --config /etc/vmess/config.json \
  --tls-port 8848 --cert /path/fullchain.pem --key /path/privkey.pem
```

### 分享链接与二维码

配置了公网地址（配置文件 `host` 或 `--public-host`）后，启动日志为每个用户输出：

```
Share link[0] (客户端扫码/粘贴导入):
  vless://xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx@your.server.com:8848?encryption=none&security=tls&type=tcp&headerType=none&allowInsecure=1#cppvless
██ ▀▀▀▀▀ █ ███▄ ...   （终端二维码，手机直接扫码）
```

兼容 v2rayN / v2rayNG / Shadowrocket / Clash 等客户端。Docker 部署直接 `docker logs vmess | head -30` 查看。

### 客户端

客户端配置 `/etc/vmess/client.json`（可选，缺省用内置默认；`--config` 或 `VLESS_CLIENT_CONFIG` 覆盖）：

```json
{
  "socks5_port": 1080,
  "remote": "SERVER_IP:8848",
  "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "log_level": "info",
  "workers": 0
}
```

```bash
vmess_client --config /etc/vmess/client.json
# 或命令行直接指定
vmess_client --remote SERVER_IP:8848 --uuid <uuid> --socks5-port 1080
```

之后把浏览器/应用的 SOCKS5 代理指向 `127.0.0.1:1080` 即可。

## 构建

```bash
# Docker 内静态构建（推荐，产物跨发行版兼容，与 CI 一致）
./build.sh

# 或本地编译（需要 cmake + g++-12+ + liburing-dev + libssl-dev）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build-docker/src/vmess_server`、`build-docker/src/vmess_client`。

## 目录结构

```
src/
├── main.cpp                  # 服务端入口（配置加载 + 多 worker 事件循环）
├── common/                   # 日志、JSON 配置、分享链接/二维码
├── net/                      # io_uring / socket / TLS 封装
├── coro/                     # 协程流抽象（AsyncStream / BufferedStream）
├── proxy/
│   ├── vless/                # decoder / encoder / validator / vision / encryption
│   └── socks5/               # SOCKS5 协议（客户端用）
├── server/                   # EventLoop + VlessConnection 状态机（握手/连接/中继）
└── client/                   # SOCKS5 → VLESS 客户端
include/                      # 公共头文件
tests/                        # 功能测试（socket echo / HTTP / TLS）
third_party/                  # BLAKE3、qrcodegen 等第三方源码
```

## 文档

| 文档 | 说明 |
|---|---|
| `doc/19-current-architecture.md` | 当前架构速览（学习入口） |
| `doc/18-server-tls-support.md` | 内置 TLS 支持设计 |
| `doc/20-logging-plan.md` | 日志改造方案 |
| `doc/22-server-ops-tuning.md` | 服务端运维调优（TCP BBR 等） |
| `doc/vless-protocol-evolution-log.md` | 协议与运行时演进日志 |
