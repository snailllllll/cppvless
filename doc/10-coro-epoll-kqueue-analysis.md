# C++20 协程 + epoll 实现分析：coro_epoll_kqueue

> 项目地址：https://github.com/franktea/coro_epoll_kqueue  
> 代码量：~500 行，非常适合学习 C++20 协程原理  
> 适用场景：理解协程挂起/恢复机制，epoll 事件驱动与协程的结合

---

## 一、项目架构概览

```
coro_epoll_kqueue/
├── task.h              # C++20 协程 Task 实现
├── awaiters.h          # 异步系统调用封装（Accept/Send/Recv）
├── socket.h/cpp       # Socket 封装，保存协程句柄
├── io_context.h/cpp   # epoll/kqueue 事件循环
└── echo_server.cpp     # 完整示例：echo 服务器
```

**核心设计思想**：

```
┌─────────────────────────────────────────────────────────┐
│                    C++20 协程层                        │
│                                                         │
│  task<T>  ──────────>  promise_type  <─────────────┐   │
│    │                     │                          │   │
│    │ co_await            │ initial_suspend()        │   │
│    │                     │ final_suspend()          │   │
│    │                     │ return_value()            │   │
│    ▼                     ▼                          │   │
│  awaiter {               continuation_              │   │
│    await_ready()         (父协程句柄)                │   │
│    await_suspend()       }                          │   │
│    await_resume()                                   │   │
│  }                                                  │   │
└──────────────────────────────────────────────────────┘   │
                                                         │
┌─────────────────────────────────────────────────────────┐
│                  AsyncSyscall 层                        │
│                                                         │
│  AsyncSyscall<Syscall, ReturnValue> (CRTP 基类)        │
│    │                                                    │
│    ├── Accept : AsyncSyscall<Accept, int>              │
│    ├── Send : AsyncSyscall<Send, ssize_t>              │
│    └── Recv : AsyncSyscall<Recv, ssize_t>              │
│                                                         │
│  关键逻辑：                                             │
│    1. 构造函数：注册 epoll 事件                         │
│    2. await_suspend()：尝试系统调用                    │
│    3. 如果 EAGAIN：保存协程句柄到 Socket               │
│    4. 析构函数：取消 epoll 事件                         │
└─────────────────────────────────────────────────────────┘
                                                         │
┌─────────────────────────────────────────────────────────┐
│                    epoll 事件循环层                     │
│                                                         │
│  IoContext {                                            │
│    epoll_fd_                                            │
│    run() {                                              │
│      epoll_wait()                                       │
│      for each event:                                    │
│        socket->ResumeRecv()  // 恢复读协程              │
│        socket->ResumeSend()  // 恢复写协程              │
│    }                                                    │
│  }                                                      │
└─────────────────────────────────────────────────────────┘
                                                         │
                                                         │
  协程挂起点 ──────────────────────────────────────────────┘
  (通过 Socket 的 coro_recv_/coro_send_ 恢复)
```

---

## 二、核心组件详解

### 2.1 协程 Task 实现（task.h）

#### 2.1.1 promise_type 设计

```cpp
template<typename T>
struct promise_type_base {
    coroutine_handle<> continuation_;  // 父协程句柄（谁在等待我）
    
    // 协程创建后先挂起，手动控制启动时机
    suspend_always initial_suspend() { return {}; }
    
    // 协程结束时的 awaiter
    struct final_awaiter {
        bool await_ready() noexcept { return false; }
        void await_resume() noexcept {}
        
        // 关键：协程结束时，自动恢复父协程
        template<typename promise_type>
        coroutine_handle<> await_suspend(coroutine_handle<promise_type> coro) noexcept {
            return coro.promise().continuation_;
        }
    };
    
    auto final_suspend() noexcept { return final_awaiter{}; }
};
```

**执行流程**：

```
1. 父协程调用 co_await child_task
   ↓
2. child_task.await_suspend(父协程句柄) 被调用
   ↓
3. 设置 child_promise.continuation_ = 父协程句柄
   ↓
4. 切换到子协程执行（return handle_）
   ↓
5. 子协程执行完毕，遇到 co_return
   ↓
6. final_suspend() 返回 final_awaiter
   ↓
7. final_awaiter.await_suspend() 返回 continuation_
   ↓
8. 自动切换回父协程
```

