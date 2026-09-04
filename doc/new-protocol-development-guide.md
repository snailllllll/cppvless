---
title: 基于 vless 框架添加新应用层协议指南
updated: 2026-09-03
---

# 基于 vless 框架添加新应用层协议指南

> 面向开发者的**自顶向下**指南：介绍框架提供的设施与抽象（EventLoop / Connection / 工厂 / 点火机制），以及如何基于它们为服务器添加新的应用层协议。
> 与既有文档的分工：底层机制（协程 Task、io_uring awaitable、挂起/恢复）见文末索引，本文不重复；本文回答"**我要加一个协议，要写什么、复用什么、避开什么坑**"。

---

## 1. 结论先行

**添加一个新应用层协议 = 实现 1 个接口 + 提供 1 个工厂 + 在 main 里组装。**

```text
1. 实现 EventLoopConnection 接口（4 个虚函数）
2. 在其中写一个协程状态机（用现成的 Task / Stream / awaitable 积木）
3. 写一个 ConnectionFactory lambda 把它接入 EventLoop
4. main() 里 runEventLoops() 启动
```

其余全部复用：多 worker 线程模型、accept、CQE 分发、连接回收、信号处理、TLS、日志。框架已经用两个真实协议验证过这条路径——服务端 VLESS（`main.cpp`）与客户端 SOCKS5（`client_main.cpp`）**共用同一套 EventLoop 代码**，差异只有一个工厂函数。新协议的最小实现量约 200–400 行。

## 2. 设施全景：自顶向下的分层

```text
┌─ main.cpp / client_main.cpp ── 组装层：配置 + 工厂 + 启动 ──────────────┐
│      │  ConnectionFactory（你的协议在这里注册）                          │
├─ event_loop_runner ────────── 多 worker 线程 + 信号 + 错误传播 ──────────┤
├─ event_loop.cpp ───────────── accept 协程 + CQE 分发 + 连接回收 ─────────┤
│      │  只认识 EventLoopConnection 接口，不认识任何协议                   │
├─ 你的协议连接（VlessConnection / Socks5Connection / 你的 XxxConnection）─┤
│      │  协程状态机：Handshake → Dispatch → Relay → Cleanup               │
├─ Stream 抽象（net/stream.h）── read / writeFull / shutdownWrite ─────────┤
│      │  AsyncStream（明文） / TlsStream（TLS 包装）实现同一接口           │
├─ 协程 + io_uring 基础设施 ────────────────────────────────────────────────┤
│   Task<T>（task.h）· awaitable（uring_awaitable.h）·                     │
│   AsyncStream/BufferedStream（coro/）· IoUring（net/io_uring.h）          │
└──────────────────────────────────────────────────────────────────────────┘
```

各层的所有权与"你需要动吗"：

| 层 | 文件 | 职责 | 加新协议时 |
|---|---|---|---|
| 组装层 | `src/main.cpp`、`src/client/client_main.cpp` | 配置加载、工厂构造、`runEventLoops` 启动 | 新增入口或改工厂 |
| 运行器 | `server/event_loop_runner.{h,cpp}` | 每 loop 一线程、SIGINT/SIGTERM、worker 异常统一传播 | 不动 |
| 事件循环 | `server/event_loop.{h,cpp}` | accept 协程、CQE→协程零查表分发、`cleanupClosedConnections` 回收 | 不动 |
| **连接接口** | `server/connection.h` | `EventLoopConnection` 抽象 | **你的协议实现它** |
| 协议连接 | `server/vless_connection*`、`client/socks5_connection*` | 协程状态机 + 双向中继 | **新增（主体工作）** |
| 流抽象 | `net/stream.h`、`coro/async_stream.*`、`coro/buffered_stream.*` | `net::Stream` 接口 + 缓冲精确读 | 不动，直接用 |
| 协程/IO 设施 | `coro/task.h`、`coro/uring_awaitable.h`、`net/io_uring.h` | Task、7 种 awaitable、ring 封装 | 不动，直接用 |
| TLS | `net/tls_stream.*`、`net/tls.*` | `TlsStream` 包装任意 `Stream` | 不动（可选） |
| 协议参考 | `proxy/vless/*`、`proxy/socks5/*` | 现有协议的 Decoder/Encoder/Parser | 作为解析层范本 |

## 3. 三个扩展点逐一拆解

### 3.1 `EventLoopConnection`：主循环唯一认识的接口

```text
// include/server/connection.h
class EventLoopConnection {
public:
    virtual ~EventLoopConnection() = default;
    virtual void start() = 0;              // 启动会话状态机（点火协程）
    virtual bool isClosed() const = 0;     // 可被主循环回收的标志
    virtual int  primaryFd() const = 0;    // 主 fd（连接查找）
    virtual bool hasFd(int fd) const { return fd == primaryFd(); }  // 反向查找（双向连接有第二个 fd）
};
```

