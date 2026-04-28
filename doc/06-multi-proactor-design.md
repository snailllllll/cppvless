# 多 Proactor 架构设计

> 版本：v1.0 DRAFT
> 整理时间：2026-04-28
> 内容：多 Proactor 模型中主线程与 Worker 线程的分工、通信、任务分派机制

---

## 1. 核心问题列表

1. 主线程和 Worker 线程的分工分别是什么？
2. 单个 TCP 连接是否一直在同一个 Worker 线程上处理？
3. 是否涉及到工作队列？
4. 主线程如何向 Worker 线程分派任务？
5. 主线程与 Worker 线程的通信方法？Pipe？Eventfd？
6. 如果是 Pipe/Eventfd，里面传递的内容是什么？
7. 主线程如何控制 Worker 线程？比如传递各种信号？

---

## 2. 主线程与 Worker 线程的分工

### 2.1 主线程（Main Thread）职责

```
┌──────────────────────────────────────────────────────────────┐
│                    Main Thread 职责                          │
│                                                              │
│  1. 启动和初始化                                              │
│     - 加载配置文件                                            │
│     - 初始化用户管理器                                        │
│     - 初始化会话管理器                                        │
│     - 创建监听 socket                                         │
│                                                              │
│  2. 接受新连接（Acceptor）                                   │
│     - accept() 或 io_uring async_accept                      │
│     - 获取新的 client_fd                                      │
│                                                              │
│  3. 负载均衡（Load Balancer）                                │
│     - 选择连接数最少的 Worker                                 │
│     - 或轮询（Round Robin）                                   │
│     - 将新连接分配给选中的 Worker                             │
│                                                              │
│  4. 监控和控制                                               │
│     - 接收 SIGINT/SIGTERM，优雅关闭                          │
│     - 统计信息收集（总连接数、QPS 等）                       │
│     - 健康检查                                                │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 Worker 线程职责

```
┌──────────────────────────────────────────────────────────────┐
│                    Worker Thread 职责                        │
│                                                              │
│  1. 运行自己的事件循环                                       │
│     - 初始化自己的 io_uring 实例                              │
│     - 运行事件循环：io_uring_submit_and_wait()              │
│                                                              │
│  2. 处理分配给自己的连接                                     │
│     - 从主线程接收新连接（通过某种通信机制）                  │
│     - 将新连接的 fd 注册到自己的 io_uring                    │
│     - 为每个连接创建协程（handle_connection）                 │
│                                                              │
│  3. 协程调度                                                │
│     - 当 io_uring 完成 IO 操作时，恢复对应的协程            │
│     - 处理协程的挂起和恢复                                   │
│                                                              │
│  4. 连接清理                                                │
│     - 连接关闭时，清理资源                                    │
│     - 更新统计信息                                            │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 关键设计问题

### 3.1 问题 1：单个 TCP 连接是否一直在同一个 Worker 线程上处理？

**回答：是的，通常应该这样。**

**原因**：
1. **避免锁**：如果每个连接只在一个线程上处理，就不需要锁来保护连接状态
2. **缓存局部性**：连接状态在 CPU 缓存中，切换线程会导致缓存失效
3. **简化设计**：不需要考虑跨线程的任务迁移

**例外情况**：
- 如果需要动态负载均衡（某个 Worker 过载，将连接迁移到另一个 Worker）
- 但这种场景比较复杂，初期不建议实现

### 3.2 问题 2：是否涉及到工作队列？

**传统方案（Reactor + 线程池）**：需要一个共享的工作队列

```
┌────────────┐     ┌────────────┐     ┌────────────┐
│  Main      │     │  Work      │     │  Worker    │
│  Reactor   │────▶│  Queue     │────▶│  Threads   │
└────────────┘     └────────────┘     └────────────┘
                      ↑ 需要锁
```

**多 Proactor 方案**：每个 Worker 有自己的事件循环，**不需要共享工作队列**

```
┌────────────┐     ┌────────────┐
│  Main      │     │  Worker 1  │
│  Thread    │────▶│  (有自己的 │
│  (Acceptor)│     │   io_uring)│
└────────────┘     └────────────┘
       │
       │           ┌────────────┐
       └──────────▶│  Worker 2  │
                   │  (有自己的 │
                   │   io_uring)│
                   └────────────┘
```

