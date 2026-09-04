# 当前架构速览

> 更新日期：2026-09-04
> 本文以当前代码为准，是项目架构的精确描述，供开发者快速建立整体认知并定位各模块。
> 相关索引见 `doc/README.md`。

## 0. 项目一句话

**C++20 + io_uring + 协程**实现的 **VLESS 代理**，同时包含：
- **服务端**（`vless_server`）：VLESS 入站，TCP + UDP 中继，支持 Vision/Encryption；
- **客户端**（`vless_client`）：本地 SOCKS5 代理，把流量经 VLESS 转发给远端服务端。

核心特色：**单线程事件循环 + 协程**，所有 IO 都走 io_uring，无阻塞调用；用协程把异步状态机写成顺序代码。

## 1. 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│ 协议层   proxy/vless（decoder/encoder/validator/vision/encryption）│
│          proxy/socks5（SOCKS5 解析，客户端用）                 │
├─────────────────────────────────────────────────────────────┤
│ 连接层   server/VlessConnection（服务端状态机）               │
│          client/Socks5Connection + Socks5UdpRelay（客户端）   │
├─────────────────────────────────────────────────────────────┤
│ 事件循环 server/EventLoop（accept + CQE 分发，工厂模式）      │
├─────────────────────────────────────────────────────────────┤
│ 协程层   coro/（Task、Async* 原语、UringOp + PendingUringOps）│
├─────────────────────────────────────────────────────────────┤
│ 系统层   net/（io_uring 封装、socket 工具）                    │
└─────────────────────────────────────────────────────────────┘
```

## 2. 协程与 io_uring 基础

### 2.1 协程类型 `coro::Task<T>`

- `co_await` 一个 `Async*` 原语 → 协程挂起，IO 完成时由事件循环 resume；
- 通过 `Task::h.resume()` 手动启动（`start()` 时调一次），后续全部由 CQE 驱动。

### 2.2 `UringOp` + `PendingUringOps` —— 挂起/恢复与取消的桥梁

- `UringOp`：操作上下文基类，`user_data` 直接存 `&op`（指针直分发，零查表），
  CQE 到达 → `completeFromCqe(userData, res, flags)` → 调用 op 自带完成回调 → 写结果、resume 协程；
- `PendingUringOps`（thread_local 单例）：按 fd 追踪挂起操作；连接关闭时
  `cancelFd(fd)` 置空回调与句柄，迟到 CQE 被忽略，不会 resume 已销毁协程帧；
- **同一 fd 同一类型同时只能有一个挂起操作**（这是单线程事件循环的约束，也是 UDP 多路复用需要 eventfd 队列的原因）。

### 2.3 常用协程原语（`include/coro/uring_awaitable.h`）

| 原语 | 底层操作 | 说明 |
|---|---|---|
| `AsyncRecv(fd, uring)` | `recv` | 读数据，结果含 EOF/错误 |
| `AsyncSend` | `send` | 写数据 |
| `AsyncConnect` | `connect` | 非阻塞 connect |
| `AsyncAccept(fd, uring)` | `accept4` | accept 循环用 |
| `AsyncShutdown` | `shutdown` | 半关闭 |
| `AsyncRecvFrom` / `AsyncSendTo` | `recvmsg`/`sendmsg` | **UDP 专用**（近期新增，注意用 recvmsg 而非 recvfrom，liburing 2.1 兼容） |

### 2.4 两个流抽象

- `coro::UringBufferedStream`（`coro/buffered_stream.h`）：**只读**缓冲流。`read(n)` 返回至少 n 字节或 EOF；内部 `drainRemaining()` 取走缓冲中多余字节。协议握手阶段用它（VLESS 解码需要"读够字节数"）。
- `coro::AsyncStream`（`coro/async_stream.h`）：读写流。`writeFull()` 保证写完；`copyStream(dst, src, closed)` 双向搬运 + **EOF 时 shutdownWrite 传播半关闭**。中继阶段用它。

> 注意：UringBufferedStream 没有 `write` 方法，写入一律走 AsyncStream。

## 3. 启动流程（服务端）

```
src/main.cpp
  └─ 解析参数：端口(默认1080)、日志级别、worker 数、VLESS_USERS(UUID)
  └─ 创建 N 个 EventLoop（N=worker 数），每个传入 ConnectionFactory：
        [&validator](fd, uring) → make_unique<VlessConnection>(fd, uring, validator)
  └─ 每个 worker 线程跑一个 EventLoop::run(socks5Port, reusePort=true)
