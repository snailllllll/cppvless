> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：早期开发规划（VMess 时代），实施路径已改变
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# vmess 项目实用开发计划

**日期**: 2026-04-29  
**作者**: AI Assistant  
**目标**: 基于 io_uring + C++20 协程实现 vmess 代理（暂不引入 std::execution）  
**参考项目**: [io_uring-examples-cpp](https://github.com/Caturra000/io_uring-examples-cpp)

---

## 一、技术选型

### 1.1 核心依赖

| 技术 | 版本 | 作用 | 参考来源 |
|------|------|------|-----------|
| **C++ 标准** | C++20 | 协程支持 | io_uring-examples-cpp |
| **io_uring** | liburing 2.0+ | 异步 IO | io_uring-examples-cpp |
| **协程实现** | 手写 Task | 200 行代码 | io_uring-examples-cpp/include/coroutine.h |
| **事件循环** | 单线程 Io_context | 简化设计 | io_uring-examples-cpp |
| **Linux 内核** | >= 5.15 | io_uring 支持 | io_uring-examples-cpp |

### 1.2 为什么选择这个方案？

✅ **编译器支持好**: C++20 已广泛支持（GCC 11+, Clang 14+）  
✅ **学习曲线平缓**: 手写 Task 仅 200 行，易于理解  
✅ **代码简洁**: 不需要 std::execution 的 boilerplate  
✅ **快速验证**: 可以快速实现 MVP（最小可行产品）  
✅ **生产可行**: io_uring 性能已接近最优  

❌ **暂不包括**: std::execution（C++26，编译器支持不完善）  
❌ **暂不包括**: 多线程（后期可扩展）  

---

## 二、开发阶段

### 阶段 1: 协程基础框架 (1-2 天)

**目标**: 实现 C++20 协程 + io_uring 的基础框架

#### 2.1.1 实现 Task 和 Awaiter

**参考**: `io_uring-examples-cpp/include/coroutine.h` (200 行)

**创建文件**:

```
vmess/
├── include/vmess/coroutine/
│   ├── Task.hpp          # 协程返回类型（参考 coroutine.h）
│   ├── Awaiter.hpp      # io_uring Awaiter 封装
│   └── IoContext.hpp    # 单线程事件循环
├── src/coroutine/
│   └── (实现文件，如果需要)
└── tests/
    └── test_coroutine.cpp  # 单元测试
```

**核心代码** (`include/vmess/coroutine/Task.hpp`):

```cpp
// 基于 io_uring-examples-cpp/include/coroutine.h
#pragma once
#include <coroutine>
#include <queue>
#include <utility>

namespace vmess {

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

struct Task::promise_type {
    constexpr auto initial_suspend() const noexcept { return std::suspend_always{}; }
    constexpr void return_void() const noexcept {}
    void unhandled_exception() { std::terminate(); }
    Task get_return_object() noexcept {
        auto h = std::coroutine_handle<promise_type>::from_promise(*this);
        return {h};
    }
    struct Final_suspend {
        constexpr bool await_ready() const noexcept { return false; }
        auto await_suspend(auto callee) const noexcept {
            auto caller = callee.promise()._caller;
            callee.destroy();
            return caller;
        }
        constexpr auto await_resume() const noexcept {}
    };
    constexpr auto final_suspend() const noexcept { return Final_suspend{}; }
    void push(std::coroutine_handle<> caller) noexcept { _caller = caller; }

    std::coroutine_handle<> _caller {std::noop_coroutine()};
};

inline auto Task::operator co_await() && noexcept {
    struct awaiter {
        bool await_ready() const noexcept { return !_handle || _handle.done(); }
        auto await_suspend(std::coroutine_handle<> caller) noexcept {
            _handle.promise().push(caller);
            return _handle;
        }
        constexpr auto await_resume() const noexcept {}
        std::coroutine_handle<Task::promise_type> _handle;
    };
    return awaiter{detach()};
}

} // namespace vmess
```

---

#### 2.1.2 实现 io_uring Awaiter

**创建文件**: `include/vmess/coroutine/Awaiter.hpp`

**核心代码**:

```cpp
#pragma once
#include <liburing.h>
#include <coroutine>
#include <utility>

namespace vmess {

struct Async_user_data {
    io_uring *uring;
    io_uring_sqe *sqe {};
    io_uring_cqe *cqe {};
    std::coroutine_handle<> h {std::noop_coroutine()};

    Async_user_data(io_uring *uring) noexcept: uring(uring) {}
};

struct Async_operation {
    constexpr bool await_ready() const noexcept {
        if(user_data.sqe && !user_data.cqe) return false;
        return true;
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        user_data.h = h;
    }
    auto await_resume() const noexcept {
        if(!user_data.sqe) return -ENOMEM;
        return user_data.cqe->res;
    }
    template <typename... Args>
    Async_operation(io_uring *uring, auto uring_prep_fn, Args&&... args) noexcept
        : user_data(uring) {
        if((user_data.sqe = io_uring_get_sqe(uring))) {
            uring_prep_fn(user_data.sqe, std::forward<Args>(args)...);
            io_uring_sqe_set_data(user_data.sqe, &user_data);
        }
    }
    Async_user_data user_data;
};

// 便捷函数
inline auto async_accept(io_uring *uring, int server_fd, int flags = 0) noexcept {
    return Async_operation(uring, io_uring_prep_accept, server_fd, nullptr, nullptr, flags);
}

inline auto async_read(io_uring *uring, int fd, void *buf, size_t n, uint64_t offset = 0) noexcept {
    return Async_operation(uring, io_uring_prep_read, fd, buf, n, offset);
}

inline auto async_write(io_uring *uring, int fd, const void *buf, size_t n, uint64_t offset = 0) noexcept {
    return Async_operation(uring, io_uring_prep_write, fd, buf, n, offset);
}

inline auto async_close(io_uring *uring, int fd) noexcept {
    return Async_operation(uring, io_uring_prep_close, fd);
}

} // namespace vmess
```

---

#### 2.1.3 实现 IoContext (事件循环)

**创建文件**: `include/vmess/coroutine/IoContext.hpp`

**核心代码**:

```cpp
#pragma once
#include <liburing.h>
#include <queue>
#include <coroutine>
#include <functional>

namespace vmess {

class IoContext {
public:
    explicit IoContext(io_uring &uring): uring(uring) {}
    
    void run() {
        for(;;) {
            io_uring_submit_and_wait(&uring, 1);
            
            io_uring_cqe *cqe;
            unsigned head;
            size_t done = 0;
            
            io_uring_for_each_cqe(&uring, head, cqe) {
                done++;
                if(cqe->res == -ECANCELED) continue;
                auto user_data = static_cast<Async_user_data*>(cqe->user_data);
                user_data->cqe = cqe;
                user_data->h.resume();
            }
            
            if(done) io_uring_cq_advance(&uring, done);
            else std::this_thread::yield();
        }
    }
    
    void stop() noexcept { _stop = true; }
    
    friend void co_spawn(IoContext &ctx, Task &&task) {
        ctx._operations.emplace(task.detach());
    }

private:
    io_uring &uring;
    std::queue<std::coroutine_handle<>> _operations;
    bool _stop {false};
};

} // namespace vmess
```

---

### 阶段 2: Echo Server 验证 (1 天)

**目标**: 实现简单的 echo server，验证协程 + io_uring 框架

#### 2.2.1 实现 Echo Server

**创建文件**: `examples/echo_server.cpp`

**核心代码**:

```cpp
#include <vmess/coroutine/Task.hpp>
#include <vmess/coroutine/Awaiter.hpp>
#include <vmess/coroutine/IoContext.hpp>
#include <netinet/in.h>
#include <iostream>
#include <array>

using namespace vmess;

Task echo(io_uring *uring, int client_fd) {
    std::array<char, 4096> buf;
    for(;;) {
        auto n = co_await async_read(uring, client_fd, buf.data(), buf.size());
        if(n <= 0) break;
        
        std::cout.write(buf.data(), n);
        
        co_await async_write(uring, client_fd, buf.data(), n);
    }
    co_await async_close(uring, client_fd);
}

Task server(io_uring *uring, IoContext &ctx, int server_fd) {
    for(;;) {
        auto client_fd = co_await async_accept(uring, server_fd);
        if(client_fd < 0) break;
        co_spawn(ctx, echo(uring, client_fd));
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8848);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, SOMAXCONN);
    
    io_uring uring;
    io_uring_queue_init(256, &uring, 0);
    
    IoContext ctx(uring);
    co_spawn(ctx, server(&uring, ctx, server_fd));
    ctx.run();
    
    io_uring_queue_exit(&uring);
    close(server_fd);
}
```

---

### 阶段 3: VMess 协议实现 (3-5 天)

**目标**: 实现 VMess 协议的核心逻辑

#### 2.3.1 VMess 协议解析

**创建文件**:

```
vmess/
├── include/vmess/protocol/
│   ├── VmessProtocol.hpp    # VMess 协议接口
│   ├── VmessHeader.hpp      # VMess 头解析
│   └── VmessCrypto.hpp     # VMess 加密/解密
└── src/protocol/
    ├── VmessProtocol.cpp
    ├── VmessHeader.cpp
    └── VmessCrypto.cpp
```

**核心接口**:

```cpp
// include/vmess/protocol/VmessProtocol.hpp
#pragma once
#include <vmess/coroutine/Task.hpp>
#include <string>

namespace vmess {

class VmessProtocol {
public:
    virtual ~VmessProtocol() = default;
    
    // 握手
    virtual Task<bool> handshake(int fd) = 0;
    
    // 读取请求
    virtual Task<std::string> read_request(int fd) = 0;
    
    // 发送响应
    virtual Task<bool> send_response(int fd, const std::string &data) = 0;
};

} // namespace vmess
```

---

#### 2.3.2 VMess 加密

**创建文件**: `include/vmess/crypto/VmessCrypto.hpp`

**支持算法**:
- AES-128-GCM
- AES-256-GCM
- ChaCha20-Poly1305

---

### 阶段 4: SOCKS5 代理实现 (2-3 天)

**目标**: 实现 SOCKS5 代理协议

**创建文件**:

```
vmess/
├── include/vmess/protocol/
│   └── Socks5Protocol.hpp   # SOCKS5 协议实现
└── src/protocol/
    └── Socks5Protocol.cpp
```

---

### 阶段 5: 完整代理服务器 (2-3 天)

**目标**: 整合所有组件，实现完整的 VMess 代理服务器

**创建文件**: `src/main.cpp`

**核心代码**:

```cpp
Task proxy_session(io_uring *uring, int client_fd) {
    // 1. SOCKS5 握手
    Socks5Protocol socks5;
    co_await socks5.handshake(client_fd);
    
    // 2. VMess 加密通信
    VmessProtocol vmess;
    auto request = co_await vmess.read_request(client_fd);
    
    // 3. 连接到目标服务器
    int target_fd = co_await async_connect(uring, request);
    
    // 4. 数据转发
    co_await forward_data(uring, client_fd, target_fd);
}

int main() {
    io_uring uring;
    io_uring_queue_init(256, &uring, 0);
    
    IoContext ctx(uring);
    
    int server_fd = create_server_socket(8848);
    
    co_spawn(ctx, accept_loop(&uring, ctx, server_fd));
    
    ctx.run();
    
    io_uring_queue_exit(&uring);
}
```

---

## 三、项目结构

### 3.1 目录结构

```
vmess/
├── include/vmess/
│   ├── coroutine/        # 协程核心
│   │   ├── Task.hpp
│   │   ├── Awaiter.hpp
│   │   └── IoContext.hpp
│   ├── protocol/         # 协议实现
│   │   ├── VmessProtocol.hpp
│   │   ├── Socks5Protocol.hpp
│   │   └── ...
│   ├── crypto/           # 加密模块
│   │   ├── VmessCrypto.hpp
│   │   └── ...
│   └── utils/            # 工具类
│       ├── Logger.hpp
│       └── Config.hpp
├── src/                  # 实现文件
│   ├── main.cpp
│   ├── protocol/
│   ├── crypto/
│   └── utils/
├── tests/                # 单元测试
│   ├── test_coroutine.cpp
│   ├── test_vmess_protocol.cpp
│   └── ...
├── examples/             # 示例代码
│   ├── echo_server.cpp
│   └── ...
├── CMakeLists.txt        # CMake 配置
└── README.md
```

---

## 四、编译配置

### 4.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(vmess CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# 包含目录
include_directories(include)

# 可执行文件
add_executable(vmess src/main.cpp)
target_link_libraries(vmess ${LIBURING_LIBRARIES})

# 示例
add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server ${LIBURING_LIBRARIES})