**关键区别**：
- 传统方案：主线程接收连接，放入队列，Worker 从队列取出
- 多 Proactor：主线程直接将 fd 传递给 Worker，Worker 自己处理

**那么，主线程如何"传递" fd 给 Worker？**

这就涉及到**线程间通信机制**。

---

## 4. 线程间通信机制

### 4.1 方案 1：Pipe（管道）

**原理**：
- 每个 Worker 线程创建一个 Pipe
- 主线程向 Pipe 写入数据（如新连接的 fd）
- Worker 线程通过 io_uring 监听 Pipe 的读端
- 当 Pipe 可读时，Worker 读取数据，获取新连接

**通信流程**：

```
主线程                 Worker 线程
  │                        │
  │  1. accept() 获得 fd   │
  │                        │
  │  2. 选择 Worker         │
  │                        │
  │  3. write(pipe_fd, &fd)│
  │  ──────────────────────▶│
  │                        │
  │                        │  4. io_uring 检测到 Pipe 可读
  │                        │
  │                        │  5. read(pipe_fd, &fd)
  │                        │
  │                        │  6. 将 fd 注册到自己的 io_uring
  │                        │
  │                        │  7. 创建协程处理连接
  │                        │
```

**Pipe 中传递的内容**：

```cpp
// 主线程向 Pipe 写入的数据结构
struct NewConnectionMessage {
    int client_fd;           // 新连接的文件描述符
    struct sockaddr_in addr; // 客户端地址（可选）
};

// 主线程代码
void dispatch_to_worker(int client_fd, Worker* worker) {
    NewConnectionMessage msg;
    msg.client_fd = client_fd;
    // 填充 addr...
    
    write(worker->pipe_write_fd(), &msg, sizeof(msg));
}

// Worker 线程代码
Task<void> watch_pipe(Worker* self) {
    for (;;) {
        NewConnectionMessage msg;
        auto n = co_await self->async_read(
            self->pipe_read_fd(), 
            std::span(reinterpret_cast<uint8_t*>(&msg), sizeof(msg))
        );
        
        // 处理新连接
        self->handle_new_connection(msg.client_fd);
    }
}
```

**Pipe 的缺点**：
- 需要额外的系统调用（read/write）
- 传递的数据量小（只有一个 fd），但仍然是额外开销

### 4.2 方案 2：Eventfd + SPSC 无锁队列

**原理**：
- 使用 `eventfd` 通知 Worker 线程
- 使用 SPSC（Single Producer, Single Consumer）无锁队列传递数据

**通信流程**：

```
主线程                 Worker 线程
  │                        │
  │  1. 将新连接 fd 放入    │
  │     SPSC 无锁队列       │
  │                        │
  │  2. write(eventfd, 1)  │
  │  ──────────────────────▶│
  │                        │
  │                        │  3. io_uring 检测到 eventfd 可读
  │                        │
  │                        │  4. read(eventfd)
  │                        │
  │                        │  5. 从 SPSC 队列取出新连接 fd
  │                        │
  │                        │  6. 处理新连接
  │                        │
```

**为什么用 SPSC 无锁队列？**

- **SPSC**：Single Producer, Single Consumer（单生产者，单消费者）
- **无锁**：使用原子操作，不需要互斥锁
- **高性能**：生产者（主线程）和消费者（Worker 线程）不会竞争锁

**SPSC 无锁队列的实现思路**：

```cpp
// SPSC 无锁队列（每个 Worker 一个）
template<typename T>
class SPSCQueue {
public:
    void push(T&& value) {
        // 只有一个生产者（主线程），不需要 CAS
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % capacity_;
        
        // 等待队列不满
        while (next_head == tail_.load(std::memory_order_acquire)) {
            // 自旋等待
        }
        
        buffer_[head] = std::forward<T>(value);
        head_.store(next_head, std::memory_order_release);
    }
    
    bool pop(T& value) {
        // 只有一个消费者（Worker 线程），不需要 CAS
        size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        value = std::move(buffer_[tail]);
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return true;
    }
    
private:
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::vector<T> buffer_;
    size_t capacity_;
};
```

**Eventfd 的优点**：
- 比 Pipe 更轻量（只需要传递一个 8 字节的整数）
- 专门用于事件通知

**实现示例**：

