# VMess C++ 实现系统设计

> 版本：v0.2 DRAFT
> 整理时间：2026-04-28
> 更新：新增技术选型章节（§0），架构从 epoll 半同步半异步演进为 io_uring + 协程纯异步方案

---

## 0. 技术选型与架构演进

### 0.1 初始方案：epoll + 半同步半异步

项目初期考虑了经典的 **epoll Reactor + 线程池** 方案：

```
【方案 A：epoll + 半同步半异步（已废弃）】

┌─────────────────────────────────────────────┐
│  Main Thread (Reactor)                     │
│  epoll_wait() → 读取事件                  │
│  将请求放入队列                            │
└──────────────┬────────────────────────────┘
               │ 请求队列（需要锁）
               ▼
┌─────────────────────────────────────────────┐
│  Worker Thread Pool (同步处理)             │
│  从队列取请求                              │
│  解析 VMess 协议（同步阻塞）               │
│  连接目标（同步阻塞）                       │
│  转发数据（同步阻塞）                       │
└─────────────────────────────────────────────┘
```

**存在问题**：

1. **IO 复杂但业务逻辑简单**：VMess 中转的场景是「解析一次头部 → 双向加解密转发」，业务逻辑极其简单统一，不需要线程池来处理复杂计算
2. **上下文切换开销**：Reactor 线程与 Worker 线程之间需要队列传递，涉及锁和内存屏障
3. **线程同步复杂**：多线程访问 Connection 状态需要加锁
4. **epoll 本身的系统调用开销**：每次 IO 都需要 `epoll_wait` + `read`/`write` 两次系统调用

---

### 0.2 问题分析：我们的场景到底是什么类型？

```
场景特征分析：
  ✅ IO 密集型（大量网络读写）
  ✅ 业务逻辑简单统一（解析头部 + 双向转发）
  ✅ 连接时间长（VPN 连接可能持续很久）
  ❌ 不需要复杂计算
  ❌ 不需要阻塞操作（所有 IO 都可以异步）

结论：这是纯异步 Proactor 的最佳适用场景！
半同步半异步反而引入了不必要的线程同步开销。
```

---

### 0.3 方案演进：io_uring Proactor

Linux 5.1+ 引入的 **io_uring** 提供了真正的异步 IO：

```
【方案 B：io_uring Proactor（最终方案）】

┌──────────────────────────────────────────────────────────┐
│                    Single Event Loop                     │
│                                                          │
│  io_uring_submit()  ← 一次系统调用提交批量操作          │
│  io_uring_peek_cqe() ← 一次系统调用获取批量完成         │
│                                                          │
│  完成通知 → 直接拿到结果（数据已读到用户态缓冲区）       │
│  （不需要再调用 read/write）                            │
└──────────────────────────────────────────────────────────┘
```

**io_uring vs epoll 对比**：

| 维度            | epoll (Reactor)      | io_uring (Proactor)    |
|-----------------|----------------------|------------------------|
| 系统调用次数     | 每次 IO 两次          | 批量提交，接近 O(1)     |
| 数据拷贝         | 需要（内核→用户态）   | 支持零拷贝（splice）    |
| 编程模型         | 回调/状态机           | 真正的异步              |
| 内核版本要求     | 2.6+                | 5.1+（推荐 5.6+）     |
| 文件 IO 支持     | 需要 aio              | 原生支持                |

---

### 0.4 协程 vs 异步 IO：实现复杂度对比

**关键认知：协程和异步 IO 不是二选一，而是互补的！**

```
┌────────────────┬──────────────────┬────────────────────────────────┐
│     方案       │   代码可读性     │  说明                           │
├────────────────┼──────────────────┼────────────────────────────────┤
│ epoll + 状态机 │  差（回调地狱）  │ 手动管理状态，极易出错         │
│ epoll + 协程   │  好              │ 协程封装回调                   │
│ io_uring 裸用  │  差              │ 手动管理 SQE/CQE               │
│ io_uring + 协程│  最好 ← 推荐     │ 两全其美                       │
│ std::execution │  中              │ 功能强但语法复杂                │
└────────────────┴──────────────────┴────────────────────────────────┘
```

