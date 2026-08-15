> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：C++ 协程通用理论笔记，与项目架构无直接对应
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# VMess C++ 高级设计：协程调度、C++ 协程体系与现代 C++ 特性

> 版本：v1.0 DRAFT
> 整理时间：2026-04-28
> 内容：协程调度机制、C++ 协程体系架构、现代 C++ 特性应用、Go Channel 的 C++ 实现

---

## 1. co_await 的协程调度机制

### 1.1 核心概念

`co_await` 并不会自动调度到其他协程，而是将当前协程**挂起（suspend）**，控制权返回给**协程的调用者或调度器**。

### 1.2 执行流程

```
协程 A 执行到 co_await async_read(...)
          ↓
    协程 A 挂起（suspend），保存当前状态
          ↓
    控制权返回给调度器（Scheduler）
          ↓
    调度器从就绪队列选择下一个协程 B
          ↓
    恢复（resume）协程 B 的执行
```

### 1.3 关键点

1. **谁负责恢复协程？** → **Scheduler（调度器）**
2. **调度器如何选择下一个协程？** → 取决于调度策略（轮询、优先级、IO 完成事件等）
3. **io_uring 场景**：IO 完成后，io_uring 的 completion queue 会通知，调度器将对应的协程加入就绪队列

### 1.4 代码示例

```cpp
// 调度器伪代码
class Scheduler {
    std::queue<CoroutineHandle> ready_queue_;
    
    void run() {
        while (!ready_queue_.empty()) {
            auto coro = ready_queue_.front();
            ready_queue_.pop();
            
            // 恢复协程执行
            coro.resume();
            // 协程可能在 co_await 处再次挂起
            // 挂起时会将自己重新加入 ready_queue_
        }
    }
};

// 协程代码
Task<void> handle_connection() {
    // ... 一些代码 ...
    
    // co_await 挂起当前协程
    // 控制权返回给 Scheduler::run()
    // Scheduler 会选择其他就绪协程执行
    auto n = co_await async_read(fd, buf);
    
    // 当 IO 完成后，Scheduler 会恢复这个协程
    // 从这里继续执行
}
```

### 1.5 回答常见问题

**Q: co_await 之后会出让 CPU 给哪个协程？**
A: `co_await` 之后，CPU 会回到调度器，调度器会选择**任意一个就绪的协程**继续执行，不一定是"其他在 co_await 的协程"。

**Q: 协程切换的成本是什么？**
A: 协程切换比线程切换轻量得多：
- 不需要系统调用
- 不需要切换页表
- 只需要保存/恢复协程的局部状态（栈、寄存器等）

---

## 2. C++ 协程体系整体架构

### 2.1 协程三部曲

C++ 的协程支持是**分多个标准版本逐步完善**的，被称为"协程三部曲"：

#### C++20：协程基础（底层机制）

提供了**协程语法**，但需要自己实现很多东西：

- `co_await`：挂起和恢复协程
- `co_yield`：产出值并挂起
- `co_return`：协程返回
- 三个关键定制点：
  - `promise_type`：控制协程行为
  - `awaitable`：定义 `co_await` 行为
  - `coroutine_handle`：手动控制协程恢复

**问题**：C++20 只提供了"协程语法"，没有"协程库"。你需要自己实现 `Task<T>`、`AsyncRead` 等包装类。

#### C++23：无栈协程库（部分实现）

原计划提供 `<coroutine>` 的标准库设施，但实际上：
- **C++23 没有加入完整的协程库**
- 只有少量补充（如 `std::generator`）

#### C++26：std::execution（P2300）—— "最后一块拼图"

提供了**高级的异步编程框架**，将协程、异步 IO、调度器统一起来：

- **Sender/Receiver 模型**：类型安全的异步操作组合
- **Scheduler**：控制协程在哪里执行（线程池、IO 线程等）
- **与协程集成**：`co_await sender` 可以直接等待异步操作

### 2.2 整体协作关系

