# uring_exec vs io_uring-examples-cpp 对比分析

**日期**: 2026-04-29  
**作者**: AI Assistant  
**目的**: 对比分析两个 io_uring C++20 协程项目的设计差异，为 vmess 项目提供设计参考

---

## 一、项目概述

### 1.1 uring_exec

**项目地址**: https://github.com/Caturra000/uring_exec  
**作者**: Caturra000  
**Stars**: 未统计  
**核心技术**:
- **C++26** `std::execution` (使用 NVIDIA stdexec 库)
- **io_uring** (liburing)
- **C++20 协程**
- **线程池** (多线程事件循环)

**项目定位**:
> "提供 liburing 的 std::execution 支持，是一个基于 std::execution 的网络库"

**关键特性**:
- ✅ Sender/Receiver 模型
- ✅ C++20 协程集成
- ✅ 线程池支持
- ✅ Stop token 支持
- ✅ 取消操作 (Cancellation)
- ✅ 信号处理 (Signal handling)
- ✅ 多线程安全 (MT-safe)

---

### 1.2 io_uring-examples-cpp

**项目地址**: https://github.com/Caturra000/io_uring-examples-cpp  
**作者**: Caturra000 (同一个作者)  
**Stars**: 未统计  
**核心技术**:
- **C++20** (不依赖 std::execution)
- **io_uring** (liburing)
- **C++20 协程** (手写 Task 实现，仅 200 行)
- **单线程** 事件循环

**项目定位**:
> "提供简单的 io_uring / liburing 使用示例，包括同步用法和 C++20 协程的异步用法"

**关键特性**:
- ✅ 简化的协程封装 (`include/coroutine.h`, 200 行)
- ✅ 单线程事件循环 (`Io_context`)
- ✅ 易于理解 (适合学习)
- ✅ 支持 io_uring 高级特性 (Multishot, SQPOLL, IO drain, IO link, Provided buffers)
- ❌ 不支持多线程
- ❌ 不支持 std::execution

---

## 二、工作模型对比

### 2.1 uring_exec (多线程 + 线程池)

```
┌─────────────────────────────────────────────────────────┐
│                    io_uring_exec                      │
├─────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│  │ Thread 0 │  │ Thread 1 │  │ Thread N │           │
│  │ (run())  │  │ (run())  │  │ (run())  │           │
│  └─────┬────┘  └─────┬────┘  └─────┬────┘           │
│        │              │              │                  │
│        └──────────────┼──────────────┘                  │
│                       │                                 │
│              ┌────────▼────────┐                        │
│              │   io_uring SQ  │                        │
│              └─────────────────┘                        │
└─────────────────────────────────────────────────────────┘

特性：
- 支持线程池 (std::jthread)
- 多线程安全 (MT-safe)
- 每个线程运行自己的 run() 循环
- 使用 stdexec::async_scope 管理协程生命周期
```

**代码示例** (线程池):

```cpp
// uring_exec 支持线程池
int main() {
    io_uring_exec uring({.uring_entries=512});
    stdexec::scheduler auto scheduler = uring.get_scheduler();
    exec::async_scope scope;

    constexpr size_t pool_size = 4;
    constexpr size_t user_number = 4;

    // 创建 4 个线程的线程池
    auto thread_pool = std::array<std::jthread, pool_size>{};
    for(auto &&j : thread_pool) {
        j = std::jthread([&](auto token) { uring.run(token); });
    }

    // 提交 4 个用户请求
    auto users = std::array<std::jthread, user_number>{};
    auto user_post_requests = [&] {
        for(auto i : std::views::iota(1) | std::views::take(10000)) {
            stdexec::sender auto s =
                stdexec::schedule(scheduler)
              | stdexec::then([&, i] { /* 处理请求 */ });
            scope.spawn(std::move(s));
        }
    };

    for(auto &&j : users) j = std::jthread(user_post_requests);
    for(auto &&j : users) j.join();
    
    // 等待所有任务完成
    stdexec::sync_wait(scope.on_empty());
}
```

---

### 2.2 io_uring-examples-cpp (单线程事件循环)

```
┌─────────────────────────────────────────┐
│          main thread                    │
├─────────────────────────────────────────┤
│  Io_context io_context{uring};         │
│  co_spawn(io_context, server(...));    │
│  io_context.run();  // 阻塞           │
└─────────────────────────────────────────┘

特性：
- 单线程事件循环
- 无锁设计
- 简单易理解
- 适合 IO 密集型任务
```

**代码示例** (单线程):