**C++20 协程 + io_uring 配合示例**：

```cpp
// 底层：io_uring 提供异步操作（Sender）
auto read_sender = io_scheduler.async_read(fd, buf, size);

// 用协程写：看起来完全像同步代码！
Task<int> handle_connection(int client_fd) {
    // co_await 一个 Sender，挂起协程，等待 IO 完成
    auto n = co_await io_scheduler.async_read(client_fd, header_buf, HEADER_SIZE);
    
    // 解析头部（同步语义，实际是协程恢复执行）
    auto header = parse_vmess_header(header_buf);
    
    // co_await 异步连接目标
    auto target_fd = co_await io_scheduler.async_connect(
        header.target_addr, header.target_port
    );
    
    // 双向中转（协程版本，非常清晰）
    co_await relay(client_fd, target_fd);
}
```

---

### 0.5 std::execution 的定位

`std::execution`（C++26 P2300）是异步操作的**组合框架**，不是底层 IO 机制：

```
层次结构：
  ┌──────────────────────────────────────┐
  │  应用层：业务逻辑                     │
  ├──────────────────────────────────────┤
  │  协程层：co_await（代码可读性）      │
  ├──────────────────────────────────────┤
  │  组合层：std::execution Sender/Receiver│
  ├──────────────────────────────────────┤
  │  底层：io_uring（真正的异步 IO）     │
  └──────────────────────────────────────┘
```

**决策**：

- **底层 IO**：使用 io_uring（Linux 5.6+）
- **异步组合**：使用 `std::execution`（C++26）或 NVIDIA/stdexec（fallback）
- **代码编写**：使用 C++20 协程 `co_await` 消费 Sender，兼顾性能和可读性

---

### 0.6 最终架构图

```
┌──────────────────────────────────────────────────────────────┐
│                    VMess Server 最终架构                     │
│                                                              │
│  ┌────────────────────────────────────────────────────┐     │
│  │            Main Event Loop (单线程)                 │     │
│  │                                                    │     │
│  │  for (;;) {                                       │     │
│  │    io_uring_submit_and_wait(uring, min_complete=1) │     │
│  │    for each cqe in cq:                             │     │
│  │      // cqe.user_data 指向对应的 Task/Receiver     │     │
│  │      resume_task(cqe.user_data, cqe.res)           │     │
│  │  }                                                │     │
│  │                                                    │     │
│  └────────────────────────────────────────────────────┘     │
│                                                              │
│  每个连接 = 一个协程 Task，在事件循环中协作运行               │
│  （不需要多线程，不需要锁）                                  │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

### 0.7 参考：为什么不用半同步半异步？

半同步半异步适合**业务逻辑复杂、可能有阻塞操作**的场景（如数据库查询、文件压缩等）。

VMess 中转服务不满足这个条件：

```
半同步半异步的成本：
  1. 请求队列的锁开销（每次 IO 都需要入队/出队）
  2. 线程间切换开销
  3. 状态同步的复杂度（需要仔细设计锁粒度）
  
收益：
  0. 没有收益！（因为我们的"同步层"并不需要执行阻塞操作）
  