```
┌─────────────────────────────────────────────┐
│          应用代码（业务逻辑）                  │
│    task() { co_await async_read(...); }     │
└────────────────┬────────────────────────────┘
                 │ co_await
                 ↓
┌─────────────────────────────────────────────┐
│        C++26 std::execution (P2300)         │
│   - Sender/Receiver 异步组合                 │
│   - Scheduler 调度器                         │
│   - 与协程无缝集成                           │
└────────────────┬────────────────────────────┘
                 │ 提交 IO 请求
                 ↓
┌─────────────────────────────────────────────┐
│         C++20 协程基础                        │
│   - co_await / co_yield / co_return          │
│   - coroutine_handle 手动控制                 │
│   - promise_type 定制行为                     │
└────────────────┬────────────────────────────┘
                 │ 底层 IO
                 ↓
┌─────────────────────────────────────────────┐
│          io_uring / epoll (OS 层)            │
│   - 异步 IO 系统调用                          │
│   - 事件通知                                  │
└─────────────────────────────────────────────┘
```

### 2.3 为什么称 C++26 是"最后一块拼图"？

因为 C++20 提供了协程的**底层机制**，C++26 提供了协程的**高层组合框架**，两者结合才构成了完整的异步编程体系：

- **C++20**：让你能写协程（语法层面）
- **C++26**：让你能组合异步操作（库层面）
- **两者结合**：才能写出像 Go 一样简洁的异步代码

---

## 3. 现代 C++ 特性应用

### 3.1 Lambda 表达式

```cpp
// 用于定义内联回调函数
auto handle_request = [](Request req) -> Task<Response> {
    auto data = co_await read_body(req);
    co_return process(data);
};

// 用于算法中的自定义操作
std::sort(connections.begin(), connections.end(),
          [](auto& a, auto& b) { return a.last_active < b.last_active; });

// 捕获列表的高级用法
auto create_handler = [this](int fd) -> Task<void> {
    auto buf = std::make_shared<Buffer>();
    co_await read_request(fd, *buf);
    co_return;
};
```

### 3.2 std::function 和 std::move_only_function

```cpp
// C++23 引入 std::move_only_function，适合存储协程回调
std::move_only_function<Task<>(Socket)> handler;

// C++20 及之前使用 std::function
std::function<Task<>(Socket)> handler;

// 使用示例
class ConnectionHandler {
    std::move_only_function<Task<>(Request)> request_handler_;
    
public:
    void set_handler(std::move_only_function<Task<>(Request)> handler) {
        request_handler_ = std::move(handler);
    }
    
    Task<void> handle(Request req) {
        co_await request_handler_(std::move(req));
    }
};
```

### 3.3 智能指针

```cpp
// 共享连接状态
std::shared_ptr<ConnectionState> state = 
    std::make_shared<ConnectionState>();

// 唯一所有权
std::unique_ptr<Socket> sock = 
    std::make_unique<Socket>(fd);

// 自定义删除器
auto sock = std::unique_ptr<Socket, decltype(&close_socket)>(
    new Socket(fd), &close_socket
);
```

### 3.4 原子操作和内存模型

```cpp
// 无锁统计计数
std::atomic<int64_t> total_bytes_sent{0};
std::atomic<int64_t> total_connections{0};

// 内存序控制
total_bytes_sent.fetch_add(n, std::memory_order_relaxed);

// 自旋锁（轻量级锁）
std::atomic_flag lock_ = ATOMIC_FLAG_INIT;

void critical_section() {
    while (lock_.test_and_set(std::memory_order_acquire)) {
        // 自旋等待
    }
    
    // 临界区代码
    
    lock_.clear(std::memory_order_release);
}
```

### 3.5 std::expected（C++23）

```cpp
// 错误处理，替代异常
std::expected<DecryptedData, VMessError> 
decrypt(const EncryptedData& data) {
    if (/* 错误 */) {
        return std::unexpected(VMessError::InvalidHash);
    }
    return decrypted;
}

// 使用方式
auto result = decrypt(encrypted_data);
if (!result) {
    log_error("Decrypt failed: {}", result.error().message);
    co_return;
}
auto data = std::move(result.value());
```

### 3.6 Concepts（C++20）

