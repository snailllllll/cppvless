# vmess 项目架构文档

> 版本: v2.0 | 最后更新: 2026-05-06

---

## 一、项目概览

本项目是一个基于 **C++20 协程 + Linux io_uring** 的高性能代理服务器，当前实现了 **VLESS 协议**（支持 Vision 流控和 Encryption 端到端加密），对标 Go 语言 Xray-core 的核心功能。

| 技术栈 | 选型 |
|--------|------|
| 语言标准 | C++20（`-fcoroutines`） |
| 异步 I/O | Linux io_uring（liburing） |
| 协程框架 | C++20 Coroutine + 自定义 Task/Awaitable |
| 密码学 | OpenSSL 3.0（X25519 / AES-256-GCM / ChaCha20-Poly1305） |
| 哈希 | BLAKE3（含 SIMD 优化） |
| 构建系统 | CMake 3.20+ |
| 命名空间 | `vmess::` |

---

## 二、模块层次与职责

```
┌────────────────────────────────────────────────────────────────────┐
│                          main.cpp (入口)                           │
│   解析参数 → 设置日志 → 注册信号 → EventLoop::run()                 │
└───────────────────────────┬────────────────────────────────────────┘
                            │
┌───────────────────────────▼────────────────────────────────────────┐
│                     server/ (服务层)                                │
│  ┌──────────────┐  ┌──────────────────────┐  ┌──────────────────┐ │
│  │  EventLoop   │  │  VlessConnection     │  │  Connection      │ │
│  │  事件循环     │  │  VLESS 连接处理      │  │  (抽象接口,遗留)  │ │
│  └──────┬───────┘  └──────────┬───────────┘  └──────────────────┘ │
└─────────┼─────────────────────┼────────────────────────────────────┘
          │                     │
          │         ┌───────────▼──────────────┐
          │         │   proxy/vless/ (协议层)   │
          │         │  ┌────────┐ ┌─────────┐ │
          │         │  │Decoder │ │Protocol │ │
          │         │  │协议解码 │ │数据结构  │ │
          │         │  └────────┘ └─────────┘ │
          │         │  ┌────────┐ ┌──────────┐│
          │         │  │Vision  │ │Encryption││
          │         │  │流控处理 │ │端到端加密 ││
          │         │  └────────┘ └──────────┘│
          │         └──────────────────────────┘
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                     coro/ (协程基础设施层)                         │
│  ┌────────────┐ ┌──────────────┐ ┌──────────────┐                │
│  │  Task<T>   │ │uring_awaitable│ │async_stream  │                │
│  │ 协程返回类型│ │可等待对象+注册表│ │Go-like 流封装│                │
│  └────────────┘ └──────────────┘ └──────────────┘                │
│  ┌──────────────────┐                                              │
│  │buffered_stream   │  协议解析专用缓冲流                           │
│  └──────────────────┘                                              │
└────────────────────────────────────────────────────────────────────┘
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                     net/ (网络抽象层)                              │
│  ┌────────────┐  ┌────────────┐                                   │
│  │  IoUring   │  │  Socket    │                                   │
│  │ io_uring封装│  │ Socket封装 │                                   │
│  └────────────┘  └────────────┘                                   │
└────────────────────────────────────────────────────────────────────┘
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                third_party/ (第三方依赖)                            │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  blake3/ — BLAKE3 哈希库（C 实现 + SIMD 优化 + C++ 封装）    │  │
│  └────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
          │
┌─────────▼─────────────────────────────────────────────────────────┐
│                     common/ (公共工具)                             │
│  ┌────────────┐                                                   │
│  │   log.h    │  日志系统 (ERROR/WARN/INFO/DEBUG)                  │
│  └────────────┘                                                   │
└────────────────────────────────────────────────────────────────────┘
```

### 各模块详细说明

#### 1. `net/` — 网络抽象层

| 类 | 文件 | 职责 |
|---|---|---|
| `IoUring` | `net/io_uring.h/.cpp` | 封装 liburing，提供 SQE 准备（Accept/Recv/Send/Connect/Shutdown/Close）、批量提交、CQE 遍历、Buffer Selection |
| `Socket` / `ServerSocket` / `ClientSocket` | `net/socket.h/.cpp` | 同步 Socket 操作封装（当前仅用于创建 fd 和 DNS 解析，异步 I/O 全走 io_uring） |