```cpp
// io_uring-examples-cpp 是单线程的
int main() {
    auto server_fd = make_server({.port=8848});
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    // 单线程事件循环
    Io_context io_context{uring};
    co_spawn(io_context, server(&uring, io_context, server_fd));
    io_context.run();  // 阻塞，直到 stop() 被调用
}
```

---

### 2.3 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **工作模型** | 多线程 (线程池) | 单线程 |
| **线程安全** | MT-safe | 仅单线程 |
| **适用场景** | CPU + IO 密集型 | IO 密集型 |
| **复杂度** | 高 (需要理解 std::execution) | 低 (易于学习) |
| **性能** | 高 (多核并行) | 中 (单核瓶颈) |
| **锁开销** | 低 (使用无锁队列) | 无 (单线程无锁) |

---

## 三、IO 模型对比

### 3.1 uring_exec (std::execution Sender)

**核心抽象**: `stdexec::sender`

```cpp
// 使用 Sender/Receiver 模型
stdexec::sender auto s =
    uring_exec::async_read(scheduler, client_fd, buf.data(), buf.size())
  | stdexec::then([](int read_bytes) {
        // 处理读取的数据
        return read_bytes;
    })
  | stdexec::let_value([&](int read_bytes) {
        // 链式调用下一个异步操作
        return uring_exec::async_write(scheduler, client_fd, buf.data(), read_bytes);
    });

// 启动 Sender
stdexec::sync_wait(std::move(s));
```

**优势**:
- ✅ 类型安全 (编译期检查)
- ✅ 组合性强 (使用 `|` 操作符)
- ✅ 支持取消 (Cancellation)
- ✅ 支持错误处理 (set_error)
- ✅ 支持 Stop token

**劣势**:
- ❌ 学习曲线陡峭 (需要理解 std::execution)
- ❌ 编译器支持要求高 (需要 C++26 + stdexec)
- ❌ 代码冗长 (需要写很多 boilerplate)

---

### 3.2 io_uring-examples-cpp (手写 Task + Awaiter)

**核心抽象**: `Task` + `Async_operation`

```cpp
// 使用 C++20 协程 (co_await)
Task echo(io_uring *uring, int client_fd) {
    char buf[4096];
    for(;;) {
        // co_await 异步读取
        auto n = co_await async_read(uring, client_fd, buf, std::size(buf)) | nofail("read");

        // 处理数据
        auto printer = std::ostream_iterator<char>{std::cout};
        std::ranges::copy_n(buf, n, printer);

        // co_await 异步写入
        n = co_await async_write(uring, client_fd, buf, n) | nofail("write");

        // 判断是否关闭连接
        bool close_proactive = n > 2 && buf[0] == 'Z' && buf[1] == 'z';
        bool close_reactive = (n == 0);
        if(close_reactive || close_proactive) {
            co_await async_close(uring, client_fd);
            break;
        }
    }
}
```

**优势**:
- ✅ 易于理解 (代码清晰)
- ✅ 学习曲线平缓 (适合入门)
- ✅ 编译器支持好 (C++20 即可)
- ✅ 代码简洁 (不需要写很多 boilerplate)

**劣势**:
- ❌ 类型安全性较弱 (运行时错误)
- ✅ 组合性较弱 (不支持 `|` 操作符)
- ❌ 不支持取消 (需要手动实现)
- ❌ 不支持 Stop token

---

### 3.3 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **IO 抽象** | std::execution Sender | Task + Awaiter |
| **类型安全** | 编译期检查 | 运行时检查 |
| **组合性** | 强 (`\|` 操作符) | 弱 (顺序 co_await) |
| **取消支持** | ✅ (原生支持) | ❌ (需要手动实现) |
| **Stop token** | ✅ (原生支持) | ❌ (不支持) |
| **学习曲线** | 陡峭 | 平缓 |
| **代码简洁性** | 中 (boilerplate 多) | 高 (代码清晰) |
| **编译器要求** | C++26 + stdexec | C++20 |

---

## 四、协程实现对比

### 4.1 uring_exec (基于 std::execution)

**协程返回类型**: `exec::task<T>` (stdexec 提供的 Task)

```cpp
// 使用 exec::task<T> (stdexec 库)
exec::task<int> async_connect(io_uring_exec::scheduler scheduler, int fd, ...) {
    // 重调度到指定的执行上下文
    co_await exec::reschedule_coroutine_on(scheduler);
    
    println("hello stdexec! and ...");
    
    // 等待 2 秒
    co_await uring_exec::async_wait(scheduler, 2s);
    
    // 异步写入
    std::string_view hi = "hello coroutine!\n";
    stdexec::sender auto s =
        uring_exec::async_write(scheduler, STDOUT_FILENO, hi.data(), hi.size());
    
    // co_return co_await
    co_return co_await std::move(s);
}
```