```cpp
// 定义概念，约束模板参数
template<typename T>
concept AsyncReadable = requires(T t, std::span<uint8_t> buf) {
    { t.async_read(buf) } -> std::same_as<Task<size_t>>;
};

template<typename T>
concept AsyncWritable = requires(T t, std::span<const uint8_t> buf) {
    { t.async_write(buf) } -> std::same_as<Task<size_t>>;
};

// 使用概念约束模板
template<AsyncReadable T>
Task<void> read_request(const T& readable) {
    // ...
}
```

### 3.7 Modules（C++20，如果编译器支持）

```cpp
// vmess.ixx (模块接口文件)
export module vmess;

export namespace vmess {
    class Server {
    public:
        void run();
    };
}

// main.cpp
import vmess;
import std.core;

int main() {
    vmess::Server server;
    server.run();
}
```

---

## 4. Go Channel 的 C++ 实现

### 4.1 Go Channel 的核心特性

Go 的 channel 是非常优雅的通信机制，核心特性包括：

1. **线程安全**：多个 goroutine 可以安全地发送和接收
2. **阻塞语义**：发送和接收可以阻塞，直到对方准备好
3. **缓冲能力**：可以创建带缓冲的 channel
4. **关闭通知**：可以关闭 channel，通知所有接收者

### 4.2 重要更正：C++ 标准中没有 std::channel

**重要提示**：经过核实，C++26 标准中**确实没有 `std::channel`**。我之前的信息有误，特此更正。

#### 当前 C++ 标准中的协程设施

- **C++20**：只有底层协程机制（`co_await`、`co_yield`、`co_return`、`coroutine_handle` 等）
- **C++23**：增加了 `std::generator`，但仍然没有 `std::channel`
- **C++26**：增加了 `std::execution`（P2300），但仍然没有 `std::channel`

#### 如果需要使用 channel-like 设施，有什么选择？

1. **自己实现**：使用 `std::queue`、锁、`coroutine_handle` 等手动实现
2. **使用第三方库**：
   - **cppcoro**：提供了 `channel` 实现
   - **libunifex**：提供了类似设施
   - **stdexec**：C++26 std::execution 的参考实现

#### 在本项目中是否需要 channel？

对于 VMess 中转服务，可能**不需要**复杂的 channel 机制，因为：

1. **每个连接是独立的协程**：不需要在协程之间传递消息
2. **IO 驱动**：协程的生命周期由 IO 事件驱动，不需要额外的通信机制
3. **如果需要**：可以自己实现一个简单的 channel，或者使用 cppcoro 的 channel

### 4.3 方案 1：基于协程的 Channel 实现（自己实现）

```cpp
template<typename T>
class Channel {
    std::queue<T> buffer;
    std::mutex mtx;
    std::coroutine_handle<> sender;  // 等待发送的协程
    std::coroutine_handle<> receiver; // 等待接收的协程
    size_t capacity;
    bool closed_ = false;
    
public:
    explicit Channel(size_t cap) : capacity(cap) {}
    
    // 协程安全的发送
    Task<bool> send(T value) {
        std::unique_lock lock(mtx);
        
        // 如果 channel 已关闭，返回 false
        if (closed_) {
            co_return false;
        }
        
        // 如果缓冲区满，挂起当前协程
        while (buffer.size() >= capacity && !closed_) {
            sender = co_await std::suspend_always{};
            lock.lock();  // 重新获取锁
        }
        
        // 再次检查是否关闭
        if (closed_) {
            co_return false;
        }
        
        buffer.push(std::move(value));
        
        // 唤醒等待接收的协程
        if (receiver) {
            receiver.resume();
            receiver = nullptr;
        }
        
        co_return true;
    }
    
    // 协程安全的接收
    Task<std::optional<T>> recv() {
        std::unique_lock lock(mtx);
        
        // 如果缓冲区空且未关闭，挂起当前协程
        while (buffer.empty() && !closed_) {
            receiver = co_await std::suspend_always{};
            lock.lock();
        }
        
        // 如果缓冲区空且已关闭，返回 std::nullopt
        if (buffer.empty() && closed_) {
            co_return std::nullopt;
        }
        
        T value = std::move(buffer.front());
        buffer.pop();
        
        // 唤醒等待发送的协程
        if (sender) {
            sender.resume();
            sender = nullptr;
        }
        
        co_return std::move(value);
    }
    
    // 关闭 channel
    void close() {
        std::unique_lock lock(mtx);
        closed_ = true;
        
        // 唤醒所有等待的协程
        if (sender) {
            sender.resume();
            sender = nullptr;
        }
        if (receiver) {
            receiver.resume();
            receiver = nullptr;
        }
    }
};
```