`IoUring` 核心接口：
- **SQE 准备**：`prepareAccept/Recv/Send/Connect/Shutdown/Close` — 只填充 SQE，不提交
- **提交**：`submitAll()` / `submitAndWait(waitNum)` — 提交到内核
- **CQE 处理**：`processCompletions(callback)` — 遍历完成队列并回调

`UringRequest` 编码（旧式，已废弃，仅 `IoUring` 内部使用）：
```
bit 0-31:  fd
bit 32-47: 事件类型 (UringEventType)
bit 48-63: 缓冲区 ID (bid)
```

#### 2. `coro/` — 协程基础设施层

这是项目最核心的基础设施，将 C++20 协程与 io_uring 桥接。

##### 2.1 `Task<T>` — 协程返回类型

```
coro/task.h
```

标准 C++20 协程框架：
- `TaskPromise<T>`：promise_type，持有返回值、异常、continuation 句柄
- `initial_suspend` → `suspend_always`（惰性启动，需手动 resume）
- `final_suspend` → `FinalAwaitable`：如果有 continuation 则 resume 调用者
- `Task<T>` 同时是 Awaitable：`await_suspend` 设置 continuation 并 resume 子协程

```cpp
Task<void> myCoroutine() {
    auto data = co_await someAsyncOp();
    co_return;
}
```

##### 2.2 `CoroutineRegistry` — 协程挂起注册表（核心桥接机制）

```
coro/uring_awaitable.h
```

**单例模式**，管理 `fd + 事件类型 → (Awaitable指针, 协程句柄)` 的映射。

io_uring 主循环收到 CQE 后的桥接流程：
1. 通过 `user_data` 识别是否为协程上下文（bit 63 = 1）
2. 从 `user_data` 解码 `fd` 和 `UringEventType`
3. 调用 `CoroutineRegistry::takeAndResume(fd, type, result)`
4. 注册表取出 Awaitable 指针，设置结果
5. 取出协程句柄，`resume()` 恢复协程

协程 `user_data` 编码格式：
```
bit 63:    固定 1（协程标志）
bit 48-62: 事件类型 (UringEventType)
bit 0-47:  fd
```

##### 2.3 可等待对象（Awaitable）

| 类 | 用途 | 返回值 |
|---|---|---|
| `AsyncAccept` | 异步接受连接 | `int`（新 fd） |
| `AsyncRecv` | 异步接收数据 | `RecvResult`（可区分 EOF/错误/正常） |
| `AsyncSend` | 异步发送数据 | `int`（发送字节数） |
| `AsyncConnect` | 异步连接远端 | `int`（0=成功） |
| `AsyncShutdown` | 异步半关闭 | `int`（0=成功） |

**缓冲区安全设计**：
- `AsyncRecv` 的缓冲区 `buf_` 在协程帧上，send 不会与 recv 共用缓冲区
- `AsyncSend` 将数据拷贝到协程帧上的 `buf_`，send 完成前不会被覆写

**`RecvResult` 三态设计**（对标 Go）：
```cpp
rr.eof()    → result == 0  （对端关闭，正常结束）
rr.error()  → result < 0   （异常错误）
rr.ok()     → result > 0   （正常读取数据）
```

##### 2.4 `AsyncStream` — Go-like 流封装

```
coro/async_stream.h
```

封装 `fd + IoUring`，提供类似 Go `net.Conn` 的高层接口：

| 方法 | 对标 Go | 说明 |
|------|---------|------|
| `read(maxBytes)` | `conn.Read(buf)` | 读取数据，返回 `RecvResult` |
| `writeFull(data, len)` | `conn.Write(data)` | 循环发送直到全部写完 |
| `shutdownWrite()` | `tcpConn.CloseWrite()` | 半关闭写方向（发送 FIN） |

**`copyStream(dst, src, stop)`**：单向数据拷贝，对标 Go `buf.Copy`：
- 读取 EOF → `shutdownWrite()` 传播半关闭
- 读错误 → 仍然 `shutdownWrite()`
- 写错误 → 返回 false

##### 2.5 `UringBufferedStream` — 协议解析缓冲流

```
coro/buffered_stream.h
```

为协议解析设计，支持小量读取：
- `read(1)` / `read(16)` → 从内部 buffer 消费
- buffer 不够 → `co_await AsyncRecv` 循环读取直到数据充足
- `drainRemaining()` → 取出 buffer 中未消费数据（用于握手剩余数据转发）

#### 3. `proxy/vless/` — VLESS 协议层

##### 3.1 `Protocol` — 协议数据结构