**关键特性**:
- ✅ 与 std::execution 深度集成
- ✅ 支持重调度 (`exec::reschedule_coroutine_on`)
- ✅ 类型安全 (编译期检查)
- ✅ 自动生命周期管理

---

### 4.2 io_uring-examples-cpp (手写 Task)

**协程返回类型**: `Task` (200 行代码实现)

```cpp
// include/coroutine.h (200 行)
struct Task {
    struct promise_type;
    constexpr Task(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ~Task() { if(_handle) _handle.destroy(); }
    auto detach() noexcept { return std::exchange(_handle, {}); }
    // Move ctor only.
    Task(Task &&rhs) noexcept: _handle(rhs.detach()) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;
    auto operator co_await() && noexcept;
private:
    std::coroutine_handle<promise_type> _handle;
};

struct Task::promise_type {
    constexpr auto initial_suspend() const noexcept { return std::suspend_always{}; }
    constexpr void return_void() const noexcept { /*exception_ptr...*/ }
    void unhandled_exception() { /*exception_ptr...*/ }
    Task get_return_object() noexcept {
        auto h = std::coroutine_handle<promise_type>::from_promise(*this);
        return {h};
    }
    struct Final_suspend {
        constexpr bool await_ready() const noexcept { return false; }
        auto await_suspend(auto callee) const noexcept {
            auto caller = callee.promise()._caller;
            // Started task (at least once) will kill itself in final_suspend.
            callee.destroy();
            return caller;
        }
        constexpr auto await_resume() const noexcept {}
    };
    constexpr auto final_suspend() const noexcept { return Final_suspend{}; }
    void push(std::coroutine_handle<> caller) noexcept { _caller = caller; }

    std::coroutine_handle<> _caller {std::noop_coroutine()};
};
```

**关键特性**:
- ✅ 代码简洁 (200 行)
- ✅ 易于理解 (适合学习)
- ✅ 零依赖 (不需要 stdexec)
- ❌ 功能有限 (不支持取消、Stop token)

---

### 4.3 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **协程返回类型** | `exec::task<T>` | `Task` |
| **代码量** | 依赖 stdexec (数千行) | 200 行 (手写) |
| **学习曲线** | 陡峭 | 平缓 |
| **取消支持** | ✅ (原生支持) | ❌ (不支持) |
| **Stop token** | ✅ (原生支持) | ❌ (不支持) |
| **重调度** | ✅ (`reschedule_coroutine_on`) | ❌ (不支持) |
| **生命周期管理** | 自动 (stdexec 管理) | 手动 (需要小心) |

---

## 五、代码组织对比

### 5.1 uring_exec (模块化设计)

```
uring_exec/
├── include/
│   ├── uring_exec.hpp              # 总入口
│   └── uring_exec/
│       ├── detail.h                # 底层细节
│       ├── underlying_io_uring.h  # io_uring 封装
│       ├── io_uring_exec.h        # 主接口
│       ├── io_uring_exec_internal.h      # 内部实现
│       ├── io_uring_exec_internal_run.h  # 事件循环
│       ├── io_uring_exec_operation.h     # Operation 实现
│       ├── io_uring_exec_sender.h       # Sender 实现
│       └── utils.h                # 工具函数
├── examples/                      # 示例代码
│   ├── echo_sender.cpp            # 使用 Sender 的 echo server
│   ├── hello_coro.cpp             # C++20 协程示例
│   ├── hello_world.cpp             # 基础示例
│   ├── per_operation_cancellation.cpp  # 取消操作示例
│   ├── signal_handling.cpp        # 信号处理示例
│   ├── stop_token.cpp             # Stop token 示例
│   ├── thread_pool.cpp            # 线程池示例
│   └── timer.cpp                  # 定时器示例
├── benchmarks/                    # 性能测试
├── tests/                         # 单元测试
├── Makefile
├── xmake.lua
└── README.md
```

**特点**:
- ✅ 模块化设计 (每个功能一个文件)
- ✅ 头文件库 (header-only)
- ✅ 易于扩展 (遵循 std::execution 规范)
- ❌ 代码分散 (需要跳多个文件)

---

### 5.2 io_uring-examples-cpp (单文件设计)