#### 2.1.2 task<T> 的 co_await 支持

```cpp
template<typename T = void>
struct task {
    using promise_type = detail::promise_type<T>;
    
    // 允许 co_await task_obj
    bool await_ready() { return false; }  // 总是挂起，立即执行子协程
    
    T await_resume() { return handle_.promise().result; }
    
    // 关键：当父协程 co_await 这个 task 时调用
    coroutine_handle<> await_suspend(coroutine_handle<> waiter) {
        // waiter = 父协程句柄
        // handle_ = 子协程句柄
        
        // 设置子协程的父协程为 waiter
        handle_.promise().continuation_ = waiter;
        
        // 返回子协程句柄，切换到子协程执行
        return handle_;
    }
    
    coroutine_handle<promise_type> handle_;
};
```

**为什么需要 continuation_？**

```
父协程：
  auto result = co_await child_task;  // ① 父协程挂起
                                          ↓
子协程：
  co_return 42;                       // ② 子协程结束
                                          ↓
自动恢复：
  final_awaiter.await_suspend()       // ③ 读取 continuation_
  → 返回父协程句柄                      // ④ 切换回父协程
                                          ↓
父协程：
  auto result = 42;                  // ⑤ 父协程继续
```

---

### 2.2 异步系统调用封装（awaiters.h）

#### 2.2.1 CRTP 模式（奇异递归模板模式）

```cpp
// 基类：定义异步系统调用的通用逻辑
template<typename Syscall, typename ReturnValue>
class AsyncSyscall {
public:
    bool await_ready() const noexcept { return false; }
    
    // 关键：尝试执行系统调用
    bool await_suspend(std::coroutine_handle<> h) noexcept {
        handle_ = h;  // 保存当前协程句柄
        
        // 调用子类的 Syscall() 方法（CRTP）
        value_ = static_cast<Syscall*>(this)->Syscall();
        
        // 检查是否需要等待（EAGAIN）
        suspended_ = value_ == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
        
        if(suspended_) {
            // 需要等待：保存协程句柄到 Socket
            static_cast<Syscall*>(this)->SetCoroHandle();
        }
        
        return suspended_;  // true = 挂起，false = 立即恢复
    }
    
    ReturnValue await_resume() noexcept {
        if(suspended_) {
            // 之前挂起了，现在重新调用获取结果
            value_ = static_cast<Syscall*>(this)->Syscall();
        }
        return value_;
    }
    
protected:
    bool suspended_;
    std::coroutine_handle<> handle_;
    ReturnValue value_;
};
```

#### 2.2.2 具体系统调用实现

**Recv 实现**：

```cpp
class Recv : public AsyncSyscall<Recv, int> {
public:
    Recv(Socket* socket, void* buffer, size_t len): AsyncSyscall(), 
        socket_(socket), buffer_(buffer), len_(len) {
        // 构造函数：注册 epoll 读事件
        socket_->io_context_.WatchRead(socket_);
    }

    ~Recv() {
        // 析构函数：取消 epoll 读事件
        socket_->io_context_.UnwatchRead(socket_);
    }

    ssize_t Syscall() {
        return ::recv(socket_->fd_, buffer_, len_, 0);
    }
    
    void SetCoroHandle() {
        // 保存协程句柄，等待 epoll 事件后恢复
        socket_->coro_recv_ = handle_;
    }
    
private:
    Socket* socket_;
    void* buffer_;
    std::size_t len_;
};
```

**执行流程**：