```cpp
struct Request {
    uint8_t version;                              // 必须为 0
    std::array<uint8_t, 16> uuid;                 // 用户 UUID
    Command command;                              // TCP/UDP/Mux
    uint16_t port;                                // 目标端口
    std::string flow;                             // 如 "xtls-rprx-vision"
    std::string encryption;                        // 如 "aes-256-gcm"
    std::variant<array<uint8_t,4>,                 // IPv4
                array<uint8_t,16>,                // IPv6
                std::string> address;             // Domain
};
```

##### 3.2 `Decoder` — 请求头解码器

完整翻译 Go 官方 `DecodeRequestHeader` 逻辑，解析流程：

```
版本(1B) → UUID(16B,校验) → Addons(protobuf:Flow+Encryption)
→ 指令(1B) → 端口(2B) → 地址(1+4/1+1+N/1+16)
```

响应编码：`{version, 0x00}`（2 字节）

##### 3.3 `VisionReader` / `VisionWriter` — XTLS Vision 流控

实现 `xtls-rprx-vision` 流控模式：

**VisionReader（上行 client → target）**：移除 padding 帧
- 帧格式：`[UUID(16B,首帧)] [cmd(1B)] [contentLen(2B)] [paddingLen(2B)] [content] [padding]`
- 命令类型：`CONTINUE(0x00)` / `END(0x01)` / `DIRECT(0x02)`
- `DIRECT` 之后切换为直接拷贝模式
- 内含 TLS ClientHello 检测，更新共享 `VisionContext`

**VisionWriter（下行 target → client）**：添加 padding 帧
- TLS 检测：识别 ServerHello → 提取 cipher → 查找 TLS 1.3 supported_versions
- TLS 1.3 Application Data 检测 → `CMD_DIRECT` 或 `CMD_END`
- 非 TLS / TLS 1.2 → 若干包后 `CMD_END`

**VisionContext（共享状态）**：`packetsToFilter` / `isTLS` / `enableXtls` / `cipher` 等

##### 3.4 `EncryptionSession` — VLESS Encryption 端到端加密

完整的密钥交换 + AEAD 加密流程：

1. **X25519 密钥交换**：OpenSSL EVP_PKEY_X25519
2. **BLAKE3 密钥派生**：
   - Context: `"VLESS Encryption: client key"` → 派生 client→server 密钥
   - Context: `"VLESS Encryption: server key"` → 派生 server→client 密钥
3. **AEAD 加密**：
   - `X25519_AES256GCM`：AES-256-GCM (key=32B, nonce=12B, tag=16B)
   - `X25519_Chacha20`：ChaCha20-Poly1305 (key=32B, nonce=12B, tag=16B)
4. **数据格式**：`nonce(12B) + encrypted + tag(16B)`
5. **Nonce 递增**：每个方向独立的 uint64 计数器，小端编码为 12 字节

#### 4. `server/` — 服务层

##### 4.1 `EventLoop` — 事件循环

核心调度器，纯协程版本：
- 所有 CQE 都通过 `CoroutineRegistry` resume 协程
- Accept 也走协程（`co_await AsyncAccept`）
- 不再有旧式回调/状态机路径

```cpp
class EventLoop {
    net::IoUring uring_;
    std::unordered_map<int, std::unique_ptr<VlessConnection>> connections_;
    coro::Task<void> acceptTask_;
};
```

##### 4.2 `VlessConnection` — VLESS 连接处理

**双协程模型**：每个连接两个协程独立处理两个方向

```cpp
class VlessConnection {
    coro::Task<void> clientTask_;    // 握手 + client→target 转发
    coro::Task<void> targetTask_;    // target→client 转发
};
```

**半关闭状态模型**（对标 Go Xray）：
- `clientReadDone_`：client→target 方向 EOF → `shutdown(target, SHUT_RDWR)`
- `targetReadDone_`：target→client 方向 EOF → `shutdown(client, SHUT_RDWR)`
- 两者都 true → 连接完全关闭

**三种转发模式**：

| 模式 | 触发条件 | client→target | target→client |
|------|---------|--------------|--------------|
| 普通 | flow 为空 | `copyStream` | `copyStream` |
| Vision | `xtls-rprx-vision` | read → unpad → write | read → pad → write |
| Encryption | encryption 字段非空 | read → decrypt → write | read → encrypt → write |

##### 4.3 `Connection` — 抽象接口（遗留设计）

定义了 `prepareIO / onIOComplete / isClosed / primaryFd` 等接口，设计用于通过工厂模式支持多协议。当前 `VlessConnection` **未继承**此接口，使用纯协程模型。此接口属旧式设计的遗留。