### 4.4 方案 2：使用现有的 C++ 异步库

#### cppcoro

```cpp
#include <cppcoro/async_manual_reset_event.hpp>
#include <cppcoro/async_auto_reset_event.hpp>
#include <cppcoro/channel.hpp>

// cppcoro 提供了类似 Go channel 的实现
cppcoro::channel<int> ch;

// 生产者协程
cppcoro::task<> producer(cppcoro::channel_writer<int> writer) {
    for (int i = 0; i < 10; ++i) {
        co_await writer.write(i);
    }
    writer.close();
}

// 消费者协程
cppcoro::task<> consumer(cppcoro::channel_reader<int> reader) {
    while (auto value = co_await reader.read()) {
        std::cout << *value << std::endl;
    }
}
```

#### libunifex

```cpp
#include <unifex/sender_concepts.hpp>
#include <unifex/then.hpp>
#include <unifex/sync_wait.hpp>

// libunifex 提供了类似 Go channel 的设施
```

#### stdexec

```cpp
#include <execution>
#include <stdexec/stdexec.hpp>

// stdexec 是 C++26 std::execution 的参考实现
// 提供了完整的 Sender/Receiver 模型
```

### 4.5 在本项目中的应用场景（如果需要）

```cpp
// Go 中的用法
// ch := make(chan Request, 100)
// go handleRequest(<-ch)

// C++ 中的对应实现（如果需要）
Channel<Request> request_ch{100};

// 生产者协程（IO 线程）
Task<> read_requests(Socket sock, Channel<Request>& ch) {
    while (true) {
        auto req = co_await read_request(sock);
        if (!co_await ch.send(std::move(req))) {
            // Channel 已关闭
            break;
        }
    }
}

// 消费者协程（工作线程）
Task<> handle_requests(Channel<Request>& ch) {
    while (true) {
        auto result = co_await ch.recv();
        if (!result.has_value()) {
            // Channel 已关闭
            break;
        }
        auto req = std::move(result.value());
        co_await process_request(req);
    }
}

// 主函数
int main() {
    Channel<Request> ch{100};
    
    // 启动生产者和消费者协程
    auto producer_task = read_requests(sock, ch);
    auto consumer_task = handle_requests(ch);
    
    // 等待完成
    // ...
    
    // 关闭 channel
    ch.close();
}
```

### 4.6 性能优化建议

1. **使用无锁队列**：对于高并发场景，可以使用无锁队列实现 Channel
2. **批量操作**：支持批量发送和接收，减少协程切换次数
3. **内存池**：为 Channel 中的消息使用内存池，减少内存分配开销

---

## 5. 多线程 vs 单线程：是否需要多线程？

### 5.1 单线程 + 协程方案

**优点**：
- 无锁编程，避免线程同步开销
- 代码简单，不需要考虑线程安全
- 适合连接数不是特别多（< 10k）的场景

**缺点**：
- 无法充分利用多核 CPU
- 如果有一个协程计算密集型操作，会阻塞所有其他协程

### 5.2 多线程 + 协程方案

**优点**：
- 充分利用多核 CPU
- 可以提高吞吐量
- 容错性：一个工作线程崩溃不会影响其他线程

**缺点**：
- 需要线程同步，增加了复杂性
- 可能需要考虑负载均衡

### 5.3 推荐方案：多 Proactor + 协程

⚠️ **重要更正**：我们项目使用的是 **Proactor 模型**（基于 io_uring），而不是 Reactor 模型。因此多线程方案应该是"多 Proactor"，而不是"多 Reactor"。