```
1. 协程调用：co_await socket.recv(buffer, 1024)
   ↓
2. 创建 Recv awaiter 对象
   ↓
3. Recv 构造函数：epoll_ctl ADD EPOLLIN
   ↓
4. await_suspend() 被调用
   ↓
5. 尝试 ::recv() → 返回 -1, errno = EAGAIN
   ↓
6. 保存协程句柄：socket_->coro_recv_ = handle_
   ↓
7. 返回 true（挂起协程）
   ↓
8. 【协程挂起，回到事件循环】
   ↓
9. epoll_wait() 返回，socket 可读
   ↓
10. IoContext::run() 调用 socket->ResumeRecv()
   ↓
11. coro_recv_.resume() → 协程恢复
   ↓
12. await_resume() 被调用
   ↓
13. 再次调用 ::recv() → 成功读取数据
   ↓
14. 返回读取的字节数
   ↓
15. Recv 析构函数：epoll_ctl MOD（移除 EPOLLIN）
   ↓
16. 协程继续执行业务逻辑
```

---

### 2.3 IO 事件循环（io_context_epoll.cpp）

#### 2.3.1 epoll 封装

```cpp
void IoContext::Attach(Socket* socket) {
    struct epoll_event ev;
    auto io_state = EPOLLIN | EPOLLET;  // 边缘触发模式
    ev.events = io_state;
    ev.data.ptr = socket;  // 关键：保存 Socket 指针
    
    epoll_ctl(fd_, EPOLL_CTL_ADD, socket->fd_, &ev);
    socket->io_state_ = io_state;
}

void IoContext::WatchRead(Socket* socket) {
    // 添加 EPOLLIN 事件
    auto new_state = socket->io_state_ | EPOLLIN;
    // 使用 EPOLL_CTL_MOD 修改
    UpdateState(new_state);
}
```

#### 2.3.2 事件循环

```cpp
void IoContext::run() {
    struct epoll_event events[max_events];
    
    for(;;) {
        int nfds = epoll_wait(fd_, events, max_events, -1);
        
        for(int i = 0; i < nfds; ++i) {
            // 从 epoll_event 中取回 Socket 指针
            auto socket = static_cast<Socket*>(events[i].data.ptr);
            
            if(events[i].events & EPOLLIN) {
                // 套接字可读，恢复等待读的协程
                socket->ResumeRecv();
            }
            if(events[i].events & EPOLLOUT) {
                // 套接字可写，恢复等待写的协程
                socket->ResumeSend();
            }
        }
    }
}
```

**关键点**：

1. **边缘触发（ET）**：只在状态变化时通知，需要一次性读取所有数据
2. **Socket 指针传递**：通过 `ev.data.ptr` 在 epoll 和应用层之间传递 Socket 对象
3. **协程恢复**：`ResumeRecv()` 和 `ResumeSend()` 直接调用 `coroutine_handle::resume()`

---

### 2.4 Socket 类（socket.h/cpp）

```cpp
class Socket {
private:
    IoContext& io_context_;
    int fd_ = -1;
    int32_t io_state_ = 0;  // 当前 epoll 事件状态
    
    // 关键：保存等待读写的协程句柄
    std::coroutine_handle<> coro_recv_;
    std::coroutine_handle<> coro_send_;
    
public:
    // 恢复读协程
    bool ResumeRecv() {
        if(!coro_recv_) { return false; }
        coro_recv_.resume();
        return true;
    }
    
    // 恢复写协程
    bool ResumeSend() {
        if(!coro_send_) { return false; }
        coro_send_.resume();
        return true;
    }
};
```

**为什么需要两个协程句柄？**

```
同一个 Socket 可能同时有两个协程在等待：
  - 协程 A：等待读数据（co_await recv）
  - 协程 B：等待写数据（co_await send）

所以需要分别保存：
  - coro_recv_ = 协程 A 的句柄
  - coro_send_ = 协程 B 的句柄

epoll 事件到来时：
  - EPOLLIN → ResumeRecv() → 恢复协程 A
  - EPOLLOUT → ResumeSend() → 恢复协程 B
```

---

## 三、完整执行流程示例

### 3.1 echo_server.cpp 分析

