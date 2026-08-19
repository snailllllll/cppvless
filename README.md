# cppvless

C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）。

- **服务端**：VLESS 明文 / 内置 TLS 双端口监听，基于 io_uring 的事件循环 + C++20 协程状态机
- **客户端**：本地 SOCKS5 → 远端 VLESS，用于本机代理转发
- 单机可承载多 worker（SO_REUSEPORT），无需第三方代理框架依赖

## 特性

- VLESS 协议：请求头解析 / 编码 / UUID 认证（多用户）/ Vision / X25519+AEAD 加密会话
- 传输层：io_uring 异步 I/O（TCP/UDP 中继）、协程式读写流、半关闭（copyStream）
- 内置 TLS：正式证书（`--cert/--key`）或自签证书保底（自动生成、落盘复用、到期自动重签）
- 配置化：`/etc/vmess/config.json` 从文件读取，首次启动自动生成（含随机 UUID）
- 轻量异步日志：后台线程批量落盘，背压丢弃防日志风暴

## 快速开始（一键安装）

从 GitHub 仓库获取脚本后即可运行（参考测试环境部署链路：Docker 静态构建 → 安装 → 生成配置 → 启动）：

```bash
# 方式一：clone 仓库后运行
git clone https://github.com/snailllllll/cppvless.git
cd cppvless
sudo ./install.sh

# 方式二：单独下载脚本运行
curl -fsSL https://raw.githubusercontent.com/snailllllll/cppvless/main/install.sh -o install.sh
sudo bash install.sh
```

安装脚本会：

1. 编译服务端（优先 Docker 内静态构建，产物跨发行版兼容；无 Docker 时用系统工具链本地编译）
2. 生成 `/etc/vmess/config.json`（首次运行自动生成随机 UUID，重复安装复用）
3. 启动服务：Docker（host 网络 + privileged）或 systemd / nohup
4. 输出 VLESS 连接信息（UUID、明文端口 1080、TLS 端口 8848）

可用环境变量：`VMESS_PORT`、`VMESS_TLS_PORT`、`VMESS_LOG_LEVEL`、`VMESS_USE_DOCKER`（auto/1/0）、`VMESS_WORKDIR`。

## 配置

服务端从 `/etc/vmess/config.json` 读取配置（可用 `--config <path>` 或环境变量 `VLESS_CONFIG` 覆盖）。**首次启动时若文件不存在，自动生成默认配置并写入**（`users` 中自动生成随机 UUID）。

```json
{
  "port": 1080,
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

字段说明：

| 字段 | 含义 | 默认 |
|---|---|---|
| `port` | 明文 VLESS 端口 | 1080 |
| `log_level` | 日志等级 debug/info/warn/error | info |
| `workers` | worker 线程数，0 = CPU 核数 | 0 |
| `tls.enabled` | 是否启用内置 TLS 端口 | false |
| `tls.port` | TLS 端口 | 8848 |
| `tls.cert_file` / `tls.key_file` | 正式证书与私钥（PEM），成对配置 | 空 = 自签保底 |
| `tls.cert_dir` | 自签证书落盘目录 | ./certs |
| `tls.cert_days` | 自签证书有效期（天） | 365 |
| `users[]` | 认证用户列表（UUID + 备注名） | 首启生成随机 UUID |

**配置优先级**：命令行参数 > 环境变量 > 配置文件。

- `VLESS_USERS` 环境变量：逗号分隔的 UUID 列表，追加到配置文件的 `users`
- 命令行：`--tls-port/--cert/--key/--cert-dir/--cert-days/--log-file` 以及位置参数 `port loglevel workers` 均可覆盖配置

### 服务端手动启动

```bash
# 明文 + TLS 双端口（自签证书保底）
/usr/local/bin/vmess_server --config /etc/vmess/config.json --log-file /var/log/vmess.log

# 使用正式证书
/usr/local/bin/vmess_server --config /etc/vmess/config.json \
  --tls-port 8848 --cert /path/fullchain.pem --key /path/privkey.pem
```

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
/usr/local/bin/vmess_client --config /etc/vmess/client.json
# 或命令行直接指定
/usr/local/bin/vmess_client --remote SERVER_IP:8848 --uuid <uuid> --socks5-port 1080
```

之后把浏览器/应用的 SOCKS5 代理指向 `127.0.0.1:1080` 即可。

## 手动构建

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
├── common/                   # 日志、JSON 配置读写
├── net/                      # io_uring / socket / TLS 封装
├── coro/                     # 协程流抽象（AsyncStream / BufferedStream）
├── proxy/
│   ├── vless/                # decoder / encoder / validator / vision / encryption
│   └── socks5/               # SOCKS5 协议（客户端用）
├── server/                   # EventLoop + VlessConnection 状态机（握手/连接/中继）
└── client/                   # SOCKS5 → VLESS 客户端
include/                      # 公共头文件
tests/                        # 功能测试（socket echo / HTTP / TLS）
third_party/                  # BLAKE3 等第三方源码
```

## 文档

| 文档 | 说明 |
|---|---|
| `doc/README.md` | 文档索引与分类（当前有效 / 本地开发学习） |
| `doc/19-current-architecture.md` | 当前架构速览（学习入口） |
| `doc/18-server-tls-support.md` | 内置 TLS 支持设计 |
| `doc/20-logging-plan.md` | 日志改造方案 |
| `doc/21-uring-op-pointer-convergence.md` | io_uring 桥接层收敛 |
| `doc/22-server-ops-tuning.md` | 服务端运维调优（TCP BBR 等） |
| `doc/vless-protocol-evolution-log.md` | 协议与运行时演进日志 |

## 许可

内部项目，无开源许可声明。