结论：用纯异步，不要半同步半异步。
```

---

## 1. 项目定位

### 1.1 目标

实现一个高性能、生产可用的 VMess 协议 C++ 服务端，支持中转模式部署。

### 1.2 范围

| 功能                | 支持 | 说明                                    |
|---------------------|------|-----------------------------------------|
| VMess 协议服务端    | ✅   | 完整实现 VMess 协议                     |
| 中转模式            | ✅   | 服务端可作为中转节点                    |
| io_uring 异步 IO    | ✅   | Linux 5.6+，Proactor 模型              |
| C++20 协程          | ✅   | 用 co_await 写异步代码                 |
| std::execution      | ✅   | Sender/Receiver 组合（C++26 或 stdexec）|
| AEAD 认证           | ✅   | 优先支持 AEAD，可选兼容遗留模式        |
| 加密套件            | ✅   | AES-128-GCM、ChaCha20-Poly1305         |
| 多用户管理          | ✅   | 配置文件加载，支持多用户                |
| 防重放              | ✅   | 会话 ID 去重                            |
| 流量混淆            | ✅   | 随机填充、大小混淆                      |
| 零拷贝转发          | ✅   | 使用 io_uring splice                    |
| 客户端              | ❌   | 仅实现服务端                            |
| QUIC/WebSocket      | ❌   | 仅 TCP                                 |
| MUX 多路复用        | ❌   | 初期不支持                              |
| 动态用户管理        | ❌   | 初期不支持，需重启生效                  |

---

## 2. 系统架构

### 2.1 整体架构（io_uring + 协程方案）

```
┌─────────────────────────────────────────────────────────────────────┐
│                        VMess Server 架构                           │
│                                                                     │
│  ┌──────────────┐    ┌──────────────────────────┐                │
│  │  配置加载     │    │  Main Event Loop         │                │
│  │  (YAML)      │───▶│  (单线程，io_uring)     │                │
│  └──────────────┘    │                          │                │
│                       │  io_uring_submit()      │                │
│  ┌──────────────┐    │  io_uring_peek_cqe()    │                │
│  │  用户管理     │───▶│                          │                │
│  │  (UUID校验)   │    │  每个连接 = 一个协程     │                │
│  └──────────────┘    │  (co_await 驱动)        │                │
│                       └──────────┬───────────────┘                │
│                                  │                                │
│                       ┌──────────▼───────────────┐                │
│                       │   协程层（业务逻辑）      │                │
│                       │                          │                │
│                       │  handle_connection() {   │                │
│                       │    co_await async_read   │                │
│                       │    parse_vmess_header()  │                │
│                       │    co_await async_connect │                │
│                       │    co_await relay_loop   │                │
│                       │  }                       │                │
│                       └──────────┬───────────────┘                │
│                                  │                                │
│                       ┌──────────▼───────────────┐                │
│                       │   std::execution 层      │                │
│                       │   (Sender/Receiver 组合) │                │
│                       └──────────┬───────────────┘                │
│                                  │                                │
│                       ┌──────────▼───────────────┐                │
│                       │   io_uring 底层          │                │
│                       │   (真正异步 IO)           │                │
│                       └──────────────────────────┘                │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块划分

```
vmess/
├── include/
│   └── vmess/
│       ├── config.h           # 配置管理
│       ├── server.h           # 服务端主入口
│       ├── session.h          # 会话管理
│       ├── crypto.h           # 加密/解密
│       ├── protocol.h         # 协议解析
│       ├── user_manager.h     # 用户管理
│       ├── relay.h            # 中转模块
│       ├── io_uring_loop.h    # io_uring 事件循环
│       ├── scheduler.h        # std::execution 调度器
│       ├── task.h             # C++20 协程 Task 定义
│       ├── buffer.h           # 缓冲区管理
│       └── error.h            # 错误处理
├── src/
│   ├── config.cpp
│   ├── server.cpp
│   ├── session.cpp
│   ├── crypto.cpp
│   ├── protocol.cpp
│   ├── user_manager.cpp
│   ├── relay.cpp
│   ├── io_uring_loop.cpp
│   ├── scheduler.cpp
│   ├── buffer.cpp
│   └── main.cpp
├── test/
│   └── ...
├── doc/
│   ├── 01-vmess-protocol.md
│   ├── 02-go-implementation-reference.md
│   ├── 03-cpp26-features.md
│   └── 04-system-design.md
├── CMakeLists.txt
└── README.md
```

---

## 3. 核心模块设计

### 3.1 io_uring 事件循环 + C++20 协程

#### 设计要点

- 使用 Linux `io_uring`（内核 5.1+，推荐 5.6+）
- 单线程事件循环，每个连接一个协程
- 使用 `liburing` 库封装底层操作
- 协程通过 `co_await` 消费 Sender，代码像同步一样清晰

#### Scheduler 设计（满足 std::execution::scheduler 概念）