四个方法的契约与生命周期：

```text
acceptLoop（协程）                     EventLoop 主循环
    │ accept 得到 clientFd               │
    ▼                                    │
factory_(clientFd, uring) ──▶ 你的连接对象
    │                                    │
conn->start()  ← 点火，立刻返回           │
    │                                    │
connections_[clientFd] = conn ───────────▶│ 每轮尾部：
                                         │   conn->isClosed() == true ?
                                         │     → erase → 析构 → fd 关闭
```

契约要点：

1. **`start()` 必须点火后立刻返回**——它在 accept 协程里被调用，阻塞它会卡住后续 accept；
2. **`isClosed()` 是回收的唯一依据**——由你的状态机在所有协程结束后置位（见 §5.4）；
3. **`hasFd()` 只有双向连接需要覆写**——VlessConnection 持有 clientFd + targetFd 两个 fd。

### 3.2 `ConnectionFactory`：协议的"插件注册处"

```text
// include/server/event_loop.h
using ConnectionFactory =
    std::function<std::unique_ptr<EventLoopConnection>(int fd, net::IoUring&)>;
```

同一个 EventLoop，换工厂即换协议——框架的两个入口就是证明：

```text
// 服务端（main.cpp）：工厂捕获 Validator 与 TLS 上下文
auto makeFactory = [&validator](SSL_CTX* tls) {
    return [&validator, tls](int clientFd, vless::net::IoUring& uring) {
        return std::make_unique<vless::server::VlessConnection>(
            clientFd, uring, validator, tls);
    };
};

// 客户端（client_main.cpp）：工厂捕获客户端配置，产出 SOCKS5 连接
loops.push_back(std::make_unique<vless::server::EventLoop>(
    [cfg](int clientFd, vless::net::IoUring& uring) {
        return std::make_unique<vless::client::Socks5Connection>(
            clientFd, uring, cfg);
    }));
```

模式：**工厂是捕获配置的闭包**——每个 accept 到的 fd 都用同一份配置构造连接对象；需要协议级状态（如用户认证表 `Validator`、TLS 上下文）就捕获进去，连接级状态（fd、协程）留在连接对象里。

### 3.3 `start()` 点火协议：连接如何获得并发

框架约定每个连接对象**持有自己的协程 Task 成员**并手动点火（不是被 co_await）：

```text
// 参考实现：VlessConnection::start()
void start() {
    if (closed_) return;
    clientTask_ = clientTask();      // 创建协程帧（suspend_always，零副作用）
    if (!clientTask_.done()) {
        clientTask_.h.resume();      // 点火：跑至第一个挂起点，控制权回到 acceptLoop
    }
}
```

为什么必须是点火而非 co_await：`co_await` 是调用语义（acceptLoop 会等这条连接跑完才能 accept 下一个，串行化）；点火是并发语义（spawn 后立刻返回，协程独立存活，由 CQE 唤醒）。这是框架并发的唯一来源。

机制细节（Task 的构造/挂起/恢复、为何 initial_suspend 必须 suspend_always、Task 为何必须存成员）见文末索引文档，此处只记结论。

## 4. 实战：添加一个新协议的步骤

以自定义协议 "XProto" 为例（握手头 + 数据中继的骨架，可直接照抄替换协议逻辑）。

### Step 1：协议解析层（纯函数，参考 `proxy/vless/decoder.cpp`）

```text
// src/proxy/xproto/decoder.h —— 解析层也是协程，依赖 BufferedStream 精确读
coro::Task<XprotoRequest> Decoder::decode(coro::BufferedStream& stream) {
    auto magic = co_await stream.read(2);            // 精确读 N 字节
    if (magic.size() != 2 || magic[0] != 'X' || magic[1] != 'P')
        throw std::runtime_error("bad magic");       // 解析失败 = 抛异常
    auto addr = co_await readAddress(stream);        // 子协程
    co_return addr;
}
```

要点：解析层只依赖 `BufferedStream`（"精确读 N 字节"能力），不关心底层是明文还是 TLS；错误用 `throw` 表达，由连接状态机顶层 `try/catch` 接住。

### Step 2：协议连接类（主体工作）

