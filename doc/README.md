# 项目文档索引

> 维护日期：2026-08-15
> 项目：C++20 + io_uring + 协程实现的 VLESS 代理（服务端 + SOCKS5 客户端）

## 阅读顺序（面向学习）

```
doc/README.md（本索引）
  └─ doc/19-current-architecture.md（当前架构速览 ← 建议最先读，学习地图）
       └─ doc/18-server-tls-support.md（进行中的 TLS 支持设计）
       └─ doc/20-logging-plan.md（进行中的日志改造方案）
       └─ doc/vless-protocol-evolution-log.md（协议演进日志，最贴近现状）
       └─ src/ include/（代码）
```

## 文档分类与状态

状态说明：
- `有效` —— 与当前代码实现一致，可放心阅读
- `部分过时` —— 主体有参考价值，但标注的章节/描述与当前代码不符，读时以代码为准
- `已归档` —— 早期设计/规划，与当前实现不符，仅作历史参考（已移至 `doc/history/`）
- `学习参考` —— 外部知识/语言特性/背景资料，不描述本项目实现

### 当前有效

| 文档 | 说明 |
|---|---|
| `vless-protocol-evolution-log.md` | 协议与运行时演进日志，与现状一致（未含 SOCKS5 客户端/Encoder/SHA-256 三条最新演进，见文末） |
| `18-server-tls-support.md` | 服务端内置 TLS 支持设计（进行中，含订阅/动态节点展望） |

### 部分过时（读时以代码为准）

| 文档 | 主题 | 过时点 |
|---|---|---|
| `architecture.md` | 当前架构（v2.0，最贴近现状） | §4.3 Connection 接口（实际为 EventLoopConnection 工厂模式）、§7 客户端/多线程/用户管理状态表 |
| `vless_coroutine_architecture.md` | 协程 + io_uring 服务端设计 | 核心思想（UringBufferedStream/drainRemaining/copyStream 半关闭）已落地；`VlessHeaderDecoder` 状态机与 `coroutineMap` 未采用 |
| `vless_protocol_parser_design.md` | VLESS 请求头解析设计 | 解析流程有效；`VlessHeaderDecoder` 接口未采用；addons 已实现 flow/seed 解析（文档假设跳过） |
| `06-multi-proactor-design.md` | 多 Proactor 方案 | §5 推荐的 eventfd 方案未采纳；**实际采用 §4.4 的 SO_REUSEPORT 方案**（与现状一致） |
| `08-protocol-architecture-design.md` | 协议与框架解耦/工厂 | 解耦+工厂思想保留（即当前 EventLoopConnection+工厂）；`Protocol`/`Connection` 类、同步握手、VLESS 无加密结论已过时 |

### 学习参考（外部知识，不描述本项目实现）

| 文档 | 主题 |
|---|---|
| `01-vmess-protocol.md` | VMess 协议规范（本项目是 VLESS，两者不同） |
| `02-go-implementation-reference.md` | Go 官方 VLESS/VMess 实现参考 |
| `03-cpp26-features.md` | C++26 特性 |
| `09-cpp-lambda-expression.md` | C++ Lambda 表达式 |
| `09-hxlibs-coroutine-io_uring-analysis.md` | HXLibs 协程 io_uring 分析 |
| `10-coro-epoll-kqueue-analysis.md` | 协程 epoll/kqueue 分析 |
| `10-cpp-multithreading.md` | C++ 多线程 |
| `13-co-uring-webserver-analysis.md` | co-uring webserver 分析 |
| `14-project-comparison-analysis.md` | 项目对比分析 |
| `15-uring-exec-vs-io_uring-examples-cpp.md` | uring_exec vs io_uring 示例 |
| `17-ai-coding-guidelines.md` | AI 编码规范 |

### 已归档（doc/history/，早期设计/规划，文件头已标注状态）

| 文档 | 原因 |
|---|---|
| `history/04-system-design.md` | VMess 系统设计（非 VLESS），std::execution/YAML 均未采用 |
| `history/05-advanced-design.md` | C++ 协程通用理论笔记，与项目架构无对应 |
| `history/07-code-architecture.md` | 早期模块蓝图（Config/Session/Scheduler 等均不存在） |
| `history/11-protocol-design-and-socks5.md` | `Protocol` 抽象接口不存在；SOCKS5 格式章节仍可参考 |
| `history/12-v2ray-proxy-interface-design.md` | Inbound/Outbound/Dispatcher 接口不存在 |
| `history/16-practical-development-plan.md` | 早期开发规划（VMess 时代） |
| `history/io_uring_design.md` | 旧回调式 + Buffer Selection 设计，已改为纯协程注册表驱动 |
| `history/vless_server_architecture.md` | 旧式 prepareIO/onIOComplete 回调事件循环，已删除 |

## 当前代码结构速查（学习时对照）

```
src/
├── main.cpp                     # 服务端入口（VLESS 明文）
├── net/                         # io_uring / socket 封装
├── coro/                        # 协程基础（Task/Async* 原语）[实际在 include/coro + src? 见注]
├── proxy/
│   ├── vless/                   # 协议：decoder/encoder/validator/vision/encryption
│   └── socks5/                  # SOCKS5 协议解析（客户端用）
├── server/                      # EventLoop + VlessConnection 状态机
└── client/                      # SOCKS5 客户端（Socks5Connection/UdpRelay/client_main）
```

> 注：协程头文件位于 `include/coro/`，实现为模板/内联；具体以 `19-current-architecture.md` 为准。