#### Proactor vs Reactor 的关键区别

**Reactor 模型（epoll 等传统 IO 多路复用）**：
```
1. 事件循环等待事件（epoll_wait）
2. 事件到达（socket 可读）
3. 应用调用 read() 读取数据  ← 应用自己读
4. 处理数据
```

**Proactor 模型（io_uring）**：
```
1. 应用提交异步 IO 请求（io_uring）
2. 应用可以继续做其他事
3. IO 操作完成（数据已经读到缓冲区）← 内核自己读
4. 内核通知应用：IO 完成，数据在这里
```

**关键区别**：Proactor 中，IO 操作由内核完成；Reactor 中，IO 操作由应用完成。

#### 多 Proactor 方案

```
┌──────────────────────────────────────────────────────────────┐
│                    VMess Server 多 Proactor 架构               │
│                                                              │
│  ┌────────────┐   ┌────────────┐   ┌────────────┐          │
│  │ Main Thread │   │ Worker 1   │   │ Worker 2   │          │
│  │ (Acceptor) │   │ (Proactor) │   │ (Proactor) │          │
│  │             │   │  io_uring  │   │  io_uring  │          │
│  └─────┬───────┘   └─────┬──────┘   └─────┬──────┘          │
│        │                  │                  │                  │
│        │ 新连接负载均衡   │                  │                  │
│        └──────────────────┴──────────────────┘                  │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

**工作流程**：
1. **主线程**：接受新连接，通过负载均衡算法分配给 Worker 线程
2. **Worker 线程**：每个线程运行自己的 io_uring 实例和事件循环

### 5.4 为什么还需要协程？

即使使用多线程，协程仍然是必要的：

1. **简化异步代码**：协程让异步代码看起来像同步代码
2. **减少线程切换开销**：协程切换比线程切换轻量得多
3. **高并发支持**：单个线程可以处理成千上万个协程

### 5.5 针对 2 核服务器的优化建议

对于只有 2 个核心的服务器，推荐**适度多线程**方案：

```
方案 A：单线程 + 协程（适合连接数 < 1k）
┌─────────────────────────────────┐
│  Single Thread (Event Loop)    │
│  - accept()                    │
│  - read/write (io_uring)      │
│  - 协程切换                    │
└─────────────────────────────────┘

方案 B：2 线程 + 协程（推荐）
┌─────────────────┐  ┌─────────────────┐
│  Thread 1       │  │  Thread 2       │
│  - accept()     │  │  - 处理连接     │
│  - 负载均衡     │  │  - 协程切换     │
└─────────────────┘  └─────────────────┘

方案 C：多 Reactor 模型（学习用）
┌─────────────────┐  ┌─────────────────┐
│  Main Reactor   │  │  Sub Reactor 1  │
│  - accept()     │  │  - 处理连接     │
└─────────────────┘  └─────────────────┘
                       ┌─────────────────┐
                       │  Sub Reactor 2  │
                       │  - 处理连接     │
                       └─────────────────┘
```

**推荐**：作为学习项目，可以实现**多 Reactor 模型**，但如果部署在 2 核服务器上，可以只启动 2 个 Sub Reactor。

---

## 6. 项目运行流程详解

### 6.1 角度 1：进程启动过程

```
1. 解析命令行参数
   └─ ./vmess-server --config /etc/vmess/server.yaml

2. 加载配置文件（YAML）
   └─ 解析监听地址、端口、用户列表、IO 设置等

3. 初始化日志系统
   └─ 设置日志级别、输出文件等

4. 初始化用户管理器（加载 UUID 列表）
   └─ 从配置文件加载用户，构建 UUID → User 映射

5. 初始化会话管理器（防重放）
   └─ 初始化会话 ID 存储结构

6. 创建监听 socket，绑定地址和端口
   └─ socket() → bind() → listen()

7. 初始化 io_uring 实例
   └─ io_uring_queue_init()

8. 初始化协程调度器
   └─ 创建调度器实例，关联 io_uring

9. 提交第一个 async_accept 操作
   └─ 将监听 socket 的 accept 操作提交给 io_uring