```text
// include/server/xproto_connection.h
class XprotoConnection : public EventLoopConnection {
public:
    XprotoConnection(int clientFd, net::IoUring& uring, /* 你的协议配置 */);
    void start() override;
    bool isClosed() const override { return closed_; }
    int  primaryFd() const override { return clientFd_; }
private:
    coro::Task<void> clientTask();           // 会话状态机主控
    coro::Task<bool> runSession();           // 握手 → 中继
    coro::Task<void> peerTask(int peerFd);   // 反方向中继（若需要双向）
    void finishClientTask();                 // 统一清理（见 §5.4）

    int clientFd_;
    net::IoUring& uring_;
    std::unique_ptr<coro::AsyncStream> rawStream_;   // 明文流（唯一持有）
    std::unique_ptr<net::TlsStream> tlsStream_;      // 可选 TLS 包装
    net::Stream* clientStream_;                      // 当前激活流视图
    coro::UringBufferedStream stream_;               // 握手阶段缓冲流
    coro::Task<void> clientTask_;
    coro::Task<void> peerTask_;
    bool closed_ = false;
    bool peerTaskStarted_ = false;
};
```

```text
// src/server/xproto_connection.cpp —— 状态机
coro::Task<void> XprotoConnection::clientTask() {
    try {
        // Phase 0: 可选 TLS 握手（与 VlessConnection 相同的模式）
        // Phase 1: 协议握手
        auto req = co_await Xproto::Decoder::decode(stream_);
        // Phase 2: 建连目标（异步 connect）
        int peerFd = co_await connectPeer(req);
        if (peerFd < 0) { finishClientTask(); co_return; }
        // Phase 3: 点火反方向协程（兄弟协程，不是子协程）
        peerTaskStarted_ = true;
        peerTask_ = peerTask(peerFd);
        if (!peerTask_.done()) peerTask_.h.resume();
        // Phase 4: 转发握手预读数据 + 本方向中继
        co_await coro::copyStream(peerStream, *clientStream_, closed_);
    } catch (const std::exception& e) {
        LOG_ERROR("XprotoConnection", "clientTask exception: ", e.what());
    }
    finishClientTask();
}

void XprotoConnection::start() {
    clientTask_ = clientTask();
    if (!clientTask_.done()) clientTask_.h.resume();
}
```

### Step 3：main 组装

```text
// 新入口或扩展现有 main
vless::server::EventLoop::ConnectionFactory factory =
    [cfg](int clientFd, vless::net::IoUring& uring) {
        return std::make_unique<vless::server::XprotoConnection>(clientFd, uring, cfg);
    };

std::vector<std::unique_ptr<vless::server::EventLoop>> loops;
for (unsigned i = 0; i < workerCount; ++i)
    loops.push_back(std::make_unique<vless::server::EventLoop>(factory));
std::vector<uint16_t> ports(workerCount, listenPort);

std::string err;
return vless::server::runEventLoops(loops, ports, workerCount > 1, &err);
```

`runEventLoops` 已包含：每 loop 一线程（`SO_REUSEPORT` 共享端口）、SIGINT/SIGTERM 优雅停止、worker 异常捕获与传播。全部免费获得。

### Step 4（可选）：TLS

构造时包一层即可，协议流程零改动：

```text
rawStream_ = std::make_unique<coro::AsyncStream>(clientFd, uring);
if (tlsCtx) {
    tlsStream_ = std::make_unique<net::TlsStream>(*rawStream_, tlsCtx, /*isServer=*/true);
    clientStream_ = tlsStream_.get();     // 握手阶段先 co_await tlsStream_->handshake()
} else {
    clientStream_ = rawStream_.get();
}
stream_ = coro::UringBufferedStream(*clientStream_);
```

## 5. 协议状态机的通用模式（从 VlessConnection 提炼）

### 5.1 四段式主控

```text
Handshake（解析协议头，UringBufferedStream 精确读，错误 throw）
   → Dispatch（按命令分发：TCP/UDP/其他）
   → Relay（双向中继）
   → Cleanup（finishClientTask 统一收尾）
```

响应先于建连（对齐 Xray inbound）：先回协议响应再 connect 目标，避免慢 connect 导致客户端 RST。

### 5.2 双向中继：两个兄弟协程 + 半关闭传播

一条连接两个方向各一个协程（`clientTask` + `peerTask`），中继复用框架的 `copyStream`（`include/coro/async_stream.h`）：

```text
inline Task<bool> copyStream(net::Stream& dst, net::Stream& src, const bool& stop) {
    while (!stop) {
        auto rr = co_await src.read();
        if (rr.eof())  { co_await dst.shutdownWrite(); co_return true; }   // 半关闭传播
        if (rr.error()){ co_await dst.shutdownWrite(); co_return false; }
        if (co_await dst.writeFull(rr.data) <= 0) co_return false;
    }
    co_return false;
}
```