```
io_uring-examples-cpp/
├── include/
│   ├── coroutine.h                # 协程封装 (200 行)
│   ├── config.h                   # 配置
│   ├── utils.h                    # 工具函数
│   ├── co_context_switch.h        # 上下文切换
│   ├── feature_io_drain.h         # IO drain 特性
│   ├── feature_io_link.h          # IO link 特性
│   ├── feature_multishot.h       # Multishot 特性
│   ├── feature_provided_buffers.h # Provided buffers 特性
│   └── feature_sqpoll.h          # SQPOLL 特性
├── examples/                      # 示例代码
│   ├── cat.cpp                    # 类似 cat 命令
│   ├── echo.cpp                   # 使用回调的 echo server
│   ├── echo_coroutine.cpp         # 使用协程的 echo server
│   ├── test_multi_task.cpp        # co_await Task 测试
│   ├── feature_multishot.cpp      # Multishot 特性示例
│   ├── feature_multishot2.cpp     # Multishot + 协程示例
│   ├── feature_sqpoll.cpp         # SQPOLL 特性示例
│   ├── feature_io_drain.cpp       # IO drain 特性示例
│   ├── feature_io_link.cpp       # IO link 特性示例
│   ├── feature_provided_buffers.cpp  # Provided buffers 示例
│   └── test_context_switch.cpp   # 上下文切换测试
├── experimental/                  # 实验性代码
├── Makefile
└── README.md
```

**特点**:
- ✅ 代码集中 (核心代码在 `include/coroutine.h`)
- ✅ 易于学习 (适合入门)
- ✅ 示例丰富 (11 个示例)
- ❌ 不支持多线程
- ❌ 不支持 std::execution

---

### 5.3 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **代码组织** | 模块化 (9 个文件) | 单文件 (coroutine.h) |
| **核心代码量** | 依赖 stdexec (数千行) | 200 行 |
| **学习曲线** | 陡峭 | 平缓 |
| **示例数量** | 8 个 | 11 个 |
| **头文件库** | ✅ (header-only) | ✅ (header-only) |
| **易于扩展** | ✅ (遵循 std::execution) | ❌ (需要修改核心代码) |

---

## 六、性能对比

### 6.1 uring_exec (有 benchmark)

**Benchmark 结果** (来自 README):

| threads / sessions | asio (io_uring) | uring_exec |
|-------------------|------------------|------------|
| 2 / 10            | 1.868 GiB/s     | 3.409 GiB/s |
| 2 / 100           | 2.744 GiB/s     | 3.870 GiB/s |
| 2 / 1000          | 1.382 GiB/s     | 2.270 GiB/s |
| 4 / 10            | 1.771 GiB/s     | 3.164 GiB/s |
| 4 / 100           | 2.694 GiB/s     | 3.477 GiB/s |
| 4 / 1000          | 1.275 GiB/s     | 4.411 GiB/s |
| 8 / 10            | 0.978 GiB/s     | 2.522 GiB/s |
| 8 / 100           | 2.107 GiB/s     | 2.676 GiB/s |
| 8 / 1000          | 1.177 GiB/s     | 3.956 GiB/s |

**测试环境**:
- Linux v6.4.8
- AMD 5800H, 16 GB
- gcc v13.2.0 -O3
- ping-pong: blocksize = 16384, timeout = 5s

**结论**:
- ✅ uring_exec 性能优于 asio (io_uring)
- ✅ 多线程性能提升明显 (4 threads / 1000 sessions: 4.411 GiB/s)
- ✅ 即使单线程也比 asio 快 (2 threads / 10 sessions: 3.409 vs 1.868)

---

### 6.2 io_uring-examples-cpp (无 benchmark)

**性能特征**:
- ❌ 无官方 benchmark
- ✅ 单线程性能应该接近 uring_exec (因为都是基于 io_uring)
- ❌ 多线程性能未知 (因为不支持多线程)

**推测**:
- 单线程性能: 接近 uring_exec
- 多线程性能: 不支持 (单线程事件循环)

---

### 6.3 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **单线程性能** | 高 (3.409 GiB/s) | 推测: 高 (接近 uring_exec) |
| **多线程性能** | 高 (4.411 GiB/s) | 不支持 |
| **Benchmark** | ✅ (有官方数据) | ❌ (无官方数据) |
| **性能优化** | ✅ (无锁队列、线程池) | ❌ (单线程无优化空间) |

---

## 七、适用场景对比

### 7.1 uring_exec

**适合场景**:
- ✅ 高并发服务器 (10K+ 连接)
- ✅ CPU + IO 密集型任务
- ✅ 需要取消操作 (Cancellation)
- ✅ 需要 Stop token (优雅关闭)
- ✅ 需要多线程并行处理
- ✅ 团队熟悉 C++26 / std::execution

**不适合场景**:
- ❌ 学习 C++20 协程 (学习曲线陡峭)
- ❌ 快速原型开发 (boilerplate 多)
- ❌ 编译器不支持 C++26 / stdexec