```cpp
// 主线程代码
void dispatch_to_worker(int client_fd, Worker* worker) {
    // 将 fd 放入 Worker 的 SPSC 队列
    worker->pending_queue_.push(client_fd);
    
    // 通知 Worker
    uint64_t val = 1;
    write(worker->eventfd(), &val, sizeof(val));
}

// Worker 线程代码
void Worker::init() {
    // 创建 eventfd
    eventfd_ = eventfd(0, EFD_NONBLOCK);
    
    // 将 eventfd 注册到 io_uring
    // 当 eventfd 可读时，表示有新连接
    co_spawn(watch_eventfd());
}

Task<void> Worker::watch_eventfd() {
    for (;;) {
        // 等待 eventfd 可读
        uint64_t val;
        co_await async_read(eventfd_, std::span<uint8_t>(
            reinterpret_cast<uint8_t*>(&val), sizeof(val)
        ));
        
        // 处理所有待处理的连接
        int fd;
        while (pending_queue_.pop(fd)) {
            handle_new_connection(fd);
        }
    }
}
```

### 4.3 方案 3：IO 多路复用（主线程 + 每个 Worker 都有自己的 io_uring）

**原理**：
- 主线程和每个 Worker 都有自己的 io_uring 实例
- 主线程通过某种机制将新连接"传递"给 Worker

**问题**：如何将 fd 从一个 io_uring 传递到另一个 io_uring？

**回答**：fd 是进程级别的，可以在线程间传递。但需要确保线程安全。

**推荐方法**：使用 `eventfd` + 无锁队列

### 4.4 方案 4：SO_REUSEPORT（最简单）

**原理**：
- 多个线程（或进程）都绑定到同一个端口
- 内核自动将新连接分配给不同的线程

**优点**：
- 不需要主线程接受连接
- 不需要线程间通信
- 内核自动负载均衡

**缺点**：
- 控制力弱（无法自定义负载均衡策略）
- 需要内核支持（Linux 3.9+）

**实现示例**：

```cpp
// 每个 Worker 线程都创建自己的监听 socket
void Worker::run() {
    // 创建 socket，设置 SO_REUSEPORT
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    bind(listen_fd, ...);
    listen(listen_fd, ...);
    
    // 将 listen_fd 注册到自己的 io_uring
    co_spawn(accept_loop(listen_fd));
}

Task<void> Worker::accept_loop(int listen_fd) {
    for (;;) {
        int client_fd = co_await async_accept(listen_fd);
        co_spawn(handle_connection(client_fd));
    }
}
```

**问题**：这种模式不需要主线程？

**回答**：是的，不需要主线程接受连接。但可能仍然需要一个主线程来：
- 加载配置
- 信号处理
- 统计信息收集

### 4.5 重要说明：所有方案都是每个 Worker 有自己的 io_uring

**关键理解**：在多 Proactor 模型中，**每个 Worker 线程有自己的 io_uring 实例是标准做法**。

三种方案的区别仅在于**谁来 accept 新连接**：

```
方案 1：主线程 accept + Pipe 通知
┌────────────┐     ┌──────────────────┐
│  Main      │     │  Worker 1        │
│  Thread    │────▶│  - 自己的 io_uring │  ← 有自己的 io_uring
│  (accept)  │     │  - 自己的事件循环  │
└────────────┘     └──────────────────┘
       │
       │           ┌──────────────────┐
       └──────────▶│  Worker 2        │
                    │  - 自己的 io_uring │  ← 有自己的 io_uring
                    │  - 自己的事件循环  │
                    └──────────────────┘

方案 2：主线程 accept + Eventfd 通知
（架构同方案 1，只是通信机制不同）
- 每个 Worker 仍然有自己的 io_uring

方案 3：SO_REUSEPORT（无主线程）
┌──────────────────┐  ┌──────────────────┐
│  Worker 1        │  │  Worker 2        │
│  - 自己的 listen │  │  - 自己的 listen │
│  - 自己的 io_uring │  │  - 自己的 io_uring │
│  - 自己的 accept  │  │  - 自己的 accept  │
└──────────────────┘  └──────────────────┘
（内核自动负载均衡新连接）
```

**结论**：所有方案都是每个 Worker 有自己的 io_uring，区别仅在于**谁来 accept 新连接**。

---

## 5. 推荐方案：主线程 + Eventfd + 无锁队列