```cpp
// io_uring 调度器，满足 std::execution::scheduler 概念
class IoUringScheduler {
public:
    // 构造：初始化 io_uring
    explicit IoUringScheduler(unsigned queue_depth = 256);
    ~IoUringScheduler();

    // ===== std::execution Sender 工厂 =====

    // 异步读取：返回 Sender<size_t>
    auto async_read(int fd, std::span<uint8_t> buf);

    // 异步写入：返回 Sender<size_t>
    auto async_write(int fd, std::span<const uint8_t> buf);

    // 异步 accept：返回 Sender<int> (新 fd)
    auto async_accept(int listen_fd);

    // 异步 connect：返回 Sender<int>
    auto async_connect(const std::string& addr, uint16_t port);

    // 异步关闭
    auto async_close(int fd);

    // ===== 事件循环 =====

    // 运行主事件循环
    void run();

    // 提交并等待完成
    void submit_and_wait(unsigned min_complete = 1);

    // 获取调度器（用于 std::execution::schedule()）
    IoUringScheduler& scheduler() { return *this; }
};
```

#### Task<T> 协程返回类型

```cpp
// C++20 协程 Task 封装（无返回值版本）
template<typename T = void>
class Task {
public:
    struct promise_type {
        Task get_return_object() {
            return Task{Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    // 关键：支持 co_await 一个 Sender
    auto operator co_await() {
        // 返回等待器，挂起当前协程
        // 当 io_uring CQE 到达时恢复
        return Awaitable{handle_};
    }

private:
    Handle handle_;
};
```

#### 主事件循环

```cpp
void IoUringLoop::run() {
    for (;;) {
        // 提交所有 pending 的 SQE，等待至少 1 个完成
        io_uring_submit_and_wait(&ring_, 1);

        // 处理所有完成的 CQE
        io_uring_cqe* cqe;
        unsigned head;
        io_uring_for_each_cqe(&ring_, &head) {
            // user_data 指向对应的 OperationState 或协程句柄
            auto* state = reinterpret_cast<OperationState*>(cqe->user_data);

            if (cqe->res < 0) {
                // 错误：调用 set_error
                std::execution::set_error(
                    state->receiver_,
                    std::error_code(-cqe->res, std::system_category())
                );
            } else {
                // 成功：调用 set_value
                std::execution::set_value(
                    state->receiver_,
                    static_cast<size_t>(cqe->res)
                );
            }

            io_uring_cqe_seen(&ring_, cqe);
        }
    }
}
```

#### 连接处理协程（核心业务逻辑）

```cpp
// 每个连接一个协程，用 co_await 驱动异步 IO
Task<void> handle_connection(int client_fd,
                            IoUringScheduler& sched,
                            UserManager& user_mgr,
                            SessionManager& session_mgr) {
    try {
        // 1. 读取 VMess 请求头（co_await 挂起协程，不阻塞线程）
        std::array<uint8_t, HEADER_SIZE> header_buf;
        auto n = co_await sched.async_read(
            client_fd, std::span(header_buf)
        );

        // 2. 解析 VMess 头部（同步计算，很快）
        auto header = parse_vmess_header(header_buf);

        // 3. 校验用户
        auto user = user_mgr.find_user(header.uuid);
        if (!user) {
            behavior_drain(client_fd, sched);  // 行为排空
            co_return;
        }

        // 4. 防重放检查
        if (!session_mgr.add_if_not_exists(header.session_id)) {
            co_return;  // 重放攻击，拒绝
        }

        // 5. 异步连接到目标（co_await 挂起协程）
        auto target_fd = co_await sched.async_connect(
            header.target_addr, header.target_port
        );

        // 6. 双向中转（协程内循环，直到连接关闭）
        co_await relay(client_fd, target_fd, sched, header.security);

    } catch (const std::exception& e) {
        log_error("connection error: {}", e.what());
    }

    // 清理
    co_await sched.async_close(client_fd);
}

// 双向中转协程
Task<void> relay(int client_fd, int target_fd,
                 IoUringScheduler& sched, SecurityType sec) {
    std::vector<uint8_t> client_buf(BUF_SIZE);
    std::vector<uint8_t> target_buf(BUF_SIZE);

    for (;;) {
        // 同时提交两个方向的异步读操作
        auto client_read = sched.async_read(client_fd, std::span(client_buf));
        auto target_read = sched.async_read(target_fd, std::span(target_buf));

        // 使用 std::execution：等待任意一个完成
        auto first = std::execution::when_any(
            std::move(client_read),
            std::move(target_read)
        );

        auto [idx, bytes_read] = co_await first;

        if (idx == 0) {
            // 客户端数据到达 → 加密后转发到目标
            auto encrypted = encrypt(
                std::span(client_buf.data(), bytes_read), sec
            );
            co_await sched.async_write(target_fd, std::span(encrypted));
        } else {
            // 目标数据到达 → 解密后转发到客户端
            auto decrypted = decrypt(
                std::span(target_buf.data(), bytes_read), sec
            );
            co_await sched.async_write(client_fd, std::span(decrypted));
        }
    }
}
```