```cpp
// 业务逻辑协程
task<bool> inside_loop(Socket& socket) {
    char buffer[1024] = {0};
    
    // ① 异步读取：协程在这里挂起
    ssize_t recv_len = co_await socket.recv(buffer, sizeof(buffer));
    
    // ④ 读取完成，继续处理
    ssize_t send_len = 0;
    while(send_len < recv_len) {
        // ⑤ 异步发送：协程可能再次挂起
        ssize_t res = co_await socket.send(buffer + send_len, recv_len - send_len);
        if(res <= 0) { co_return false; }
        send_len += res;
    }
    
    co_return true;
}

// 每个客户端连接一个协程
task<> echo_socket(std::shared_ptr<Socket> socket) {
    for(;;) {
        bool b = co_await inside_loop(*socket);
        if(!b) break;
    }
}

// 接受连接协程
task<> accept(Socket& listen) {
    for(;;) {
        // ② 异步 accept：协程挂起
        auto socket = co_await listen.accept();
        
        // ③ 接受新连接，启动客户端协程
        auto t = echo_socket(socket);
        t.resume();
    }
}

int main() {
    IoContext io_context;
    Socket listen{"10009", io_context};
    
    // 启动 accept 协程
    auto t = accept(listen);
    t.resume();
    
    // 进入事件循环
    io_context.run();
}
```

### 3.2 时序图

```
客户端连接 → 发送 "Hello" → 接收 "Hello" → 断开

时间轴：
═══════════════════════════════════════════════════════════

主线程：
  main()
    ├─ IoContext io_context
    ├─ Socket listen{"10009", io_context}
    ├─ accept(listen).resume()  ──→  协程 A 启动
    └─ io_context.run()  ──→  进入事件循环
                                    │
                    ┌───────────────┘
                    │  epoll_wait() 阻塞
                    │
协程 A (accept)：   │
  co_await listen.accept()
    ├─ 创建 Accept awaiter
    ├─ epoll_ctl ADD EPOLLIN
    ├─ await_suspend() → accept() 返回 EAGAIN
    ├─ 保存协程句柄：socket->coro_recv_ = 协程 A
    └─ 返回 true → 协程 A 挂起 ◄──┘
                    │
                    │  【协程 A 挂起，回到事件循环】
                    │
                    │  epoll_wait() 阻塞...
                    │
═══════════════════│═════════════════════════════════════
客户端连接到来：     │  epoll_wait() 返回
                    ├─ 检查 events[i].events & EPOLLIN
                    ├─ socket->ResumeRecv()
                    │    └─ 协程 A.resume()  ──→  协程 A 恢复
                    │
协程 A (恢复)：     │
  await_resume()
    ├─ 再次调用 accept() → 返回新 fd
    ├─ co_return 新 Socket
    ├─ 协程 A 结束
    ├─ final_suspend() → 返回 continuation_ (nullptr)
    └─ 回到事件循环 ◄──┘
                    │
                    │  echo_socket(socket).resume()
                    │    └─ 协程 B 启动（处理客户端）
                    │
协程 B (echo)：     │
  co_await socket.recv(buffer, 1024)
    ├─ 创建 Recv awaiter
    ├─ epoll_ctl MOD EPOLLIN
    ├─ await_suspend() → recv() 返回 EAGAIN
    ├─ 保存协程句柄：socket->coro_recv_ = 协程 B
    └─ 返回 true → 协程 B 挂起 ◄──┘
                    │
                    │  【协程 B 挂起，回到事件循环】
                    │
                    │  epoll_wait() 阻塞...
                    │
═══════════════════│═════════════════════════════════════
客户端发送 "Hello"： │  epoll_wait() 返回
                    ├─ socket->ResumeRecv()
                    │    └─ 协程 B.resume()  ──→  协程 B 恢复
                    │
协程 B (恢复)：     │
  await_resume()
    ├─ 再次调用 recv() → 返回 5 ("Hello")
    ├─ 处理数据
    ├─ co_await socket.send(buffer, 5)  ──→  可能再次挂起
    └─ 发送完成，循环回去 recv ◄──┘
                    │
                    │  【协程 B 挂起等待下次数据】
                    │
                    │  epoll_wait() 阻塞...
                    │
═══════════════════│═════════════════════════════════════
客户端断开：         │  epoll_wait() 返回
                    ├─ EPOLLIN | EPOLLERR | EPOLLHUP
                    ├─ socket->ResumeRecv()
                    │    └─ 协程 B.resume()
                    │
协程 B：            │
  recv() → 返回 0 (客户端关闭)
    ├─ co_return false
    ├─ 协程 B 结束
    └─ Socket 析构，close(fd)
```