综合考虑，我推荐**方案 2：Eventfd + 无锁队列**。

### 5.1 架构图

```
┌──────────────────────────────────────────────────────────────┐
│                    VMess Server 架构                        │
│                                                              │
│  ┌────────────────────────────────────────────────────┐     │
│  │                   Main Thread                       │     │
│  │                                                    │     │
│  │  1. 接受新连接（accept）                          │     │
│  │  2. 负载均衡：选择 Worker                         │     │
│  │  3. 将 fd 放入 Worker 的无锁队列                 │     │
│  │  4. write(eventfd) 通知 Worker                    │     │
│  │                                                    │     │
│  └────────────────┬───────────────────────────────────┘     │
│                   │                                          │
│                   │ eventfd + 无锁队列                       │
│                   ▼                                          │
│  ┌────────────────────────────────────────────────────┐     │
│  │                   Worker 1                         │     │
│  │                                                    │     │
│  │  ┌─────────────┐      ┌─────────────────────┐    │     │
│  │  │ Eventfd      │      │  io_uring 实例      │    │     │
│  │  │ (事件通知)   │      │  - async_read        │    │     │
│  │  └─────────────┘      │  - async_write       │    │     │
│  │  ┌─────────────┐      │  - async_accept      │    │     │
│  │  │ 无锁队列     │      │  - ...               │    │     │
│  │  │ (待处理连接) │      └─────────────────────┘    │     │
│  │  └─────────────┘                                 │     │
│  │                                                    │     │
│  └────────────────┬───────────────────────────────────┘     │
│                   │                                          │
│                   │ eventfd + 无锁队列                       │
│                   ▼                                          │
│  ┌────────────────────────────────────────────────────┐     │
│  │                   Worker 2                         │     │
│  │                                                    │     │
│  │  ┌─────────────┐      ┌─────────────────────┐    │     │
│  │  │ Eventfd      │      │  io_uring 实例      │    │     │
│  │  │ (事件通知)   │      │  - async_read        │    │     │
│  │  └─────────────┘      │  - async_write       │    │     │
│  │  ┌─────────────┐      │  - async_accept      │    │     │
│  │  │ 无锁队列     │      │  - ...               │    │     │
│  │  │ (待处理连接) │      └─────────────────────┘    │     │
│  │  └─────────────┘                                 │     │
│  │                                                    │     │
│  └────────────────────────────────────────────────────┘     │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 详细流程

#### 主线程流程

```cpp
// 主线程代码
void MainThread::run() {
    // 1. 初始化
    load_config();
    init_user_manager();
    init_session_manager();
    
    // 2. 创建监听 socket
    int listen_fd = create_listen_socket();
    
    // 3. 创建 Worker 线程
    for (int i = 0; i < num_workers_; ++i) {
        workers_[i] = new Worker();
        workers_[i]->start();
    }
    
    // 4. 接受新连接，分派给 Worker
    for (;;) {
        int client_fd = accept(listen_fd, ...);
        
        // 负载均衡：选择连接数最少的 Worker
        Worker* worker = select_worker();
        
        // 将 fd 放入 Worker 的队列
        worker->pending_queue_.push(client_fd);
        
        // 通知 Worker
        uint64_t val = 1;
        write(worker->eventfd(), &val, sizeof(val));
    }
}

Worker* MainThread::select_worker() {
    // 策略 1：轮询
    static std::atomic<int> index{0};
    return workers_[index.fetch_add(1) % workers_.size()];
    
    // 策略 2：最少连接数
    // return *std::min_element(workers_.begin(), workers_.end(),
    //     [](Worker* a, Worker* b) {
    //         return a->connection_count() < b->connection_count();
    //     });
}
```

#### Worker 线程流程

```cpp
// Worker 线程代码
class Worker {
public:
    Worker() {
        // 创建 eventfd
        eventfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        
        // 初始化 io_uring
        io_uring_queue_init(QUEUE_DEPTH, &ring_, 0);
    }
    
    void start() {
        // 启动事件循环线程
        thread_ = std::thread([this] { run(); });
    }
    
    int eventfd() const { return eventfd_; }
    