---

### 7.2 io_uring-examples-cpp

**适合场景**:
- ✅ 学习 C++20 协程 + io_uring
- ✅ 快速原型开发
- ✅ IO 密集型任务 (单线程足够)
- ✅ 编译器只支持 C++20 (不支持 C++26)
- ✅ 代码简洁性优先

**不适合场景**:
- ❌ 高并发服务器 (10K+ 连接)
- ❌ CPU 密集型任务
- ❌ 需要取消操作
- ❌ 需要多线程并行处理

---

### 7.3 对比表格

| 场景 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **学习 C++20 协程** | ❌ (太复杂) | ✅ (推荐) |
| **快速原型开发** | ❌ (boilerplate 多) | ✅ (代码简洁) |
| **高并发服务器** | ✅ (推荐) | ❌ (单线程瓶颈) |
| **CPU + IO 密集型** | ✅ (推荐) | ❌ (单线程) |
| **IO 密集型** | ✅ | ✅ (推荐) |
| **需要取消操作** | ✅ (原生支持) | ❌ (不支持) |
| **需要多线程** | ✅ (原生支持) | ❌ (不支持) |
| **编译器 C++20** | ❌ (需要 C++26) | ✅ (推荐) |
| **编译器 C++26** | ✅ (推荐) | ✅ |

---

## 八、对 vmess 项目的建议

### 8.1 推荐方案：分阶段实现

**第一阶段 (MVP)**:
- **参考**: io_uring-examples-cpp
- **原因**:
  - ✅ 易于学习 (适合入门)
  - ✅ 代码简洁 (快速原型)
  - ✅ C++20 即可 (编译器要求低)
  - ✅ 单线程足够 (VMess 是 IO 密集型)

**第二阶段 (优化)**:
- **参考**: uring_exec
- **原因**:
  - ✅ 性能更高 (多线程)
  - ✅ 支持取消操作 (超时自动取消)
  - ✅ 支持 Stop token (优雅关闭)
  - ✅ 生产级稳定性

**第三阶段 (扩展)**:
- **参考**: uring_exec + HXLibs
- **原因**:
  - ✅ 支持 Windows (IOCP)
  - ✅ 支持 macOS (kqueue)
  - ✅ 跨平台兼容性

---

### 8.2 具体实现路径

#### 8.2.1 第一阶段：单线程 + 手写 Task

**参考**: io_uring-examples-cpp

**目录结构**:

```
vmess/
├── include/vmess/
│   ├── coroutine/        # 协程核心 (参考 io_uring-examples-cpp)
│   │   └── Task.hpp     # 手写 Task (200 行)
│   ├── io/               # IO 抽象层
│   │   ├── IoUringLoop.hpp     # 单线程事件循环
│   │   └── Awaiter.hpp        # Awaiter 实现
│   ├── crypto/           # 加密模块
│   ├── protocol/         # 协议解析
│   └── utils/            # 工具类
├── src/                  # 实现文件
└── tests/                # 单元测试
```

**核心代码** (参考 io_uring-examples-cpp):

```cpp
// include/vmess/coroutine/Task.hpp (200 行)
struct Task {
    struct promise_type;
    constexpr Task(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ~Task() { if(_handle) _handle.destroy(); }
    auto detach() noexcept { return std::exchange(_handle, {}); }
    Task(Task &&rhs) noexcept: _handle(rhs.detach()) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;
    auto operator co_await() && noexcept;
private:
    std::coroutine_handle<promise_type> _handle;
};

// include/vmess/io/IoUringLoop.hpp
class IoUringLoop {
public:
    IoUringLoop(size_t entries = 256) {
        io_uring_queue_init(entries, &uring, 0);
    }
    ~IoUringLoop() { io_uring_queue_exit(&uring); }

    void run() {
        for(;;) {
            io_uring_submit_and_wait(&uring, 1);
            io_uring_cqe *cqe;
            unsigned head;
            io_uring_for_each_cqe(&uring, head, cqe) {
                auto user_data = static_cast<Async_user_data*>(cqe->user_data);
                user_data->cqe = cqe;
                user_data->h.resume();
            }
            io_uring_cq_advance(&uring, done);
        }
    }

    io_uring& get() { return uring; }

private:
    io_uring uring;
};
```

---

#### 8.2.2 第二阶段：多线程 + std::execution

**参考**: uring_exec

**目录结构**:

