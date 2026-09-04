# 项目文档索引

> 维护日期：2026-09-03
> 项目：C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）

> 本目录是项目的说明性文档（架构、机制、运维与扩展指南），以当前代码为准。

## 文档结构

```
doc/
├── README.md                        # 本索引
├── current-architecture.md          # 项目架构导览（以代码为准）
├── server-tls-support.md            # 内置 TLS 支持（已实施）
├── uring-op-pointer-convergence.md  # io_uring 桥接层：user_data 指针化 + cancelFd（已实施）
├── server-ops-tuning.md             # 服务端运维调优：TCP BBR、跨境链路（已实施）
└── new-protocol-development-guide.md # 新协议开发指南（开发者扩展入口）
```

## 阅读顺序

```
doc/README.md（本索引）
  └─ doc/current-architecture.md（架构导览 ← 建议最先读）
       └─ doc/server-tls-support.md（内置 TLS）
       └─ doc/uring-op-pointer-convergence.md（io_uring 桥接机制）
       └─ doc/server-ops-tuning.md（运维调优）
       └─ doc/new-protocol-development-guide.md（要加新协议时读）
       └─ src/ include/（代码）
```

## 当前有效文档

| 文档 | 说明 |
|---|---|
| `current-architecture.md` | 项目架构导览，以代码为准，供开发者快速定位各模块 |
| `server-tls-support.md` | 服务端内置 TLS 支持设计（已实施，含订阅展望） |
| `uring-op-pointer-convergence.md` | io_uring user_data 指针化 + cancelFd 取消模型（已实施） |
| `server-ops-tuning.md` | 服务端运维调优：TCP BBR 开启、跨境链路测速与分层定位（已实施） |
| `new-protocol-development-guide.md` | 基于 vless 框架添加新应用层协议的开发者指南（扩展点、步骤、踩坑清单） |

## 配置与部署

- 用户认证：`/etc/vless/config.json`（首次启动自动生成随机 UUID，见 `include/common/config.h`）
- 一键安装：根目录 `install.sh`；构建：`build.sh`
- 详见项目根目录 `README.md`
