# 28. 跨平台异步 IO 规划（io_uring 之外的 Windows / macOS）

> 状态：**已搁置**（2026-08-31 决策）
> 日期：2026-08-31
> 背景：当前异步 IO 模型为 Linux 5.1+ 的 io_uring。本文评估将其抽象为统一上层
>        Proactor 语义、下层多后端实现（io_uring / epoll / kqueue / IOCP）的改造方案，
>       使客户端可移植到 macOS、Windows 及老版本 Linux。
> 关联文档：`19-current-architecture.md`、`21-uring-op-pointer-convergence.md`
>
> **搁置原因与决策**：项目核心设计大量直接围绕 `fd` 实现（裸 `int fd` + POSIX 调用
> 散布约 60 处、`PendingUringOps` 按 fd 索引取消、EventLoop 按 fd 管理连接生命周期），
> 跨平台迁移成本显著大于收益。**客户端多平台适配暂时搁置，主线保持为
> 代理协议服务端（VLESS server，Linux + io_uring）**。
> 本文保留作为未来重启该方向时的评估基线；届时第 3 节的 `IoBackend` 接口草案
> 与第 5 节的实施顺序可直接复用。

---

## 1. 现状盘点：可复用与强绑定的边界

### 1.1 平台无关，零改动复用

| 层 | 说明 |
|---|---|
| `coro/task.h` 协程体系 | 纯 C++20 标准，无平台依赖 |
| `net/stream.h` 抽象 + `coro/buffered_stream.h` | 只依赖 `co_await` 语义 |
| `coro/async_stream.h`（`read/writeFull/shutdownWrite`） | 语义平台无关，仅内部 awaitable 需换实现 |
| 协议层（proxy/decoder、encoder、vision、encryption） | 纯字节流逻辑 |
| TLS（net/tls_stream，OpenSSL） | OpenSSL 本身跨平台 |
| 会话状态机（VlessConnection / Socks5Connection） | 除 fd 操作外零改动 |

### 1.2 强绑定 Linux / io_uring，需改造

| 位置 | 绑定点 |
|---|---|
| `include/coro/uring_awaitable.h` | 7 个 awaitable 直接调 `io_uring_prep_*`、`uring_.ring()` |
| `include/net/io_uring.h` | `IoUring` 类（submitAndWait / processCompletions） |
| `src/server/event_loop.cpp` | `submitAndWait` + `SO_REUSEPORT` + `fcntl` |
| 应用层 ~60 处 | 裸 `int fd` + `::close/::shutdown/fcntl/setsockopt/getaddrinfo`（12 个文件） |
| `PendingUringOps` 取消模型 | 语义可抽象，实现需按后端重写 |
| 协程内同步 `getaddrinfo` | 所有平台均阻塞 EventLoop 线程（Linux 上已是隐患） |

核心结论：**应用层已经完全不碰 SQE/CQE，只消费 `co_await` 语义**（见 21 号文档的收敛过程），
因此"抽取引擎接口"本身不改任何行为，风险低；真正的改造大头是散落的 fd 直接调用。

## 2. 各平台异步 IO 设施盘点

| 平台 | 原生 Proactor（发起即完成通知） | Reactor（就绪通知） | 选型 |
|---|---|---|---|
| Linux 5.1+ | io_uring（现状） | epoll | io_uring 后端 |
| Linux 老内核 / 容器 | 无 | epoll + 非阻塞 | Reactor(epoll) 后端 |
| macOS / BSD | 无（POSIX AIO 仅限文件） | kqueue | Reactor(kqueue) 后端 |
| Windows | IOCP（WSARecv/WSASend + OVERLAPPED） | WSAPoll（性能差，不取） | IOCP 后端 |

结论：不存在一套 API 覆盖三平台。业界标准解法（Asio 同构，Asio 现已支持 io_uring）：

> 上层统一 **Proactor 接口**，下层三种实现：
> - io_uring —— Linux 原生 Proactor；
> - epoll / kqueue —— Reactor 模拟 Proactor（等就绪 → 执行非阻塞 syscall → 回调）；
> - IOCP —— Windows 原生 Proactor。

本项目 `AsyncRecv` 等的"发起操作 → 完成时 resume 协程"本来就是 Proactor 语义，
上层无需改变。

## 3. 目标架构：IoBackend 抽象

### 3.1 接口草案