```

**SO_REUSEPORT**：N 个 EventLoop 监听同一端口，内核按四元组负载均衡分发给不同 worker。

## 4. 事件循环 `server/EventLoop`（工厂模式）

```
run(port):
  ├─ 创建/绑定/监听 socket（非阻塞）
  ├─ acceptTask_ = acceptLoop(); acceptTask_.h.resume()
  └─ 主循环 while(running):
       ├─ uring_.submitAndWait(1)
       ├─ processCompletions: 每个 CQE → UringOp::completeFromCqe(userData, res, flags)
       └─ cleanupClosedConnections(): 回收 isClosed() 的连接
```

`acceptLoop()`（协程）：
```
while(running):
  clientFd = co_await AsyncAccept(listenFd)
  设置非阻塞
  conn = factory_(clientFd, uring)   ← 工厂创建协议连接
  conn->start()                      ← 启动协程状态机
  connections_[clientFd] = move(conn)
```

**工厂模式的意义**：EventLoop 不认识任何协议，只认 `EventLoopConnection` 接口（`start/isClosed/primaryFd/hasFd`）。服务端传 VLESS 工厂，客户端传 SOCKS5 工厂——**同一个 EventLoop 两端共用**（这是近期重构）。

## 5. 连接状态机 `server/VlessConnection`

### 5.1 总览

两个协程协作（成员 `clientTask_` 和 `targetTask_`）：

| 协程 | 方向 | 职责 |
|---|---|---|
| `clientTask_` | 客户端 → 目标 | 握手 → 建连 → 中继 client→target → 清理 |
| `targetTask_` | 目标 → 客户端 | 中继 target→client |

### 5.2 握手（`vless_connection_handshake.cpp`）

```
processHandshake:
  Decoder::decode(stream_, validator_)   ← 从 UringBufferedStream 解析 VLESS 请求头
  setupVision / setupEncryption          ← 可选增强（见 §7）
  sendResponseAndKey                     ← 发响应头（可能含服务端公钥）
```

**响应头格式**：2 字节 `{version, addonsLen}`。明文（无加密）时 `addonsLen=0`，只有 2 字节；加密时响应头后追加 32 字节服务端公钥。

### 5.3 分发（`vless_connection.cpp`）

```
runTcpSession:
  响应先于建连（对齐 Xray）
  connectTarget（getaddrinfo 取第一个 IPv4/IPv6，注意是潜在坑）
  startTargetTask(targetFd)
  forwardHandshakeRemaining（握手缓冲中多读出的数据转发给目标）
  relayClientToTarget：copyStream(targetStream, clientStream, closed_)
```

```
runUdpSession:
  UDP 要求 flow 与 encryption 均为空（当前限制）
  relayUdpClientToTarget / relayUdpTargetToClient（2 字节长度帧，见 §8）
```

### 5.4 半关闭模型

标志位：`clientReadDone_`（client→target 方向 EOF）、`targetReadDone_`（target→client EOF）。

- `copyStream` 在源 EOF 时对目标 `shutdownWrite`（半关闭）；
- `clientTask_` 结束时（`finishClientTask`）：若对端还没结束，`shutdown(targetFd, SHUT_RDWR)` 唤醒它；
- **两者都为 true → closed_ = true → 事件循环回收连接**。

## 6. VLESS 协议（`proxy/vless`）

### 6.1 请求头格式（`protocol.h` + `decoder.cpp`）

```
version(1B=0) | uuid(16B) | addonsLen(1B) | addons(protobuf) | command(1B) | port(2B BE) | addrType(1B) | addr
```

- `command`：TCP=1，UDP=2，Mux=3，Rvs=4（当前服务端只处理 TCP/UDP）；
- `addons`：protobuf，解析出 `flow`（如 "xtls-rprx-vision"）与 `encryption`（如 "x25519-aes128-gcm-sha256"）；
- 地址：IPv4(1)/域名(2)/IPv6(3)，与 SOCKS5 的 ATYP 编号**不同**（SOCKS5 是 1/3/4）。

### 6.2 编码器（`encoder.cpp`，客户端方向，近期新增）

`Encoder::encodeRequest` 是 `Decoder::decode` 的逆操作；`decodeResponse` 读并校验响应头。客户端连远端时用。

### 6.3 UUID 校验（`validator.cpp`）

`parseUuid` 支持带连字符的 UUID 字符串；服务端从 `VLESS_USERS`（逗号分隔）解析用户列表。

## 7. 可选增强：Vision / Encryption（`vision.cpp` / `encryption.cpp`）

| 增强 | 原理 | 密钥派生 |
|---|---|---|
| Vision（xtls-rprx-vision） | 替换真实 TLS 指纹，防主动探测 | BLAKE3（早期）→ **已改为 OpenSSL SHA-256** |
| Encryption（x25519-aes128-gcm-sha256） | 客户端/服务端 ECDH + AEAD 加密封装数据 | X25519 共享密钥派生 |

握手时根据请求头 addons 选择启用。**客户端方向尚未实现这两个增强**（目前客户端纯明文）。

## 8. UDP 中继（长度帧）

VLESS 的 UDP 是"UDP over TCP"：一条 TCP 连接承载，**每个数据报 = 2 字节大端长度 + 载荷**。服务端收到 UDP 命令后把该 TCP 连接与目标 UDP socket 双向转换。SOCKS5 客户端侧也是同样的帧格式。

## 9. SOCKS5 客户端（`src/client` + `src/proxy/socks5`，近期新增）

```
vless_client（client_main.cpp）
  └─ 工厂模式 EventLoop，工厂创建 Socks5Connection