    void run() {
        // 提交对 eventfd 的异步读取
        co_spawn(watch_eventfd());
        
        // 主事件循环
        for (;;) {
            io_uring_submit_and_wait(&ring_, 1);
            
            // 处理完成的 IO 操作
            io_uring_cqe* cqe;
            unsigned head;
            io_uring_for_each_cqe(&ring_, &head) {
                // 根据 cqe->user_data 恢复对应的协程
                auto* coroutine = reinterpret_cast<Coroutine*>(cqe->user_data);
                coroutine->resume();
                
                io_uring_cqe_seen(&ring_, cqe);
            }
        }
    }
    
    Task<void> watch_eventfd() {
        for (;;) {
            // 异步读取 eventfd
            uint64_t val;
            co_await async_read(eventfd_, std::span<uint8_t>(
                reinterpret_cast<uint8_t*>(&val), sizeof(val)
            ));
            
            // 处理所有待处理的连接
            while (!pending_queue_.empty()) {
                int client_fd = pending_queue_.pop();
                handle_new_connection(client_fd);
            }
        }
    }
    
    void handle_new_connection(int client_fd) {
        // 将 client_fd 注册到自己的 io_uring
        // 创建协程处理这个连接
        co_spawn(handle_connection(client_fd));
    }
    
    Task<void> handle_connection(int client_fd) {
        // 处理 VMess 连接
        // ...
    }
    
private:
    int eventfd_;
    io_uring ring_;
    std::thread thread_;
    LockFreeQueue<int> pending_queue_;  // 无锁队列
    std::atomic<int> connection_count_{0};
};
```

### 5.3 Eventfd 中传递的内容

**Eventfd 本身只传递一个 8 字节的整数**，通常表示"事件计数"。

**在我们的场景中**：
- **主线程写入**：任意非零值（如 1），表示"有新连接待处理"
- **Worker 读取**：读取到的值表示"有多少次通知"（通常不需要关心具体值）

**实际数据（如 client_fd）通过共享的无锁队列传递**：

```
主线程                 Worker 线程
  │                        │
  │  1. pending_queue_.push(fd)                    │
  │                        │
  │  2. write(eventfd_, 1) │
  │  ──────────────────────▶│
  │                        │
  │                        │  3. read(eventfd_, &val)
  │                        │
  │                        │  4. fd = pending_queue_.pop()
  │                        │
  │                        │  5. handle_connection(fd)
  │                        │
```

---

## 6. 主线程如何控制 Worker 线程？

### 6.1 优雅关闭

**问题**：如何通知所有 Worker 线程停止？

**方案 1：使用 Pipe 传递控制命令**

```cpp
// 主线程向所有 Worker 发送停止命令
void MainThread::shutdown() {
    for (auto* worker : workers_) {
        ControlCommand cmd;
        cmd.type = CMD_STOP;
        worker->control_queue_.push(cmd);
        
        uint64_t val = 1;
        write(worker->control_eventfd(), &val, sizeof(val));
    }
    
    // 等待所有 Worker 退出
    for (auto* worker : workers_) {
        worker->thread_.join();
    }
}
```

**方案 2：使用原子变量**

```cpp
// 全局原子变量
std::atomic<bool> g_shutdown{false};

// Worker 线程定期检查
void Worker::run() {
    for (;;) {
        if (g_shutdown.load()) {
            // 优雅关闭：处理完现有连接后再退出
            drain_and_exit();
            break;
        }
        
        // 正常处理...
    }
}
```

**方案 3：使用信号（Signal）**

```cpp
// 主线程接收 SIGINT/SIGTERM
void signal_handler(int sig) {
    g_shutdown.store(true);
}

// Worker 线程使用 signalfd 监听信号
int signalfd = signalfd_init({SIGINT, SIGTERM});
```

### 6.2 动态调整 Worker 数量

**问题**：如何动态添加或移除 Worker？

**回答**：这比较复杂，初期不建议实现。可以预留接口，后期扩展。

### 6.3 统计信息收集

**问题**：如何收集所有 Worker 的统计信息？

**方案**：每个 Worker 定期更新共享的统计结构

```cpp
struct ServerStats {
    std::atomic<int64_t> total_connections{0};
    std::atomic<int64_t> active_connections{0};
    std::atomic<int64_t> bytes_sent{0};
    std::atomic<int64_t> bytes_received{0};
};