10. 进入主事件循环
    └─ for (;;) {
          io_uring_submit_and_wait(&ring_, 1);
          for each cqe in cq:
              resume_coroutine(cqe.user_data);
      }
```

### 6.2 角度 2：连接处理过程

```
1. 新连接到达（io_uring 完成 async_accept）
   └─ io_uring 返回新的 client_fd

2. 创建新的协程 handle_connection()
   └─ 为这个新连接创建一个协程任务

3. 协程启动，co_await async_read() 读取 VMess 头部
   └─ 提交读取操作给 io_uring，协程挂起

4. IO 提交给 io_uring，协程挂起
   └─ 控制权返回给调度器

5. 调度器处理其他就绪协程
   └─ 继续处理其他连接的 IO 事件

6. io_uring 完成读取，恢复 handle_connection 协程
   └─ io_uring 返回读取的字节数，协程恢复执行

7. 解析 VMess 头部
   └─ 验证协议版本、校验和等

8. 校验用户 UUID
   └─ 查找用户管理器，验证 UUID 是否存在

9. 防重放检查
   └─ 检查会话 ID 是否已存在

10. co_await async_connect() 连接目标服务器
    └─ 提交连接操作给 io_uring，协程挂起

11. io_uring 完成连接，恢复协程
    └─ 连接成功，获取 target_fd

12. 进入双向中转循环
    └─ for (;;) {
          co_await async_read() 从客户端读取
          decrypt() 解密数据
          co_await async_write() 加密后写入目标
          
          co_await async_read() 从目标读取
          encrypt() 加密数据
          co_await async_write() 解密后写入客户端
      }

13. 连接关闭，清理资源
    └─ co_await async_close() 关闭 socket
    └─ 释放协程资源
```

### 6.3 时序图

```
进程启动:
  main() → load_config() → init_log() → init_users() 
  → init_sessions() → create_socket() → init_io_uring() 
  → init_scheduler() → submit_accept() → event_loop()

连接处理:
  客户端连接 → io_uring 完成 accept → 创建协程 
  → co_await async_read() → 协程挂起 → 调度器处理其他协程 
  → io_uring 完成 read → 恢复协程 → 解析头部 
  → 校验用户 → 防重放检查 → co_await async_connect() 
  → 协程挂起 → io_uring 完成 connect → 恢复协程 
  → 双向中转循环 → 连接关闭 → 清理资源
```

---

## 7. 总结

### 7.1 协程调度机制

- `co_await` 挂起当前协程，控制权返回给调度器
- 调度器负责选择下一个就绪的协程执行
- 在 io_uring 场景中，IO 完成后会将对应的协程加入就绪队列

### 7.2 C++ 协程体系

- **C++20**：提供协程语法和底层机制
- **C++23**：部分协程库支持
- **C++26**：提供 std::execution，构成完整的异步编程体系

### 7.3 现代 C++ 特性

- **Lambda 表达式**：用于定义内联回调函数
- **std::move_only_function**：用于存储协程回调
- **智能指针**：用于内存管理
- **原子操作**：用于无锁编程
- **std::expected**：用于错误处理
- **Concepts**：用于模板约束

### 7.4 多线程 + 协程方案

- **主线程**：接受新连接，负载均衡分配给工作线程
- **工作线程**：每个线程运行一个事件循环，处理分配给它的连接
- **协程**：每个连接一个协程，简化异步代码

### 7.5 Go Channel 的 C++ 实现

- **方案 1**：基于协程手动实现 Channel
- **方案 2**：使用现有的 C++ 异步库（cppcoro、libunifex、stdexec）
- **重要更正**：C++26 标准中没有 `std::channel`

---

## 8. 参考资料

- C++20 协程教程：https://lewissbaker.github.io/
- cppcoro GitHub：https://github.com/lewissbaker/cppcoro
- libunifex GitHub：https://github.com/facebookexperimental/libunifex
- stdexec GitHub：https://github.com/NVIDIA/stdexec
- C++26 std::execution 提案：P2300
- Go Channel 设计：https://go.dev/blog/pipelines