```

`Socks5Connection` 状态机（对应 `server/VlessConnection` 的服务端版镜像）：

```
clientTask_:
  Parser::readGreeting（SOCKS5 握手，仅无认证）
  Parser::readRequest（CONNECT / UDP ASSOCIATE）
  ├─ CONNECT：
  │    vlessConnectAndHandshake（连远端 VLESS + 编码请求头 + 读响应）
  │    发 SOCKS5 成功响应 → 双向中继（半关闭传播，同服务端）
  └─ UDP ASSOCIATE：
       绑定本地 UDP socket → 回 BND 地址
       Socks5UdpRelay：每目标一条 VLESS UDP 会话（TCP 连接 + 长度帧）
       等待控制连接关闭 → 停止 relay
```

`Socks5UdpRelay` 并发模型（单线程约束下的汇聚设计）：
- `recvLoop_`：独占 `(udpFd, READ)`，收应用 UDP 数据报，按目标路由到会话；
- 每个会话 `outTask_`：独占 `(remoteFd, READ)`，读长度帧 → 入队；
- `sendLoop_`：独占 `(udpFd, WRITE)` 与 `(eventFd, READ)`，**eventfd 唤醒队列**批量 sendto；
- 因为同一 fd 同一类型只能一个挂起操作，所以用 **eventfd + 队列** 把多个会话的回程汇聚到单一 sendLoop。

## 10. 关键数据流（TCP 一次完整请求）

```
iOS/SOCKS5 客户端 ──TCP──> VlessConnection ──TCP──> 目标服务器
      │                      │                        │
      │  VLESS 请求头 + 数据   │  解密/解析              │
      │  (明文/加密)           │  转发数据               │
      │                      └─ connectTarget(fd)      │
      │                           ↑ 半关闭传播           │
```

时序（服务端）：
1. accept → factory 建 VlessConnection → start()；
2. 握手：decode 请求头（读够字节）→ 响应头；
3. connectTarget → targetTask 启动；
4. 双向 copyStream（各方向 EOF 半关闭传播）；
5. 两方向都结束 → closed_ → 事件循环回收。

## 11. 构建与运行

```bash
# 依赖：liburing、OpenSSL
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 服务端（明文 VLESS，端口 1080）
./build/src/vless_server 1080 info 2

# 客户端（SOCKS5 本地 1080 → 远端 VLESS）
./build/src/vless_client --socks5-port 1080 --remote <host>:<port> --uuid <uuid>
```

## 12. 已知限制与待办

- `createTargetSocket` / `resolveRemote` 只取 getaddrinfo **第一个** IPv4/IPv6 结果（无多地址回退）→ 已列为待修；
- UDP 会话帧解析简化：半帧数据丢弃尾部（`socks5_udp_relay.cpp` 注释）；
- 明文 VLESS 在墙内易被 GFW 识别 → 参见 `server-tls-support.md`（TLS 改造进行中）；
- 日志走 `std::cerr` + 每次 flush，高并发下性能差 → 异步日志改造进行中。