---

### 3.2 协议解析模块

#### 请求头解析

```cpp
class VmessParser {
public:
    // 解析请求头（AEAD 模式）
    static std::expected<VmessRequest, VmessError>
    parse_request_aead(std::span<const uint8_t> buf,
                      const UserManager& users);

    // 解析请求头（遗留模式）
    static std::expected<VmessRequest, VmessError>
    parse_request_legacy(std::span<const uint8_t> buf,
                         const UserManager& users);

    // 编码响应头
    static std::vector<uint8_t> encode_response(
        const VmessResponse& resp,
        const CryptoContext& ctx);
};
```

#### 请求头结构

```cpp
struct VmessRequest {
    uint8_t version;
    std::array<uint8_t, 16> body_iv;
    std::array<uint8_t, 16> body_key;
    uint8_t response_cmd;
    SecurityType security;
    uint8_t option;
    CommandType command;
    Address target_addr;
    uint16_t target_port;
    std::vector<uint8_t> padding;
    uint32_t fnv_hash;
};
```

---

### 3.3 加密模块

#### 加密上下文

```cpp
struct CryptoContext {
    SecurityType security;
    std::array<uint8_t, 16> iv;
    std::array<uint8_t, 16> key;

    // AEAD 相关
    std::array<uint8_t, 12> nonce;  // AES-GCM/ChaCha20 nonce
};
```

#### 加密接口

```cpp
class Crypto {
public:
    // AEAD 加密
    static std::vector<uint8_t> seal_aead(
        std::span<const uint8_t> plaintext,
        const CryptoContext& ctx);

    // AEAD 解密
    static std::expected<std::vector<uint8_t>, CryptoError>
    open_aead(std::span<const uint8_t> ciphertext,
              const CryptoContext& ctx);

    // 根据类型选择加密方法
    static SecurityType select_security();
};
```

---

### 3.4 用户管理模块

```cpp
struct User {
    uuid_t uuid;
    std::string email;
    SecurityType security;
    std::array<uint8_t, 16> cmd_key;  // AEAD CmdKey
};

class UserManager {
public:
    // 从配置加载用户
    bool load_from_config(const Config& config);

    // 根据 UUID 查找用户（AEAD 模式）
    std::optional<User> find_user(const uuid_t& uuid) const;

    // 根据认证 ID 查找用户（AEAD 模式，从 auth_id 反查）
    std::optional<User> find_user_by_auth_id(
        const std::array<uint8_t, 16>& auth_id) const;

private:
    std::unordered_map<std::string, User> users_;
    // AEAD 模式下：auth_id → user 索引（加速查找）
    std::unordered_map<std::string, std::string> auth_id_index_;
};
```

---

### 3.5 会话管理模块（防重放）

```cpp
struct SessionId {
    uuid_t user_uuid;
    int64_t timestamp;
    std::array<uint8_t, 16> random_iv;

    bool operator==(const SessionId&) const = default;
};

class SessionManager {
public:
    // 添加会话（如果不存在则返回 true）
    bool add_if_not_exists(const SessionId& id);

    // 清理过期会话（定时调用）
    void cleanup_expired();

    // 启动定期清理任务（协程版本）
    Task<void> cleanup_loop();

private:
    // 使用 hash set 存储会话 ID
    std::unordered_set<std::string> sessions_;

    // 时间戳 → 会话 ID 列表（用于高效清理）
    std::multimap<int64_t, std::string> expiry_index_;

    // 清理间隔：30 秒
    // 有效期：3 分钟
    static constexpr auto EXPIRY_DURATION = std::chrono::minutes(3);
    static constexpr auto CLEANUP_INTERVAL = std::chrono::seconds(30);
};
```