---

## 四、关键技术点解析

### 4.1 协程挂起与恢复的三种场景

#### 场景 1：立即完成（不需要挂起）

```cpp
// 假设缓冲区有数据
ssize_t Syscall() {
    return ::recv(fd, buffer, len, 0);  // 立即返回 1024
}

bool await_suspend(coroutine_handle<> h) {
    value_ = Syscall();
    suspended_ = (value_ == -1 && errno == EAGAIN);
    
    // suspended_ = false，不挂起
    return false;  // ← 协程立即恢复
}
```

#### 场景 2：需要等待（挂起协程）

```cpp
// 缓冲区没有数据
ssize_t Syscall() {
    return ::recv(fd, buffer, len, 0);  // 返回 -1, errno = EAGAIN
}

bool await_suspend(coroutine_handle<> h) {
    value_ = Syscall();
    suspended_ = (value_ == -1 && errno == EAGAIN);
    
    if(suspended_) {
        // 保存协程句柄
        socket_->coro_recv_ = h;
    }
    
    // suspended_ = true，挂起协程
    return true;  // ← 协程挂起，回到事件循环
}
```

#### 场景 3：协程恢复后重新获取结果

```cpp
// 协程恢复后
ReturnValue await_resume() noexcept {
    if(suspended_) {
        // 之前挂起了，现在重新调用系统调用
        value_ = static_cast<Syscall*>(this)->Syscall();
    }
    return value_;
}
```

### 4.2 epoll 事件管理

#### 问题：为什么需要保存 io_state_？

```cpp
class Socket {
    int32_t io_state_ = 0;  // 当前注册的事件
};

void IoContext::WatchRead(Socket* socket) {
    // 添加 EPOLLIN，但保留其他事件（如 EPOLLOUT）
    auto new_state = socket->io_state_ | EPOLLIN;
    UpdateState(new_state);
}

void UpdateState(new_state) {
    if(socket->io_state_ != new_state) {
        epoll_ctl(fd_, EPOLL_CTL_MOD, socket->fd_, &ev);
        socket->io_state_ = new_state;
    }
}
```

**原因**：

```
同一个 Socket 可能同时监听读和写：
  - 协程 A 在等待读（EPOLLIN）
  - 协程 B 在等待写（EPOLLOUT）
  
如果 WatchRead 直接设置 EPOLLIN，会覆盖 EPOLLOUT！

所以需要：
  1. 读取当前状态：io_state_
  2. 修改对应位：io_state_ | EPOLLIN
  3. 调用 EPOLL_CTL_MOD
  4. 保存新状态：socket->io_state_ = new_state
```

### 4.3 边缘触发（ET）与水平触发（LT）

#### 边缘触发（项目使用）

```cpp
void IoContext::Attach(Socket* socket) {
    auto io_state = EPOLLIN | EPOLLET;  // ← ET 模式
    ev.events = io_state;
}
```

**特点**：

```
客户端发送 2048 字节数据：

LT 模式：
  epoll_wait() 返回 → 应用读取 1024 字节 → 缓冲区还有 1024 字节
  epoll_wait() 会再次返回（因为缓冲区还有数据）

ET 模式：
  epoll_wait() 返回 → 应用读取 1024 字节 → 缓冲区还有 1024 字节
  epoll_wait() 不会返回（除非客户端再发送数据）

所以 ET 模式必须：
  1. 循环读取直到 EAGAIN
  2. 或者使用非阻塞 socket
```

#### 项目中的处理

```cpp
// 在 Recv awaiter 中
ssize_t Syscall() {
    return ::recv(socket_->fd_, buffer_, len_, 0);
}

// 在 echo_server.cpp 中
ssize_t recv_len = co_await socket.recv(buffer, sizeof(buffer));
// 这里只读取一次！如果数据超过 1024 字节，会丢失！

// 正确的 ET 模式处理：
task<bool> inside_loop(Socket& socket) {
    char buffer[1024];
    for(;;) {
        ssize_t n = co_await socket.recv(buffer, sizeof(buffer));
        if(n <= 0) break;
        // 处理数据...
    }
}
```