```
vmess/
├── include/vmess/
│   ├── coroutine/        # 协程核心 (参考 uring_exec)
│   │   ├── Task.hpp     # exec::task<T>
│   │   └── Sender.hpp  # std::execution Sender
│   ├── io/               # IO 抽象层
│   │   ├── IoUringLoop.hpp     # 多线程事件循环
│   │   ├── Scheduler.hpp       # stdexec::scheduler
│   │   └── AsyncScope.hpp     # exec::async_scope
│   ├── crypto/           # 加密模块
│   ├── protocol/         # 协议解析
│   └── utils/            # 工具类
├── src/                  # 实现文件
└── tests/                # 单元测试
```

**核心代码** (参考 uring_exec):

```cpp
// include/vmess/io/IoUringLoop.hpp
class IoUringLoop {
public:
    IoUringLoop(size_t entries = 512) {
        io_uring_queue_init(entries, &uring, 0);
    }

    // 多线程 run()
    void run(std::stop_token stop_token = {}) {
        for(; !stop_token.stop_requested();) {
            io_uring_submit_and_wait(&uring, 1);
            // ... 处理 CQE ...
        }
    }

    // 获取 scheduler
    auto get_scheduler() {
        return stdexec::schedule(*this);
    }

private:
    io_uring uring;
};
```

---

### 8.3 技术选型对比

| 技术 | 第一阶段 (MVP) | 第二阶段 (优化) | 第三阶段 (扩展) |
|------|-----------------|-----------------|-----------------|
| **工作模型** | 单线程 | 多线程 (线程池) | 多线程 + 跨平台 |
| **IO 模型** | io_uring (liburing) | io_uring + std::execution | io_uring + IOCP + kqueue |
| **协程实现** | 手写 Task (200 行) | exec::task<T> | exec::task<T> |
| **编译器要求** | C++20 | C++26 + stdexec | C++26 + stdexec |
| **学习曲线** | 平缓 | 陡峭 | 陡峭 |
| **性能** | 中 (单线程) | 高 (多线程) | 高 (多线程 + 跨平台) |
| **代码量** | 少 (200 行) | 多 (依赖 stdexec) | 多 (依赖 stdexec) |

---

## 九、核心代码对比

### 9.1 Echo Server 实现对比

#### 9.1.1 uring_exec (Sender 风格)

```cpp
// examples/echo_sender.cpp
using uring_exec::io_uring_exec;

// READ -> WRITE -> [CLOSE]
//      <-
stdexec::sender
auto echo(io_uring_exec::scheduler scheduler, int client_fd) {
    return
        stdexec::just(std::array<char, 512>{})
      | stdexec::let_value([=](auto &buf) {
            return
                uring_exec::async_read(scheduler, client_fd, buf.data(), buf.size())
              | stdexec::then([=, &buf](int read_bytes) {
                    auto copy = std::ranges::copy;
                    auto view = buf | std::views::take(read_bytes);
                    auto to_console = std::ostream_iterator<char>{std::cout};
                    copy(view, to_console);
                    return read_bytes;
                })
              | stdexec::let_value([=, &buf](int read_bytes) {
                    return uring_exec::async_write(scheduler, client_fd, buf.data(), read_bytes);
                })
              | stdexec::let_value([=, &buf](int written_bytes) {
                    return stdexec::just(written_bytes == 0 || buf[0] == '@');
                })
              | exec::repeat_effect_until();
        })
      | stdexec::let_value([=] {
            std::cout << "Closing client..." << std::endl;
            return uring_exec::async_close(scheduler, client_fd) | stdexec::then([](...){});
        });
}

// ACCEPT -> ACCEPT
//        -> ECHO
stdexec::sender
auto server(io_uring_exec::scheduler scheduler, int server_fd, exec::async_scope &scope) {
    return
        uring_exec::async_accept(scheduler, server_fd, nullptr, nullptr, 0)
      | stdexec::let_value([=, &scope](int client_fd) {
            scope.spawn(echo(scheduler, client_fd));
            return stdexec::just(false);
        })
      | exec::repeat_effect_until();
}

int main() {
    auto server_fd = uring_exec::utils::make_server({.port=8848});
    auto server_fd_cleanup = uring_exec::utils::defer([=] { close(server_fd); });

    io_uring_exec uring({.uring_entries=512});
    exec::async_scope scope;

    stdexec::scheduler auto scheduler = uring.get_scheduler();

    scope.spawn(
        stdexec::schedule(scheduler)
      | stdexec::let_value([=, &scope] {
            return server(scheduler, server_fd, scope);
        })
    );

    // Run infinitely.
    uring.run();
}
```

**代码分析**:
- ✅ 类型安全 (编译期检查)
- ✅ 组合性强 (使用 `|` 操作符)
- ✅ 支持取消 (exec::repeat_effect_until)
- ❌ 代码冗长 (boilerplate 多)
- ❌ 学习曲线陡峭