---

### 3.6 中转模块

```cpp
class RelayManager {
public:
    // 异步连接到目标地址（返回 Sender）
    auto async_connect(const Address& addr, uint16_t port,
                      IoUringScheduler& sched);

    // 启动双向中转协程
    Task<void> start_relay(int client_fd, int target_fd,
                           SecurityType sec,
                           IoUringScheduler& sched);

private:
    // 连接超时设置
    std::chrono::seconds connect_timeout_{10};
    std::chrono::seconds read_timeout_{60};
};
```

---

## 4. 缓冲区设计

### 4.1 设计目标

- 零拷贝（尽可能）
- 支持 `readv`/`writev`（io_uring 支持）
- 支持 `splice`（io_uring 原生支持，实现零拷贝转发）

### 4.2 Buffer 接口

```cpp
class Buffer {
public:
    // 追加数据（从 io_uring 读取结果）
    void append(std::span<const uint8_t> data);

    // 随机访问（用于协议解析）
    std::span<uint8_t> data() { return {buf_.data() + read_pos_, size()}; }
    std::span<const uint8_t> data() const { ... }

    // 标记已消费
    void consume(size_t n);

    // 预留空间（避免频繁重新分配）
    void reserve(size_t n);

    // 当前有效数据大小
    size_t size() const { return write_pos_ - read_pos_; }

    // 清空（回收空间）
    void clear();

private:
    // 使用连续缓冲区（初期实现）
    // 后期可优化为环形缓冲区
    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
};
```

---

## 5. 配置设计

### 5.1 配置文件格式（YAML）

```yaml
# vmess-server.yaml
server:
  listen: 0.0.0.0:443
  max_connections: 1024
  timeout: 300  # 秒

  # io_uring 设置
  io_uring:
    queue_depth: 256
    sq_thread_idle: 0  # 0 = 不启用 SQ 线程

  # 中转模式设置
  relay:
    enabled: true
    connect_timeout: 10
    read_timeout: 60

  # 安全设置
  security:
    enforce_aead: true   # 强制 AEAD 模式
    allow_legacy: false  # 不允许遗留模式

users:
  - uuid: "b831381d-6324-4d53-ad4f-8cda48b30811"
    email: "user1@example.com"
    security: "auto"  # auto, aes-128-gcm, chacha20-poly1305

  - uuid: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    email: "user2@example.com"
    security: "aes-128-gcm"

logging:
  level: info  # debug, info, warn, error
  file: "/var/log/vmess/server.log"
```

### 5.2 配置加载

```cpp
class Config {
public:
    struct ServerConfig {
        std::string listen_addr;
        uint16_t listen_port;
        size_t max_connections;
        size_t io_uring_queue_depth;
        bool enforce_aead;
    };

    struct UserConfig {
        std::string uuid;
        std::string email;
        std::string security;  // "auto", "aes-128-gcm", "chacha20-poly1305"
    };

    struct LogConfig {
        std::string level;
        std::string file;
    };

    // 从文件加载配置
    static std::expected<Config, ConfigError>
    load_from_file(const std::string& path);

private:
    ServerConfig server_;
    std::vector<UserConfig> users_;
    LogConfig logging_;
};
```

---

## 6. 错误处理

### 6.1 错误码设计

```cpp
enum class VmessErrorCode {
    // 协议错误
    invalid_header,
    invalid_auth,
    replay_detected,
    unsupported_version,

    // 加密错误
    decrypt_failed,
    encrypt_failed,
    invalid_security_type,

    // 网络错误
    connection_failed,
    timeout,
    connection_reset,

    // 配置错误
    invalid_config,
    user_not_found,
};

struct VmessError {
    VmessErrorCode code;
    std::string message;
};
```

### 6.2 使用 std::expected