---

## 五、与 HXLibs 的对比

| 特性 | coro_epoll_kqueue | HXLibs |
|------|-------------------|--------|
| **IO 模型** | epoll/kqueue | io_uring |
| **代码量** | ~500 行 | 数千行 |
| **协程实现** | 简化 Task | Task + AioTask + Promise |
| **事件注册** | awaiter 构造/析构 | io_uring SQ 提交 |
| **完成通知** | epoll_wait | io_uring CQ 轮询 |
| **批量操作** | 不支持 | 支持（SQ 批量提交） |
| **学习曲线** | 平缓（适合入门） | 陡峭（生产级） |
| **性能** | 依赖 epoll 性能 | io_uring 更高性能 |
| **适用场景** | 学习、理解原理 | 生产环境、高性能服务 |

### 5.1 事件注册方式对比

**coro_epoll_kqueue（epoll）**：

```cpp
// 在 awaiter 构造函数中注册
Recv::Recv(Socket* socket, ...) {
    socket_->io_context_.WatchRead(socket_);  // epoll_ctl ADD
}

// 在 awaiter 析构函数中取消
Recv::~Recv() {
    socket_->io_context_.UnwatchRead(socket_);  // epoll_ctl MOD
}
```

**HXLibs（io_uring）**：

```cpp
// 在 Task 中提交 io_uring SQ
AioTask::AioTask(int fd, unsigned opcode, void* buf, unsigned nbytes) {
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, nbytes, 0);
    io_uring_sqe_set_data(sqe, this);  // 保存 this 指针
    io_uring_submit(&ring);  // 提交到内核
}

// 在 EventLoop 中处理完成事件
void EventLoop::run() {
    io_uring_peek_cqe(&ring, &cqe);
    auto* task = static_cast<AioTask*>(io_uring_cqe_get_data(cqe));
    task->_res = cqe->res;
    task->_previous.resume();  // 恢复协程
}
```

### 5.2 协程恢复方式对比

**coro_epoll_kqueue**：

```cpp
// 通过 Socket 对象保存协程句柄
socket->coro_recv_ = handle_;

// epoll 事件到来后恢复
void IoContext::run() {
    if(events[i].events & EPOLLIN) {
        socket->ResumeRecv();  // → coro_recv_.resume()
    }
}
```

**HXLibs**：

```cpp
// 通过 io_uring CQE 的 user_data 找到 Task
auto* task = static_cast<AioTask*>(io_uring_cqe_get_data(cqe));

// 恢复协程
task->_previous.resume();
```

---

## 六、对 vmess 代理项目的启发

### 6.1 协程化 vmess 代理的优势

#### 传统回调方式（难维护）

```cpp
// 传统方式：状态机 + 回调
class VmessConnection {
    enum State {
        HANDSHAKE,
        READ_COMMAND,
        CONNECT_REMOTE,
        RELAY_DATA
    };
    
    void onRead() {
        switch(state) {
            case HANDSHAKE:
                handleHandshake();
                break;
            case READ_COMMAND:
                handleCommand();
                break;
            // ...
        }
    }
};
```

#### 协程方式（易维护）

```cpp
// 协程方式：线性代码
task<> handleVmessConnection(Socket& client) {
    // 1. 握手
    co_await vmessHandshake(client);
    
    // 2. 读取命令
    auto cmd = co_await vmessReadCommand(client);
    
    // 3. 连接远程
    auto remote = co_await Socket::connect(cmd.host, cmd.port);
    
    // 4. 数据转发
    co_await relayData(client, remote);
}
```

### 6.2 实现建议

#### 建议 1：使用 io_uring（参考 HXLibs）

```cpp
// 封装 io_uring 的异步 IO
task<ssize_t> asyncRecv(int fd, void* buf, size_t len) {
    co_return co_await AioTask(fd, IORING_OP_READ, buf, len);
}

task<ssize_t> asyncSend(int fd, const void* buf, size_t len) {
    co_return co_await AioTask(fd, IORING_OP_WRITE, (void*)buf, len);
}
```