---

#### 9.1.2 io_uring-examples-cpp (协程风格)

```cpp
// examples/echo_coroutine.cpp
Task echo(io_uring *uring, int client_fd) {
    char buf[4096];
    for(;;) {
        // co_await 异步读取
        auto n = co_await async_read(uring, client_fd, buf, std::size(buf)) | nofail("read");

        // 处理数据
        auto printer = std::ostream_iterator<char>{std::cout};
        std::ranges::copy_n(buf, n, printer);

        // co_await 异步写入
        n = co_await async_write(uring, client_fd, buf, n) | nofail("write");

        // 判断是否关闭连接
        bool close_proactive = n > 2 && buf[0] == 'Z' && buf[1] == 'z';
        bool close_reactive = (n == 0);
        if(close_reactive || close_proactive) {
            co_await async_close(uring, client_fd);
            break;
        }
    }
}

Task server(io_uring *uring, Io_context &io_context, int server_fd) {
    for(;;) {
        // co_await 异步 accept
        auto client_fd = co_await async_accept(uring, server_fd) | nofail("accept");
        // Fork a new connection.
        co_spawn(io_context, echo(uring, client_fd));
    }
}

int main() {
    auto server_fd = make_server({.port=8848});
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    // 单线程事件循环
    Io_context io_context{uring};
    co_spawn(io_context, server(&uring, io_context, server_fd));
    io_context.run();
}
```

**代码分析**:
- ✅ 代码简洁 (易于理解)
- ✅ 学习曲线平缓
- ✅ 编译器要求低 (C++20)
- ❌ 类型安全性较弱
- ❌ 不支持取消

---

### 9.2 对比表格

| 特性 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **代码行数** | ~90 行 (echo server) | ~50 行 (echo server) |
| **可读性** | 中 (Sender 抽象) | 高 (协程抽象) |
| **类型安全** | 编译期检查 | 运行时检查 |
| **组合性** | 强 (`\|` 操作符) | 弱 (顺序 co_await) |
| **取消支持** | ✅ (原生支持) | ❌ (不支持) |
| **学习曲线** | 陡峭 | 平缓 |

---

## 十、总结

### 10.1 核心差异总结

| 维度 | uring_exec | io_uring-examples-cpp |
|------|------------|----------------------|
| **C++ 标准** | C++26 + stdexec | C++20 |
| **IO 抽象** | std::execution Sender | Task + Awaiter |
| **工作模型** | 多线程 (线程池) | 单线程 |
| **线程安全** | MT-safe | 仅单线程 |
| **取消支持** | ✅ (原生支持) | ❌ (不支持) |
| **Stop token** | ✅ (原生支持) | ❌ (不支持) |
| **学习曲线** | 陡峭 | 平缓 |
| **代码简洁性** | 中 (boilerplate 多) | 高 (代码清晰) |
| **性能** | 高 (多线程) | 中 (单线程) |
| **适用场景** | 生产环境 | 学习/原型开发 |

---

### 10.2 推荐方案

**对于 vmess 项目**:

1. **第一阶段 (MVP)**:
   - ✅ 参考 **io_uring-examples-cpp**
   - ✅ 手写 Task (200 行)
   - ✅ 单线程事件循环
   - ✅ 快速验证可行性

2. **第二阶段 (优化)**:
   - ✅ 参考 **uring_exec**
   - ✅ 引入 std::execution (stdexec)
   - ✅ 多线程事件循环
   - ✅ 支持取消操作

3. **第三阶段 (扩展)**:
   - ✅ 参考 **uring_exec + HXLibs**
   - ✅ 跨平台支持 (Linux/Windows/macOS)
   - ✅ 生产级稳定性

---

### 10.3 参考资料