若协议需要逐包加工（加密/流控），手写 while 循环替代 `copyStream`，检测到可直通后降级调用它（参考 `relayClientToTarget` 的 Vision 分支）。

### 5.3 握手预读（粘包处理）

握手解析可能多读（对端已开始发数据）：`stream_.drainRemaining()` 取出暂存，建连成功后先转发给对端（`forwardHandshakeRemaining` 模式），再进入中继循环。

### 5.4 收尾与回收协议

```text
finishClientTask():
    clientReadDone_ = true;
    对端协程未启动？           → closed_ = true（立刻可回收）
    对端还在跑？               → ::shutdown(peerFd, SHUT_RDWR) 唤醒它，等双方结束
    双方都结束（两标志皆 true）→ closed_ = true
析构 doClose()（无条件执行）:
    PendingUringOps::cancelFd(两个 fd)   ← 迟到 CQE 变 no-op，防 UAF
    ::close(peerFd); ::close(clientFd); 置 -1（幂等）
```

### 5.5 现成积木清单

| 积木 | 位置 | 用途 |
|---|---|---|
| `AsyncStream` | `coro/async_stream.*` | `read()/writeFull()/shutdownWrite()`，Go net.Conn 语义 |
| `copyStream` | `coro/async_stream.h` | 单向中继 + 半关闭传播，一行接入 |
| `UringBufferedStream` | `coro/buffered_stream.*` | 协议解析用"精确读 N 字节"（read/readByte/drainRemaining） |
| `AsyncConnect/Accept/Recv/Send/Shutdown/RecvFrom/SendTo` | `coro/uring_awaitable.h` | 全部异步 IO 原语 |
| `TlsStream` | `net/tls_stream.*` | 任意 Stream 的 TLS 包装 |
| `EventLoop` + `runEventLoops` | `server/` | accept/调度/回收/多线程/信号 |

## 6. 踩坑清单（每条都对应真实机制）

| # | 坑 | 根因与解法 |
|---|---|---|
| 1 | Task 存局部变量，连接挂起后崩溃 | Task 析构 destroy 协程帧。**挂起中的协程 Task 必须存成员**，帧生命周期跟随连接对象 |
| 2 | 构造函数里 resume 协程 | 对象尚未构造完成，协程访问成员即 UB。点火只在 `start()`（acceptLoop 调用）里做 |
| 3 | 中继循环吞掉非 EOF 错误 | 错误不传播 → 链不解 → 资源泄漏。**非 EOF 错误必须沿链传播到 finishClientTask** |
| 4 | 一侧结束后对端协程永远挂起 | 挂在 recv 上的协程只能被数据/FIN/错误唤醒。**放弃一侧必须 shutdown 对端 fd**（finishClientTask 模式） |
| 5 | 析构漏掉 cancelFd 或重复 close | CQE 迟到指向已释放 op 即 UAF。doClose 无条件执行：先 cancelFd 再 close，置 -1 保幂等 |
| 6 | 协程里调 `getaddrinfo` 等阻塞函数 | 阻塞整个 EventLoop 线程，所有连接停摆。DNS 解析目前是已知妥协点（见 fd-idle-timeout-design 关联规划） |
| 7 | `isClosed()` 过早置 true | 对端协程还在跑就被 erase → 协程帧悬空。**两方向都结束才置位**（clientReadDone_ && targetReadDone_） |
| 8 | 双向连接只报 primaryFd | cleanup 按 fd 查找/回收，`hasFd()` 不覆写会导致 targetFd 相关查找失效 |

## 7. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 协议接入方式 | 实现 `EventLoopConnection` + 工厂闭包 | 主循环零协议知识；双端（VLESS/SOCKS5）已验证复用 |
| 连接启动方式 | 手动 resume（点火）而非 co_await | 并发语义：acceptLoop 不被单连接阻塞；co_await 是调用语义会串行化 |
| 流抽象 | `net::Stream` 接口 + AsyncStream/TlsStream 双实现 | 协议代码与传输（明文/TLS）解耦，TLS 只是换实现 |
| 解析层 | 协程 + `BufferedStream` 精确读 + throw 报错 | 解析写成直线代码，错误沿链传播到状态机顶层 catch |
| 双向中继 | 每方向一个协程 + `copyStream` 复用 | 全双工独立推进；半关闭传播靠 TCP 语义，无锁 |

## 8. 相关文档索引

| 主题 | 文档 |
|---|---|
| 现有架构总览 | `current-architecture.md` |
| io_uring 指针直分发模型 | `uring-op-pointer-convergence.md` |
| 协程 Task 构造 / 挂起恢复 / AsyncRecv vs Task（外部笔记） | `cpp-playground/doc/vless-coro-*.md`（已发布至笔记平台） |
