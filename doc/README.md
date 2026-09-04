# 项目文档索引

> 维护日期：2026-09-03
> 项目：C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）

> 本目录只保留**当前生效的说明性文档**（架构、机制、运维），作为项目对外的说明材料。
> 设计方案、规划、学习笔记、压测与排障记录在 `doc/dev/` —— 本地开发文档库，
> 由 `.gitignore` 忽略、独立纳管，入口见 `dev/README.md`。

## 文档结构

```
doc/
├── README.md                        # 本索引
├── current-architecture.md          # 当前架构速览（新读者第一入口）
├── server-tls-support.md            # 内置 TLS 支持（已实施）
├── uring-op-pointer-convergence.md  # io_uring 桥接层：user_data 指针化 + cancelFd（已实施）
├── server-ops-tuning.md             # 服务端运维调优：TCP BBR、跨境链路（已实施）
├── new-protocol-development-guide.md # 新协议开发指南（开发者扩展入口）
└── dev/                             # 开发文档库（不入本仓库）：方案 / 笔记 / 压测 / 排障
```

## 阅读顺序（面向学习）

```
doc/README.md（本索引）
  └─ doc/current-architecture.md（当前架构速览 ← 建议最先读）
       └─ doc/server-tls-support.md（内置 TLS）
       └─ doc/uring-op-pointer-convergence.md（io_uring 桥接机制）
       └─ doc/server-ops-tuning.md（运维调优）
       └─ doc/new-protocol-development-guide.md（要加新协议时读）
       └─ doc/dev/README.md（设计方案与规划，按需查阅）
       └─ src/ include/（代码）
```

## 当前有效文档

| 文档 | 说明 |
|---|---|
| `current-architecture.md` | 当前架构速览，以代码为准，学习地图 |
| `server-tls-support.md` | 服务端内置 TLS 支持设计（已实施，含订阅展望） |
| `uring-op-pointer-convergence.md` | io_uring user_data 指针化 + cancelFd 取消模型（已实施） |
| `server-ops-tuning.md` | 服务端运维调优：TCP BBR 开启、跨境链路测速与分层定位（已实施） |
| `new-protocol-development-guide.md` | 基于 vless 框架添加新应用层协议的开发者指南（扩展点、步骤、踩坑清单） |

## 配置与部署

- 用户认证：`/etc/vless/config.json`（首次启动自动生成随机 UUID，见 `include/common/config.h`）
- 一键安装：根目录 `install.sh`；构建：`build.sh`
- 详见项目根目录 `README.md`
