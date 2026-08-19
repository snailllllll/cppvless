# 项目文档索引

> 维护日期：2026-08-19
> 项目：C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）

## 文档结构

```
doc/
├── README.md                     # 本索引
├── 19-current-architecture.md    # 当前架构速览（新读者第一入口）
├── 18-server-tls-support.md      # 内置 TLS 支持设计（已实施）
├── 20-logging-plan.md            # 日志改造方案（阶段 1 已实施）
├── 21-uring-op-pointer-convergence.md  # io_uring 桥接层收敛（已实施）
├── 22-server-ops-tuning.md       # 服务端运维调优（TCP BBR，已实施）
└── vless-protocol-evolution-log.md    # 协议与运行时演进日志
```

> `doc/dev/`：本地开发讨论与学习资料（外部知识笔记、早期设计、参考代码），
> 不入库（见 `.gitignore`）。含：
> - `dev/notes/`   —— 外部知识/语言特性学习笔记
> - `dev/design/`  —— 早期设计/规划（含 history/），与当前实现不符
> - `dev/reference/` —— 外部参考代码仓库（HXLibs、Xray-core 等）

## 阅读顺序（面向学习）

```
doc/README.md（本索引）
  └─ doc/19-current-architecture.md（当前架构速览 ← 建议最先读）
       └─ doc/18-server-tls-support.md（内置 TLS）
       └─ doc/20-logging-plan.md（异步日志）
       └─ doc/vless-protocol-evolution-log.md（协议演进，最贴近现状）
       └─ src/ include/（代码）
```

## 当前有效文档

| 文档 | 说明 |
|---|---|
| `19-current-architecture.md` | 当前架构速览，以代码为准，学习地图 |
| `18-server-tls-support.md` | 服务端内置 TLS 支持设计（已实施，含订阅展望） |
| `20-logging-plan.md` | 日志改造方案（阶段 1 已实施） |
| `21-uring-op-pointer-convergence.md` | io_uring user_data 指针化 + cancelFd 取消模型（已实施） |
| `22-server-ops-tuning.md` | 服务端运维调优：TCP BBR 开启、跨境链路测速与分层定位（已实施） |
| `vless-protocol-evolution-log.md` | 协议与运行时演进日志 |

## 配置与部署

- 用户认证：`/etc/vmess/config.json`（首次启动自动生成随机 UUID，见 `include/common/config.h`）
- 一键安装：根目录 `install.sh`；构建：`build.sh`
- 详见项目根目录 `README.md`