#### 5. `common/` — 公共工具

| 组件 | 说明 |
|------|------|
| `log.h` | 日志系统，支持 ERROR/WARN/INFO/DEBUG 四级，宏接口 `LOG_ERROR/WARN/INFO/DEBUG` |

#### 6. `third_party/blake3/` — BLAKE3 哈希库

C 实现的 BLAKE3 哈希库，含：
- `blake3_c.c/h`：C 实现（portable）
- `blake3_dispatch.c`：运行时 SIMD 分发
- `blake3_sse2/sse41/avx2/avx512.c`：SIMD 优化版本
- `blake3_wrap.cpp`：C++ 封装，提供 `vmess::crypto::blake3_derive_key()` 和 `blake3_hash()`

---

## 三、一个请求的完整生命周期

以一个 **VLESS + Encryption** 请求为例，展示从 TCP 连接到数据转发的完整流程：

```
                         客户端                     服务端                      目标
                           │                         │                         │
    1. TCP Connect         │──── SYN ───────────────►│                         │
                           │◄─── SYN+ACK ───────────│                         │
                           │──── ACK ───────────────►│                         │
                           │                         │                         │
    2. Accept              │                         │                         │
       EventLoop::acceptLoop()                       │                         │
       co_await AsyncAccept                          │                         │
       → 新 VlessConnection(clientFd)                │                         │
       → conn.start() → clientTask_ 启动            │                         │
                           │                         │                         │
    3. VLESS 请求头       │──── VLESS Header ──────►│                         │
       (协程解析)         │  version(1B)             │                         │
                           │  uuid(16B)              │                         │
       co_await stream.read(1)   版本号              │                         │
       co_await stream.read(16)  UUID校验            │                         │
       co_await parseAddons()   Flow+Encryption       │                         │
       co_await stream.read(1)  指令                  │                         │
       co_await stream.read(2)  端口                  │                         │
       co_await readAddress()   地址                  │                         │
                           │                         │                         │
    4. Encryption 握手    │──── Client PubKey(32B)─►│                         │
       co_await stream.read(32)                      │                         │
       encryptionSession_->generateKeyPair()         │                         │
                           │                         │                         │
    5. 连接目标           │                         │──── Connect ──────────►│
       createTargetSocket()                          │                         │
       co_await AsyncConnect(targetFd, ...)          │◄─── ACK ───────────────│
                           │                         │                         │
    6. VLESS 响应         │◄─── Response(2B) ──────│                         │
       co_await AsyncSend(clientFd, response)        │                         │
                           │                         │                         │
    7. Encryption 完成    │◄─── Server PubKey(32B)──│                         │
       co_await encStream.writeFull(serverPubKey)    │                         │
       encryptionSession_->computeSharedSecret()      │                         │
       → BLAKE3 派生双向密钥 → AEAD 就绪             │                         │
                           │                         │                         │
    8. 启动反向协程        │                         │                         │
       startTargetTask(targetFd)                     │                         │
       → targetTask_ 启动                             │                         │
                           │                         │                         │
    9. 转发握手剩余数据   │                         │──── Plaintext ────────►│
       drainRemaining() → decrypt → writeFull        │                         │
                           │                         │                         │
   10. 双向加密转发       │                         │                         │
       ┌──────────────────────────────────────────────────────────────────────┐
       │ clientTask:                                    targetTask:          │
       │   while (!closed_) {                             while (!closed_) { │
       │     rr = co_await clientStream.read()             rr = co_await    │
       │     decryptClient(rr.data)                         targetStream   │
       │     co_await targetStream.writeFull(plaintext)       .read()        │
       │   }                                               encryptServer(   │
       │                                                     rr.data)      │
       │                                                   co_await         │
       │                                                     clientStream  │
       │                                                     .writeFull()  │
       │                                                 }                   │
       └──────────────────────────────────────────────────────────────────────┘
                           │                         │                         │
   11. 半关闭              │──── Encrypted EOF ─────►│                         │
       clientTask: EOF     │                         │──── FIN ──────────────►│
       → shutdownWrite()   │                         │                         │
       clientReadDone_=true│                         │                         │
                           │                         │                         │
   12. 反向半关闭          │◄─── Encrypted EOF ─────│                         │
       targetTask: EOF     │                         │                         │
       → shutdownWrite()   │                         │                         │
       targetReadDone_=true│                         │                         │
                           │                         │                         │
   13. 连接关闭            │                         │                         │
       closed_ = true      │                         │                         │
       EventLoop 清理连接   │                         │                         │
```