#### 建议 2：实现完全发送/接收（参考 HXLibs）

```cpp
task<> fullySend(Socket& socket, const void* data, size_t len) {
    size_t sent = 0;
    while(sent < len) {
        ssize_t n = co_await asyncSend(socket.fd(), 
                                      (char*)data + sent, 
                                      len - sent);
        if(n <= 0) throw std::runtime_error("send failed");
        sent += n;
    }
}

task<> fullyRecv(Socket& socket, void* data, size_t len) {
    size_t received = 0;
    while(received < len) {
        ssize_t n = co_await asyncRecv(socket.fd(), 
                                       (char*)data + received, 
                                       len - received);
        if(n <= 0) throw std::runtime_error("recv failed");
        received += n;
    }
}
```

#### 建议 3：协程化的 SOCKS5/vmess 代理

```cpp
task<> handleSocks5Connection(Socket& client) {
    // 1. 握手
    co_await fullyRecv(client, &handshake, sizeof(handshake));
    co_await fullySend(client, &response, sizeof(response));
    
    // 2. 连接请求
    co_await fullyRecv(client, &request, sizeof(request));
    auto remote = co_await Socket::connect(request.host, request.port);
    co_await fullySend(client, &reply, sizeof(reply));
    
    // 3. 数据转发
    auto t1 = relayData(client, remote);
    auto t2 = relayData(remote, client);
    co_await (t1 && t2);
}
```

---

## 七、学习路径建议

### 7.1 第一步：理解协程基础

```cpp
// 1. 最简单的协程
task<int> simple_coro() {
    std::cout << "Hello from coroutine\n";
    co_return 42;
}

// 2. 调用协程
task<void> caller() {
    int result = co_await simple_coro();
    std::cout << "Result: " << result << "\n";
}
```

**关键概念**：

1. **协程句柄（coroutine_handle）**：指向协程帧的指针
2. **promise_type**：协程的配置对象（如何挂起、如何恢复、如何返回值）
3. **awaiter**：定义 `co_await` 行为的对象（await_ready/await_suspend/await_resume）

### 7.2 第二步：理解异步 awaiter

```cpp
// 模拟异步操作
struct AsyncDelay {
    bool await_ready() { return false; }  // 总是挂起
    
    void await_suspend(coroutine_handle<> h) {
        // 模拟异步操作：1 秒后恢复协程
        std::thread([h]() {
            std::this_thread::sleep_for(1s);
            h.resume();
        }).detach();
    }
    
    void await_resume() {}
};

task<> demo() {
    std::cout << "Before delay\n";
    co_await AsyncDelay{};
    std::cout << "After delay\n";
}
```

### 7.3 第三步：结合 epoll

```cpp
// 参考 coro_epoll_kqueue 项目
// 1. 理解 AsyncSyscall 如何封装系统调用
// 2. 理解 IoContext 如何管理 epoll 事件
// 3. 理解 Socket 如何保存协程句柄
```

### 7.4 第四步：参考 HXLibs 实现 io_uring 版本

```cpp
// 1. 理解 io_uring 的 SQ/CQ 机制
// 2. 理解 AioTask 如何作为协程和 io_uring 的桥梁
// 3. 理解 EventLoop 如何处理完成事件
```

---

## 八、常见问题解答

### Q1: 为什么需要 continuation_？

**A**: 因为协程可能嵌套调用：

```cpp
task<int> child() {
    co_return 42;
}

task<void> parent() {
    int x = co_await child();  // 父协程挂起，子协程启动
    std::cout << x;            // 子协程结束后，需要自动回到父协程
}

// continuation_ 保存了父协程句柄
// 子协程结束时，通过 continuation_ 回到父协程
```

### Q2: 为什么 AsyncSyscall 要用 CRTP？

**A**: 因为 `await_suspend()` 中需要调用子类的 `Syscall()` 方法：

```cpp
template<typename Syscall, typename ReturnValue>
class AsyncSyscall {
    bool await_suspend(coroutine_handle<> h) {
        // 调用子类的 Syscall() 方法
        value_ = static_cast<Syscall*>(this)->Syscall();
        // ...
    }
};

class Recv : public AsyncSyscall<Recv, ssize_t> {
    ssize_t Syscall() {
        return ::recv(...);  // 具体实现
    }
};
```

