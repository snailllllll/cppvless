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
├── 26-benchmark-report.md        # cpp-vless vs Go 性能压测综合报告（v0.0.2，最终结论）
├── 27-double-free-troubleshooting.md  # double-free 崩溃排障全流程（v0.0.2 修复记录）
├── benchmark-data/               # 压测原始逐轮数据 CSV
 └── vless-protocol-evolution-log.md    # 协议与运行时演进日志
```

> `doc/dev/`：本地开发讨论与学习资料（外部知识笔记、早期设计、参考代码、开发期压测过程文档），
> 不入库（见 `.gitignore`）。含：
> - `dev/notes/`   —— 外部知识/语言特性学习笔记
> - `dev/design/`  —— 早期设计/规划（含 history/），与当前实现不符
> - `dev/reference/` —— 外部参考代码仓库（HXLibs、Xray-core 等）
> - `dev/benchmark/` —— 开发期压测过程文档（首轮方法、接管说明、复测记录）与原始数据备份

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
| `26-benchmark-report.md` | **压测综合报告**：环境/方案/脚本/步骤/逐轮数据/结论（最终结论，v0.0.2） |
| `27-double-free-troubleshooting.md` | **double-free 崩溃排障全流程**：复现→gdb→反汇编→根因→修复→验证（v0.0.2） |
| `benchmark-data/` | 压测逐轮原始数据（bench_l1/l2/c100/c200.csv） |
| `vless-protocol-evolution-log.md` | 协议与运行时演进日志 |

## 配置与部署

- 用户认证：`/etc/vmess/config.json`（首次启动自动生成随机 UUID，见 `include/common/config.h`）
- 一键安装：根目录 `install.sh`；构建：`build.sh`
- 详见项目根目录 `README.md`