**uring_exec**:
- GitHub: https://github.com/Caturra000/uring_exec
- 依赖: [stdexec](https://github.com/NVIDIA/stdexec), [liburing](https://github.com/axboe/liburing)
- 作者博客: https://www.bluepuni.com/archives/porting-liburing-to-stdexec/

**io_uring-examples-cpp**:
- GitHub: https://github.com/Caturra000/io_uring-examples-cpp
- 依赖: [liburing](https://github.com/axboe/liburing)
- 作者博客: (无)

**其他参考**:
- [HXLibs](https://github.com/hengxincong/HXLibs) - 协程 + io_uring 的另一种实现
- [co-uring-webserver](https://github.com/Caturra000/co-uring-webserver) - 协程 + io_uring 的 Web 服务器

---

## 十一、附录

### 11.1 编译命令对比

#### 11.1.1 uring_exec

```bash
# 使用 xmake
xmake build examples        # 编译所有示例
xmake run hello_coro        # 运行指定示例
xmake build benchmarks      # 编译 benchmark
xmake run benchmarks        # 运行 benchmark

# 使用 make
make all                   # 编译所有示例和 benchmark
make hello_coro            # 编译指定示例
make benchmark_script      # 运行 benchmark
```

**依赖**:
- `stdexec` (NVIDIA std::execution 实现)
- `liburing` (>= 2.0)
- `asio` (可选)
- Linux kernel (>= 6.1)

---

#### 11.1.2 io_uring-examples-cpp

```bash
# 使用 make
make all                   # 编译所有示例
make echo_coroutine        # 编译指定示例
./build/echo_coroutine     # 运行示例
```

**依赖**:
- `liburing` (>= 2.0)
- Linux kernel (>= 6.1)

---

### 11.2 代码示例对比

#### 11.2.1 取消操作

**uring_exec** (支持取消):

```cpp
int main() {
    io_uring_exec uring({.uring_entries = 8});
    std::array<std::jthread, 5> threads;
    for(auto &&j : threads) {
        j = std::jthread([&](auto token) { uring.run(token); });
    }
    using namespace std::chrono_literals;
    stdexec::scheduler auto s = uring.get_scheduler();
    stdexec::sender auto _3s = make_sender(s, 3s);
    stdexec::sender auto _9s = make_sender(s, 9s);
    // Waiting for 3 seconds, not 9 seconds.
    stdexec::sender auto any = exec::when_any(std::move(_3s), std::move(_9s));
    stdexec::sync_wait(std::move(any));
}
```

**io_uring-examples-cpp** (不支持取消):

```cpp
// 需要手动实现取消逻辑
Task async_timeout(io_uring *uring, int timeout_ms) {
    co_await async_wait(uring, timeout_ms);
    // 手动取消其他操作
}
```

---

#### 11.2.2 信号处理

**uring_exec** (支持信号处理):

```cpp
int main() {
    io_uring_exec uring(512);
    stdexec::scheduler auto scheduler = uring.get_scheduler();

    // Async_sigwait for specified signals.
    stdexec::sender auto signal_watchdog =
        uring_exec::async_sigwait(scheduler, std::array {SIGINT, SIGUSR1});

    auto sb = uring_exec::signal_blocker();

    std::jthread j([&](auto token) { uring.run(token); });
    // $ kill -USR1 $(pgrep signal_handling)
    stdexec::sync_wait(
        exec::when_any(
            stdexec::when_all(std::move(s1), std::move(s2))
            | stdexec::then([](auto &&...) { std::cout << "timer!" << std::endl; }),
            stdexec::starts_on(scheduler, std::move(signal_watchdog))
            | stdexec::then([](auto &&...) { std::cout << "signal!" << std::endl; })
        )
    );
}
```

**io_uring-examples-cpp** (不支持信号处理):

```cpp
// 需要手动处理信号
void signal_handler(int sig) {
    // 手动处理信号
}
signal(SIGINT, signal_handler);
```

---

### 11.3 性能优化建议

#### 11.3.1 uring_exec

**优化建议**:
1. ✅ 使用线程池 (4-8 个线程)
2. ✅ 使用 `exec::async_scope` 管理协程生命周期
3. ✅ 使用 `stdexec::when_any` 实现超时取消
4. ✅ 使用 `stdexec::schedule` 重调度到指定线程

---

#### 11.3.2 io_uring-examples-cpp

**优化建议**:
1. ✅ 使用 `Io_context` 单线程事件循环
2. ✅ 使用 `co_spawn` 启动新协程
3. ✅ 使用 `async_read` / `async_write` 异步 IO
4. ❌ 不支持多线程 (需要手动实现)

---

## 十二、参考资料

1. **uring_exec GitHub**: https://github.com/Caturra000/uring_exec
2. **io_uring-examples-cpp GitHub**: https://github.com/Caturra000/io_uring-examples-cpp
3. **stdexec GitHub**: https://github.com/NVIDIA/stdexec
4. **liburing GitHub**: https://github.com/axboe/liburing
5. **C++26 std::execution**: https://en.cppreference.com/w/cpp/execution
6. **io_uring 官方文档**: https://man7.org/linux/man-pages/man7/io_uring.7.html
7. **作者博客**: https://www.bluepuni.com/archives/porting-liburing-to-stdexec/

---

**文档版本**: v1.0  
**最后更新**: 2026-04-29  
**作者**: AI Assistant  
**项目**: vmess 代理软件  
**状态**: 草稿