如果不使用 CRTP，需要定义虚函数，会有性能损失。

### Q3: 边缘触发（ET）为什么要非阻塞 socket？

**A**: 因为 ET 模式只通知一次，需要确保将数据全部读取：

```cpp
// 错误示例（阻塞 socket + ET）
void onRead() {
    char buf[1024];
    int n = recv(fd, buf, 1024, 0);  // 如果数据超过 1024，会阻塞！
}

// 正确示例（非阻塞 socket + ET）
void onRead() {
    for(;;) {
        char buf[1024];
        int n = recv(fd, buf, 1024, 0);
        if(n == -1 && errno == EAGAIN) break;  // 数据读取完毕
        if(n <= 0) break;  // 连接关闭
        // 处理数据...
    }
}
```

### Q4: 为什么项目中的 echo_server 可能有 bug？

**A**: 因为 `recv` 只调用一次，如果数据超过缓冲区大小，会丢失：

```cpp
// 问题代码
task<bool> inside_loop(Socket& socket) {
    char buffer[1024] = {0};
    ssize_t recv_len = co_await socket.recv(buffer, sizeof(buffer));
    // 如果客户端发送 2000 字节，只能读取 1024 字节！
    // ...
}

// 修复：循环读取
task<bool> inside_loop(Socket& socket) {
    char buffer[1024];
    for(;;) {
        ssize_t n = co_await socket.recv(buffer, sizeof(buffer));
        if(n <= 0) break;
        // 处理数据...
    }
}
```

---

## 九、总结

### 9.1 项目优点

1. **代码简洁**：只有 ~500 行，非常适合学习
2. **完整示例**：echo_server.cpp 展示完整使用流程
3. **清晰展示协程机制**：awaiter 的三个方法（await_ready/suspend/resume）
4. **实用价值**：可以直接用于生产环境（需要修复 ET 模式的 bug）

### 9.2 项目缺点

1. **ET 模式处理不完整**：需要循环读取直到 EAGAIN
2. **错误处理不完善**：unhandled_exception() 直接 exit
3. **不支持批量操作**：每次只能提交一个 epoll 事件
4. **性能不如 io_uring**：epoll 每次操作都需要系统调用

### 9.3 学习价值

| 学习阶段 | 推荐项目 | 原因 |
|---------|---------|------|
| **入门 C++ 协程** | coro_epoll_kqueue | 代码简洁，完整示例 |
| **理解协程原理** | coro_epoll_kqueue | 清晰展示挂起/恢复机制 |
| **学习 io_uring** | HXLibs | 生产级实现，高性能 |
| **实现代理服务** | HXLibs + coro_epoll_kqueue | 参考两者设计 |

### 9.4 下一步行动

1. **编译运行 coro_epoll_kqueue**
   ```bash
   cd /data/workspace/vmess/doc/reference/coro_epoll_kqueue
   mkdir build && cd build
   cmake .. && make
   ./echo_server
   ```

2. **使用 telnet 测试**
   ```bash
   telnet 127.0.0.1 10009
   # 输入数据，应该回显
   ```

3. **参考实现 vmess 代理的协程版本**
   - 使用 io_uring（参考 HXLibs）
   - 实现完全发送/接收
   - 协程化处理 SOCKS5/vmess 协议

---

## 十、参考资料

1. **C++20 协程标准**：https://en.cppreference.com/w/cpp/language/coroutines
2. **epoll 官方文档**：`man epoll`
3. **io_uring 官方文档**：https://kernel.dk/io_uring.pdf
4. **参考项目**：
   - coro_epoll_kqueue：https://github.com/franktea/coro_epoll_kqueue
   - HXLibs：https://github.com/HengXin666/HXLibs
   - epoll-coroutine：https://github.com/Ender-events/epoll-coroutine

---

**文档版本**：v1.0  
**创建时间**：2026-04-29  
**作者**：基于 coro_epoll_kqueue 项目分析