```cpp
// 每个 op 内嵌"完成上下文"（沿用 UringOp 内嵌于 awaitable 的设计），
// 完成时由后端调用 op->onComplete(res) → resume 协程。
class IoBackend {
public:
    virtual void submitRecv(RecvOp* op, SocketHandle s, void* buf, size_t len) = 0;
    virtual void submitSend(WriteOp* op, SocketHandle s, const void* buf, size_t len) = 0;
    virtual void submitConnect(WriteOp* op, SocketHandle s, const sockaddr* a, socklen_t l) = 0;
    virtual void submitAccept(AcceptOp* op, SocketHandle listen) = 0;
    virtual void submitShutdown(WriteOp* op, SocketHandle s, int how) = 0;
    virtual void submitRecvFrom(RecvOp* op, SocketHandle s, ...) = 0;   // UDP
    virtual void submitSendTo(WriteOp* op, SocketHandle s, ...) = 0;    // UDP
    virtual void cancel(SocketHandle s) = 0;      // 对应 PendingUringOps::cancelFd
    virtual int  poll(int timeoutMs) = 0;         // 对应 submitAndWait + processCompletions
};
```

### 3.2 各后端 op 存放方式（均与现有"指针直分发"同构）

| 后端 | op 关联机制 |
|---|---|
| io_uring | `SQE.user_data = &op`（现状） |
| epoll / kqueue | `epoll_event.udata = &op` / `kevent.udata = &op` |
| IOCP | op 内嵌 `OVERLAPPED`，`CONTAINING_RECORD` 反推 op（与 `from_promise` 同模式） |

### 3.3 分层示意

```text
┌─ 应用层（协程状态机，不变）────────────────┐
├─ coro/awaitable（不变，只改"向谁提交"）──────┤
├─ IoBackend 接口（新增）─────────────────────┤
│   UringBackend │ ReactorBackend(epoll|kqueue) │ IocpBackend
├─ SocketHandle + 平台包装（新增）────────────┤
│   int fd + POSIX   │   SOCKET + WinSock2
└──────────────────────────────────────────────┘
```

## 4. 五个隐藏地雷（比写后端本身更耗时）

1. **fd 类型不统一**：Windows `SOCKET` 是 `UINT_PTR` 非 `int`；`close/fcntl/shutdown`
   对应 `closesocket/ioctlsocket`。需将 ~60 处裸 `int fd` 收拢为 `SocketHandle` + 平台包装。
   **工作量最大的一项，纯机械改造。**
2. **取消语义不同**：io_uring 靠"迟到 CQE 变 no-op"；IOCP 靠 `CancelIoEx` /
   关 socket 触发 `ERROR_OPERATION_ABORTED`；kqueue/epoll 是就绪模型，取消 =
   摘除事件 + 处理"已就绪未执行"。`cancelFd` 语义需三端分别实现。
3. **IOCP 的 accept/connect 特化**：`AcceptEx`（预提供缓冲区）+
   `SO_UPDATE_ACCEPT_CONTEXT` + 新 socket 显式关联完成端口；`ConnectEx` 需动态加载函数指针。
4. **`SO_REUSEPORT` Windows 没有**：per-worker EventLoop + reuseport 模型需改为
   "共享监听 socket + IOCP 线程池"；`runEventLoops` 需平台分支。
5. **同步 `getaddrinfo`**：当前在协程内直接调用会卡死整个 EventLoop 线程。
   需异步 DNS（Windows `GetAddrInfoEx`，或统一线程池封装）。

## 5. 工作量评估（单人，含测试）

| 阶段 | 内容 | 工时 |
|---|---|---|
| P1 | 抽 `IoBackend` 接口 + `SocketHandle` 句柄抽象 + 平台包装层；现有 io_uring 下沉为实现（不改行为） | 2 周 |
| P2 | Reactor 骨架 + epoll 后端（顺带获得老内核 / 容器 fallback） | 1 周 |
| P3 | kqueue 后端（macOS，复用 Reactor 骨架） | 1 周 |
| P4 | IOCP 后端（Windows：SOCKET 抽象、AcceptEx/ConnectEx、取消、线程池模型） | 3–4 周 |
| P5 | 应用层 fd 调用改造 + `runEventLoops` 平台分支 + 异步 DNS | 1–2 周 |
| P6 | 三平台 CI + 性能回归（对照 io_uring 基线）+ 文档 | 1–2 周 |
| **合计** | | **9–12 周** |

风险排序：IOCP > 异步 DNS > 取消语义 > 多 worker 模型。
两人并行（后端实现 + 句柄抽象改造）可压至 5–6 周。

## 6. 实施顺序建议

1. **先做 P1**：接口抽象不改行为、可随时验证，是一切前提；
2. **再做 P2/P3**：成本低、立刻获得 macOS + 老 Linux 覆盖，同时以实战验证接口是否够用；
3. **最后做 P4**：IOCP 工作量最大，但做完 P2 后对接口已有认知，可少走弯路。

## 7. 遗留问题

- `include/net/socket.h` 中旧版同步 `Socket` 封装（`ServerSocket::accept` 等）当前应用层已不使用。
  句柄抽象时需决定其去留，避免两套 socket 封装并存。
- 是否引入第三方（如 Asio）vs 自研：本文按自研评估（保持学习价值与零依赖）；
  若求速度，Asio 已完成全部四后端且支持协程（awaitable/boost.cobalt），可作为对照参考实现。