# 测试
enable_testing()
add_test(NAME test_coroutine COMMAND test_coroutine)
```

---

## 五、开发时间线

| 阶段 | 任务 | 预计时间 | 状态 |
|------|------|----------|------|
| 1 | 协程基础框架 | 1-2 天 | ⏳ 待开始 |
| 2 | Echo Server 验证 | 1 天 | ⏳ 待开始 |
| 3 | VMess 协议实现 | 3-5 天 | ⏳ 待开始 |
| 4 | SOCKS5 代理实现 | 2-3 天 | ⏳ 待开始 |
| 5 | 完整代理服务器 | 2-3 天 | ⏳ 待开始 |

**总计**: 9-14 天

---

## 六、后续扩展

### 6.1 性能优化

- ✅ 零拷贝 (`splice`)
- ✅ 多线程事件循环
- ✅ io_uring 高级特性 (Multishot, SQPOLL)

### 6.2 功能扩展

- ✅ 行为排空 (Behavior draining)
- ✅ 防重放攻击
- ✅ 流量混淆

### 6.3 跨平台支持

- ✅ Windows (IOCP)
- ✅ macOS (kqueue)
- ✅ BSD (kqueue)

---

## 七、参考资源

1. **io_uring-examples-cpp**: https://github.com/Caturra000/io_uring-examples-cpp
2. **liburing 文档**: https://man7.org/linux/man-pages/man7/io_uring.7.html
3. **C++20 协程教程**: https://en.cppreference.com/w/cpp/language/coroutines
4. **VMess 协议文档**: https://www.v2ray.com/developers/protocols/vmess.html

---

**文档版本**: v1.0  
**最后更新**: 2026-04-29  
**作者**: AI Assistant  
**状态**: 待审核