```cpp
// 函数返回类型
std::expected<VmessRequest, VmessError> parse_request(...);
std::expected<std::vector<uint8_t>, CryptoError> decrypt(...);

// 使用方式
auto result = parse_request(buf, users);
if (!result) {
    log_error("Parse failed: {}", result.error().message);
    co_return;  // 协程中直接返回
}
auto request = std::move(result.value());
```

---

## 7. 性能优化

### 7.1 io_uring 批量提交

```cpp
// 批量提交多个 IO 操作，减少系统调用
void submit_batch(std::vector<Operation>& ops) {
    for (auto& op : ops) {
        io_uring_prep_read(sqe, op.fd, op.buf, op.len, 0);
        io_uring_sqe_set_data(sqe, &op);
    }
    io_uring_submit(&ring_);  // 一次系统调用
}
```

### 7.2 零拷贝优化

- 使用 `io_uring` 的 `splice` 操作在两个 socket 之间直接传输
- 注意：VMess 需要加解密，所以无法完全零拷贝，但 splice 可用于未加密的元数据传输

### 7.3 内存池

```cpp
// Buffer 对象池（减少 malloc/free）
class BufferPool {
public:
    std::unique_ptr<Buffer> allocate(size_t size);
    void deallocate(std::unique_ptr<Buffer> buf);
};
```

---

## 8. 安全性设计

### 8.1 行为排空

即使拒绝非法连接，也模拟正常连接的流量特征：

```cpp
// 行为排空：模拟正常连接的行为
Task<void> behavior_drain(int fd, IoUringScheduler& sched) {
    // 1. 读取并丢弃数据（模拟正常读取）
    std::array<uint8_t, 1024> buf;
    auto n = co_await sched.async_read(fd, std::span(buf));

    // 2. 发送随机数据（模拟正常响应）
    std::vector<uint8_t> random_data(512);
    std::generate(random_data.begin(), random_data.end(),
                  [] { return std::rand() % 256; });
    co_await sched.async_write(fd, std::span(random_data));

    // 3. 延迟关闭连接（模拟正常连接持续时间）
    co_await sleep_for(std::chrono::seconds(5));

    co_await sched.async_close(fd);
}
```

### 8.2 时间攻击防护

- 使用常量时间比较 UUID（`CRYPTO_memcmp`）
- 使用常量时间比较认证标签（AEAD tag 验证）

### 8.3 资源限制

- 最大连接数限制（`max_connections`）
- 每个连接的缓冲区大小限制
- 连接频率限制（防止 DoS）

---

## 9. 开发路线图

### Phase 1：基础框架

- [ ] 项目结构和 CMake 构建系统
- [ ] 配置加载模块（YAML）
- [ ] `Task<T>` 协程基础类型
- [ ] `IoUringScheduler` 基础框架（封装 liburing）
- [ ] Buffer 管理

### Phase 2：协议实现

- [ ] VMess 请求头解析（AEAD）
- [ ] VMess 请求头解析（遗留模式，可选）
- [ ] 加密/解密模块（AES-128-GCM、ChaCha20-Poly1305）
- [ ] 响应头编码

### Phase 3：中转功能

- [ ] 用户管理模块（UUID 校验）
- [ ] 会话管理（防重放）
- [ ] `async_connect` 实现
- [ ] 双向数据转发协程

### Phase 4：优化和测试

- [ ] 行为排空实现
- [ ] 内存池
- [ ] 单元测试
- [ ] 集成测试
- [ ] 性能基准测试

### Phase 5：高级特性（可选）

- [ ] `std::execution` 完整集成（Sender/Receiver）
- [ ] MUX 多路复用支持
- [ ] 动态用户管理
- [ ] 统计和监控接口

---

## 10. 参考资料

- VMess 协议文档：`doc/01-vmess-protocol.md`
- Go 实现参考：`doc/02-go-implementation-reference.md`
- C++26 特性：`doc/03-cpp26-features.md`
- V2Ray Go 实现：`reference/go-v2ray-core/`
- io_uring 参考：https://kernel.dk/io_uring.pdf
- stdexec（std::execution 参考实现）：https://github.com/NVIDIA/stdexec
- C++20 协程教程：https://lewissbaker.github.io/