// 每个 Worker 更新统计信息
void Worker::update_stats(size_t bytes_sent, size_t bytes_received) {
    server_stats->bytes_sent.fetch_add(bytes_sent);
    server_stats->bytes_received.fetch_add(bytes_received);
}

// 主线程读取统计信息
void MainThread::print_stats() {
    std::cout << "Total connections: " 
              << server_stats->total_connections.load() << std::endl;
    // ...
}
```

---

## 7. 完整架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         VMess Server                                │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                     Main Thread                              │   │
│  │                                                              │   │
│  │  ┌────────────┐   ┌────────────┐   ┌────────────┐        │   │
│  │  │ 初始化      │   │ 接受连接    │   │ 负载均衡    │        │   │
│  │  └────────────┘   └────────────┘   └────────────┘        │   │
│  │        │                                                      │   │
│  │        │ eventfd + 无锁队列                                   │   │
│  │        ▼                                                      │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │                     Worker 1                          │  │   │
│  │  │                                                        │  │   │
│  │  │  ┌────────────┐   ┌─────────────────────────────┐    │  │   │
│  │  │  │ Eventfd    │   │  io_uring 实例              │    │  │   │
│  │  │  │ (事件通知) │   │  - async_read                │    │  │   │
│  │  │  └────────────┘   │  - async_write               │    │  │   │
│  │  │  ┌────────────┐   │  - async_connect             │    │  │   │
│  │  │  │ 无锁队列   │   │  - ...                       │    │  │   │
│  │  │  │ (待处理连接)│   └─────────────────────────────┘    │  │   │
│  │  │  └────────────┘                                     │  │   │
│  │  │  ┌────────────┐                                     │  │   │
│  │  │  │ 协程调度器  │                                     │  │   │
│  │  │  └────────────┘                                     │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  │        │                                                      │   │
│  │        │ eventfd + 无锁队列                                   │   │
│  │        ▼                                                      │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │                     Worker 2                          │  │   │
│  │  │                                                        │  │   │
│  │  │  ┌────────────┐   ┌─────────────────────────────┐    │  │   │
│  │  │  │ Eventfd    │   │  io_uring 实例              │    │  │   │
│  │  │  │ (事件通知) │   │  - async_read                │    │  │   │
│  │  │  └────────────┘   │  - async_write               │    │  │   │
│  │  │  ┌────────────┐   │  - async_connect             │    │  │   │
│  │  │  │ 无锁队列   │   │  - ...                       │    │  │   │
│  │  │  │ (待处理连接)│   └─────────────────────────────┘    │  │   │
│  │  │  └────────────┘                                     │  │   │
│  │  │  ┌────────────┐                                     │  │   │
│  │  │  │ 协程调度器  │                                     │  │   │
│  │  │  └────────────┘                                     │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 8. 总结

### 8.1 主线程与 Worker 线程的分工

- **主线程**：接受新连接、负载均衡、将连接分派给 Worker
- **Worker 线程**：运行自己的 io_uring 实例和事件循环，处理分配给自己的连接

### 8.2 单个 TCP 连接是否一直在同一个 Worker 线程上处理？

- **是的**：简化设计，避免锁和缓存失效

### 8.3 是否涉及到工作队列？

- **不需要共享工作队列**：每个 Worker 有自己的事件循环
- **但需要无锁队列**：主线程将新连接放入 Worker 的队列

### 8.4 主线程如何向 Worker 线程分派任务？

- 将新连接的 fd 放入 Worker 的无锁队列
- 通过 eventfd 通知 Worker

### 8.5 主线程与 Worker 线程的通信方法？

- **Eventfd**：用于事件通知（有新连接、停止等）
- **无锁队列**：用于传递数据（如新连接的 fd）

### 8.6 Eventfd 中传递的内容是什么？

- Eventfd 本身只传递一个 8 字节的整数（事件计数）
- 实际数据通过无锁队列传递

### 8.7 主线程如何控制 Worker 线程？

- **优雅关闭**：通过 eventfd + 控制队列传递命令
- **统计信息收集**：通过共享的原子变量

---

## 9. 参考资料

- eventfd man page：https://man7.org/linux/man-pages/man2/eventfd.2.html
- io_uring 与 eventfd 结合：https://kernel.dk/io_uring.pdf
- 无锁队列实现：https://github.com/cameron314/concurrentqueue
- SO_REUSEPORT：https://lwn.net/Articles/542629/