### 普通模式 (Plain VLESS) 的差异

- 步骤 4、7 跳过（无 Encryption 握手）
- 步骤 10 使用 `copyStream` 直接转发

### Vision 模式的差异

- 步骤 4、7 跳过（无 Encryption 握手）
- 步骤 10 使用 VisionReader/Writer 做 padding/unpadding
- 检测到 TLS 1.3 Application Data 后切换为 `copyStream`（direct copy）

---

## 四、io_uring 与协程的协同工作原理

### 4.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                    用户代码（协程）                         │
│                                                          │
│  auto rr = co_await AsyncRecv(fd, uring);               │
│  // 协程在此挂起，控制权返回 EventLoop                     │
│  // ... CQE 到达后 ...                                    │
│  // 协程在此恢复，rr 包含读取的数据                         │
│                                                          │
└──────────────┬───────────────────────────┬───────────────┘
               │ co_await                  │ resume()
               ▼                           │
┌──────────────────────────┐    ┌──────────▼────────────────┐
│    AsyncRecv (Awaitable) │    │   CoroutineRegistry      │
│                          │    │                           │
│  await_suspend():        │    │  registerAwaitable():     │
│  1. 注册到 Registry       │───►│    fd+type → {await, h}  │
│  2. 获取 SQE             │    │                           │
│  3. 填充 io_uring_prep_recv │  │  takeAndResume():        │
│  4. 设置 user_data       │    │    查找 → set result →   │
│     (协程编码格式)        │    │    resume 协程            │
└──────────────┬───────────┘    └───────────▲───────────────┘
               │ prepare SQE                │ takeAndResume
               ▼                            │
┌──────────────────────────┐    ┌───────────┴───────────────┐
│       IoUring            │    │       EventLoop            │
│                          │    │  (主循环)                  │
│  submitAndWait(1) ───────┼───►│                           │
│                          │    │  processCompletions():    │
│  processCompletions():   │◄───┤    解码 user_data         │
│    遍历 CQE 队列         │    │    isCoroutineUserData?   │
│    回调 (fd, type, res)  │────┤    → takeAndResume()     │
│                          │    │                           │
└──────────────────────────┘    └───────────────────────────┘
```

### 4.2 一次 `co_await AsyncRecv` 的完整流程

```
时间线：
─────────────────────────────────────────────────────────────────►

[1] 协程执行 co_await AsyncRecv(fd, uring)
    │
    ├── await_ready() → false（总是挂起）
    │
    ├── await_suspend(handle):
    │   ├── CoroutineRegistry::registerAwaitable(fd, READ, &result_, handle)
    │   │   → 记录 {awaitable指针, 协程句柄} 到注册表
    │   │
    │   ├── io_uring_get_sqe(ring) → 获取 SQE
    │   │
    │   ├── io_uring_prep_recv(sqe, fd, buf, bufSize, 0)
    │   │
    │   └── sqe->user_data = makeCoroutineUserData(fd, READ)
    │       → bit 63 = 1, bit 48-62 = READ, bit 0-47 = fd
    │
    └── 协程挂起，控制权返回调用者
        （如果是从 Task chain 调用，最终回到 EventLoop 主循环）

[2] EventLoop 主循环
    │
    ├── uring_.submitAndWait(1)
    │   → 提交 SQE 到内核，等待至少 1 个 CQE
    │
    └── uring_.processCompletions(callback):
        ├── io_uring_for_each_cqe(ring, head, cqe):
        │   ├── 检查 cqe->user_data
        │   │   isCoroutineUserData(user_data) → true (bit 63 = 1)
        │   │
        │   ├── 解码：fd = user_data_fd, type = user_data_type
        │   │
        │   └── CoroutineRegistry::takeAndResume(fd, type, cqe->res):
        │       ├── 查找 entries_[key] → {awaitable指针, 协程句柄}
        │       ├── 从注册表移除条目
        │       ├── 设置结果：static_cast<AsyncRecvResult*>(awaitable)->result = cqeResult
        │       │   如果 result > 0: 复制数据到 result.data
        │       └── handle.resume() → 恢复协程
        │
        └── io_uring_cq_advance(ring, count) → 标记 CQE 已消费

[3] 协程恢复
    │
    ├── await_resume():
    │   ├── 从 result_ 中取出数据
    │   └── 返回 RecvResult{data, result}
    │
    └── 协程继续执行后续代码
```

### 4.3 协程嵌套与 Continuation 链

```
EventLoop::acceptLoop()               VlessConnection::clientTask()        Decoder::decode()
    │                                       │                                   │
    │ co_await AsyncAccept                  │ co_await processHandshake()        │
    │   ├── 注册 ACCEPT                      │   ├── co_await Decoder::decode()  │
    │   ├── 挂起                             │   │   ├── co_await stream.read(1) │
    │   └── [CQE 到达后恢复]                  │   │   │   ├── 注册 READ           │
    │                                       │   │   │   ├── 挂起                │
    │                                       │   │   │   └── [CQE 后恢复]         │
    │                                       │   │   └── 返回 version            │
    │                                       │   └── 返回 Request                │
    │                                       │                                   │
    │                                       │ co_await AsyncConnect(...)        │
    │                                       │ co_await AsyncSend(response)      │
    │                                       │ ...                               │
```

**关键**：`Task<T>` 的 `await_suspend` 会设置 `continuation`，子协程完成时自动 resume 父协程。这样协程链最终都能回到 EventLoop 主循环。

### 4.4 多协程并发模型

```
EventLoop 主循环
│
├── acceptTask_ (acceptLoop 协程)
│   └── co_await AsyncAccept → 挂起等待新连接
│
├── Connection A
│   ├── clientTask_ → co_await AsyncRecv(clientFd) → 挂起
│   └── targetTask_ → co_await AsyncRecv(targetFd) → 挂起
│
├── Connection B
│   ├── clientTask_ → co_await AsyncRecv(clientFd) → 挂起
│   └── targetTask_ → co_await AsyncRecv(targetFd) → 挂起
│
└── ... 更多连接

所有协程共享同一个 io_uring 实例。
submitAndWait(1) 会等待任意一个 CQE 完成，
然后 processCompletions 恢复对应的协程。
```

### 4.5 缓冲区安全性

| 操作 | 缓冲区位置 | 安全保证 |
|------|-----------|---------|
| `AsyncRecv` | 协程帧上的 `result_.buf` | recv 完成前不会被覆写 |
| `AsyncSend` | 协程帧上的 `buf_`（数据拷贝） | send 完成前源数据有效 |
| `UringBufferedStream` | 内部 `buffer_` | 每次读取返回独立 vector |

---

## 五、库依赖关系

```
vmess_server (可执行文件)
  └── vmess_server_lib (静态库)
        ├── io_uring (静态库) → -luring
        └── vless_proxy (静态库)
              ├── OpenSSL::SSL + OpenSSL::Crypto
              └── blake3 (源码内联编译，含 SIMD 优化)
```

编译选项：
- C++20, `-fcoroutines`（仅 C++ 文件）
- BLAKE3 SIMD: `-msse2`, `-msse4.1`, `-mavx2 -mbmi2`, `-mavx512f -mavx512vl`

---

## 六、设计特点总结

| 特点 | 说明 |
|------|------|
| **纯协程模型** | 所有 I/O（包括 Accept）都通过 `co_await` 完成，没有回调/状态机 |
| **Go-like 设计** | 对标 Xray 的 `buf.Copy` + `task.Close` + 半关闭传播 |
| **io_uring 原生** | 通过 `CoroutineRegistry` 实现 CQE → 协程恢复的桥接 |
| **双协程连接** | 每个连接 `clientTask` + `targetTask` 独立处理两个方向 |
| **三种转发** | 普通 VLESS / Vision (XTLS) / Encryption，根据请求头自动选择 |
| **半关闭传播** | 一方 EOF → shutdown 另一端，双方完成才关闭连接 |
| **缓冲区安全** | AsyncRecv/AsyncSend 缓冲区在协程帧上，消除竞争 |
| **Encryption 优先** | Encryption 模式优先级高于 Vision，同时指定时走 Encryption |

---

## 七、当前状态与限制

| 项目 | 状态 |
|------|------|
| VLESS 协议 | ✅ 完整实现 |
| Vision (xtls-rprx-vision) | ✅ 完整实现 |
| Encryption (X25519+AEAD) | ✅ 完整实现 |
| UUID 认证 | ✅ 硬编码 `e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b` |
| IPv4 支持 | ✅ |
| IPv6 支持 | ❌ 返回错误 |
| 域名 DNS 解析 | ✅ getaddrinfo (同步) |
| TLS 终止 | ❌ |
| VMess 协议 | ❌ 预留 |
| SOCKS5 协议 | ❌ 预留 |
| 多线程 | ❌ 单 io_uring 实例 |
| 用户管理 | ❌ 硬编码 UUID |
