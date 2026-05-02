# HXLibs 协程 + io_uring 实现分析

> 本文档分析 HXLibs 项目中协程与 io_uring 的结合实现，特别是 SOCKS5 代理部分的参考价值。

## 一、项目概述

HXLibs 是一个现代 C++ 网络库，其核心特性包括：
- 基于 `io_uring` (Linux) / `IOCP` (Windows) 的协程网络库
- 支持 Http(s) / WebSocket 客户端与服务端
- 支持 SOCKS5 / Http 代理
- 本地测试性能：500MB 文件传输吞吐量高达 20 GB/s，wrk 压测 Http 请求并发高达 200w+ Requests/sec

### 核心依赖
- **Linux**: `liburing` (io_uring 的封装)
- **OpenSSL 3.3.1+**: 用于 https 的证书/握手

---

## 二、协程 + io_uring 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        应用层                                │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │
│  │  HttpClient │  │ HttpServer │  │ Socks5Proxy │  │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  │
│         │                │                │             │
│  ┌─────▼────────────────▼────────────────▼─────┐  │
│  │              HttpIO / HttpsIO                      │  │
│  └─────┬────────────────────────────────────┬────┘  │
│        │                                    │           │
│  ┌─────▼────────────┐              │           │
│  │   AioTask        │              │           │
│  │  (异步IO任务)    │              │           │
│  └─────┬───────────┘              │           │
│        │                            │           │
│  ┌─────▼────────────────────────▼─────┐      │
│  │          EventLoop (事件循环)           │      │
│  │  ┌──────────┐  ┌──────────┐  │      │
│  │  │ IoUring  │  │ TimerLoop │  │      │
│  │  │(io_uring) │  │(定时器)  │  │      │
│  │  └──────────┘  └──────────┘  │      │
│  └─────┬────────────────────────────┬────┘      │
│        │                            │             │
│  ┌─────▼────────────┐  ┌─────▼─────┐       │
│  │   Linux 内核   │  │ 用户态   │       │
│  │  io_uring     │  │ EventLoop │       │
│  │  (完成队列)   │  │ (调度协程)│       │
│  └───────────────┘  └───────────┘       │
└─────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 Task - 协程任务包装器

```cpp
// 文件: include/HXLibs/coroutine/task/Task.hpp
template <
    typename T = void,
    typename P = Promise<T>,
    typename Awaiter = ExitAwaiter<T, P>
>
struct [[nodiscard]] Task {
    using promise_type = P;
    
    // 关键：重载 co_await 运算符
    constexpr Awaiter operator co_await() noexcept {
        return Awaiter{_handle};
    }
    
    std::coroutine_handle<promise_type> _handle;
};
```

**设计要点**：
1. `Task<T>` 是协程的返回类型
2. 通过 `Promise<T>` 控制协程的行为（懒启动、结果返回等）
3. `ExitAwaiter<T, P>` 决定协程退出时的行为（恢复到调用者）

#### 2.2.2 AioTask - 异步 IO 任务

```cpp
// 文件: include/HXLibs/coroutine/task/AioTask.hpp
struct AioTask {
    // 构造函数：将 this 绑定到 sqe->user_data
    AioTask(::io_uring_sqe* sqe) noexcept
        : _sqe{sqe}
    {
        ::io_uring_sqe_set_data(_sqe, this);
    }
    
    // 关键：重载 co_await 运算符
    AioAwaiter operator co_await() noexcept {
        return {this};
    }
    
    // AioAwaiter - 协程挂起点
    struct AioAwaiter {
        constexpr bool await_ready() const noexcept { return false; }
        
        // 协程挂起时调用：保存上一个协程句柄
        constexpr void await_suspend(std::coroutine_handle<> coroutine) const noexcept {
            _task->_previous = coroutine;
            _task->_res = -ENOSYS;
        }
        
        // 协程恢复时调用：返回 IO 操作结果
        constexpr int await_resume() const noexcept {
            return _task->_res;
        }
        
        AioTask* _task;
    };
    
    // 准备各种异步 IO 操作
    AioTask&& prepRead(int fd, std::span<char> buf, uint64_t offset) &&;
    AioTask&& prepWrite(int fd, std::span<char const> buf, uint64_t offset) &&;
    AioTask&& prepRecv(int fd, std::span<char> buf, int flags) &&;
    AioTask&& prepSend(int fd, std::span<char const> buf, int flags) &&;
    AioTask&& prepAccept(int fd, sockaddr* addr, socklen_t* addrlen, int flags) &&;
    AioTask&& prepConnect(int fd, const sockaddr* addr, socklen_t addrlen) &&;
    AioTask&& prepClose(int fd) &&;
    
private:
    friend internal::IoUring;
    
    union {
        int _res;              // IO 操作结果
        ::io_uring_sqe* _sqe;  // 提交队列项
    };
    std::coroutine_handle<> _previous;  // 上一个协程（调用者）
};
```

**设计要点**：
1. **协程与 io_uring 的桥梁**：`AioTask` 将协程的挂起/恢复与 io_uring 的异步操作绑定
2. **user_data 传递**：通过 `io_uring_sqe_set_data(sqe, this)` 将 `AioTask*` 存入 SQE
3. **协程挂起**：`await_suspend` 保存当前协程句柄到 `_previous`
4. **结果回传**：EventLoop 处理完成队列时，通过 `user_data` 找到 `AioTask`，设置 `_res`，然后恢复协程

#### 2.2.3 EventLoop - 事件循环

```cpp
// 文件: include/HXLibs/coroutine/loop/EventLoop.hpp
struct EventLoop {
    /**
     * @brief 启动协程
     */
    template <CoroutineObject T>
    void start(T& mainTask) {
        static_cast<std::coroutine_handle<>>(mainTask).resume();
    }
    
    /**
     * @brief 事件循环主函数
     * 1. 检查定时器是否有待处理任务
     * 2. 如果有 IO 任务，调用 io_uring 等待完成
     * 3. 处理完成的 IO，恢复对应的协程
     */
    void run() {
        for (;;) {
            auto timeout = _timerLoop.run();
            if (_eventDrive.isRun()) [[likely]] {
                _eventDrive.run(timeout);
            } else if (timeout) {
                std::this_thread::sleep_for(*timeout);
            } else if (!_threadLoop.isRun()) [[unlikely]] {
                break;
            }
        }
    }
    
    /**
     * @brief 创建异步 IO 协程任务
     */
    decltype(auto) makeAioTask() {
        return _eventDrive.makeAioTask();
    }
    
private:
    internal::EventDrive _eventDrive;  // IoUring 或 Iocp
    TimerLoop _timerLoop;          // 定时器循环
    ThreadLoop _threadLoop;          // 线程池任务
};
```

#### 2.2.4 IoUring - io_uring 封装

```cpp
// 文件: include/HXLibs/coroutine/loop/EventLoop.hpp (internal 命名空间)
struct IoUring {
    explicit IoUring(unsigned int size = 1024U) {
        ::io_uring_queue_init(size, &_ring, 0);
    }
    
    /**
     * @brief 创建一个 AioTask，绑定一个 SQE
     */
    AioTask makeAioTask() {
        return AioTask{getSqe()};
    }
    
    /**
     * @brief 事件循环：等待 IO 完成，恢复协程
     * 
     * 核心流程：
     * 1. 调用 io_uring_submit_and_wait_timeout() 等待完成事件
     * 2. 遍历完成队列 (CQE)
     * 3. 通过 CQE->user_data 找到对应的 AioTask
     * 4. 设置 AioTask 的结果 (_res)
     * 5. 将 AioTask 的上一个协程 (_previous) 加入待恢复列表
     * 6. 批量恢复所有待恢复的协程
     */
    void run(std::optional<std::chrono::system_clock::duration> timeout) {
        ::io_uring_cqe* cqe = nullptr;
        
        // 1. 提交并等待完成
        int res = ::io_uring_submit_and_wait_timeout(
            &_ring, &cqe, 1, timespecPtr, nullptr);
        
        // 2. 遍历完成队列
        unsigned head, numGot = 0;
        io_uring_for_each_cqe(&_ring, head, cqe) {
            ++numGot;
            if (cqe->res == -ECANCELED) {
                continue;  // 操作已取消
            }
            
            // 3. 通过 user_data 找到 AioTask
            auto* task = reinterpret_cast<AioTask*>(cqe->user_data);
            if (!task) {
                continue;  // 仅 prepNop
            }
            
            // 4. 设置结果
            task->_res = cqe->res;
            
            // 5. 保存待恢复的协程
            tasks.push_back(task->_previous);
        }
        
        // 6. 批量恢复协程
        ::io_uring_cq_advance(&_ring, numGot);
        _numSqesPending -= numGot;
        for (const auto& it : tasks) {
            it.resume();  // 恢复协程！
        }
        tasks.clear();
    }
    
private:
    /**
     * @brief 获取一个 SQE (提交队列项)
     * 
     * 如果 SQE 队列已满，会阻塞等待内核处理
     */
    ::io_uring_sqe* getSqe() {
        ::io_uring_sqe* sqe = ::io_uring_get_sqe(&_ring);
        while (!sqe) {
            // 队列满了，提交并等待一个空位
            ::io_uring_submit_and_wait(&_ring, 1);
            sqe = ::io_uring_get_sqe(&_ring);
        }
        ++_numSqesPending;
        return sqe;
    }
    
    ::io_uring _ring;
    std::size_t _numSqesPending;
    std::vector<std::coroutine_handle<>> tasks;  // 待恢复的协程
};
```

---

## 三、协程 + io_uring 工作流程

### 3.1 完整流程示意图

```
应用代码                      AioTask                   IoUring (内核)
    │                          │                          │
    │  co_await task          │                          │
    │─ ─ ─ ─ ─ ─ ─ ─ ─ ─▶│                          │
    │                          │  prepSend/Send)          │
    │                          │─ ─ ─ ─ ─ ─ ─ ─ ─ ─▶│ 1. io_uring_get_sqe()
    │                          │                          │ 2. io_uring_prep_send()
    │                          │                          │ 3. io_uring_submit()
    │                          │  await_suspend()          │
    │                          │─ ─ ─ ─ ─ ─ ─ ─ ─ ─▶│
    │                          │ 保存 _previous           │
    │                          │ 协程挂起                │
    │                          │                          │ 4. 内核异步处理发送
    │                          │                          │
    │                          │                          │ 5. 内核完成，写入 CQE
    │                          │                          │
    │                          │  run() 被调用           │
    │                          │◀ ─ ─ ─ ─ ─ ─ ─ ─ ─ │
    │                          │ 遍历 CQE               │
    │                          │ 找到 AioTask           │
    │                          │ 设置 _res = cqe->res  │
    │                          │                          │
    │  await_resume()        │                          │
    │◀ ─ ─ ─ ─ ─ ─ ─ ─ ─ │ 恢复 _previous 协程     │
    │  返回结果               │─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─▶│
    │ 继续执行                │                          │
```

### 3.2 关键代码解析

#### 发送数据的完整流程

```cpp
// 应用层代码 (如 Socks5Proxy::handshake)
coroutine::Task<> handshake(bool authentication) {
    char handshakeRequest[3] = {0x05, 0x01, ...};
    
    // 1. 调用 HttpIO::fullySend()
    co_await _io.fullySend(handshakeRequest);
}

// HttpIO::fullySend() - 保证完全发送
coroutine::Task<> fullySend(std::span<char const> buf) {
    while (!buf.empty()) {
        // 2. 创建 AioTask，提交到 io_uring
        auto sent = co_await _eventLoop.makeAioTask()
            .prepSend(_fd, buf, 0);
        
        // 6. 协程恢复，获取发送结果
        buf = buf.subspan(sent);
    }
}

// AioTask::prepSend() - 准备发送操作
AioTask&& prepSend(int fd, std::span<char const> buf, int flags) && {
    // 3. 设置 SQE
    ::io_uring_prep_send(_sqe, fd, buf.data(), buf.size(), flags);
    return std::move(*this);
}

// AioAwaiter::await_suspend() - 协程挂起
constexpr void await_suspend(std::coroutine_handle<> coroutine) const noexcept {
    // 4. 保存当前协程（调用者）
    _task->_previous = coroutine;
    _task->_res = -ENOSYS;
    // 协程挂起，返回到 EventLoop::run()
}

// EventLoop::run() - 事件循环
void IoUring::run(...) {
    // 5. 等待 io_uring 完成
    ::io_uring_submit_and_wait_timeout(&_ring, &cqe, ...);
    
    // 遍历完成队列
    io_uring_for_each_cqe(&_ring, head, cqe) {
        auto* task = reinterpret_cast<AioTask*>(cqe->user_data);
        task->_res = cqe->res;  // 设置结果
        
        // 恢复协程
        tasks.push_back(task->_previous);
    }
    
    // 6. 恢复所有完成的协程
    for (auto& t : tasks) {
        t.resume();  // 协程从 await_resume() 继续执行
    }
}
```

---

## 四、SOCKS5 代理实现分析

### 4.1 SOCKS5 协议流程

```
客户端 (Proxy)               SOCKS5 代理服务器            目标服务器
    │                          │                          │
    │──── 1. 握手请求 ──────────▶│                          │
    │  VER=5, NMETHODS=1     │                          │
    │  METHODS=0x00          │                          │
    │                          │                          │
    │◀─── 2. 握手响应 ─────────│                          │
    │  VER=5, METHOD=0x00   │                          │
    │                          │                          │
    │──── 3. 连接请求 ──────────▶│──── 建立 TCP 连接 ─────▶│
    │  VER=5, CMD=0x01       │                          │
    │  ATYP=0x03, DST.ADDR  │                          │
    │  DST.PORT              │                          │
    │                          │                          │
    │◀─── 4. 连接响应 ─────────│◀── 连接结果 ───────────│
    │  VER=5, REP=0x00      │                          │
    │  BND.ADDR, BND.PORT   │                          │
    │                          │                          │
    │════ 5. 数据转发 ══════│════ 数据转发 ═════════│
    │                          │                          │
```

### 4.2 HXLibs 的 SOCKS5 实现

```cpp
// 文件: include/HXLibs/net/protocol/proxy/Socks5Proxy.hpp
class Socks5Proxy : public Proxy<Socks5Proxy> {
public:
    using ProxyBase = Proxy<Socks5Proxy>;
    using ProxyBase::ProxyBase;  // 继承构造函数，接收 HttpIO& _io

    /**
     * @brief 主流程：连接代理服务器，进行握手，建立到目标服务器的连接
     * 
     * 使用协程 + io_uring 实现异步操作
     */
    coroutine::Task<> connect(std::string_view url, std::string_view targetUrl) {
        // 1. 解析代理 URL (可能有用户名密码)
        auto user = UrlParse::extractUser(url);
        
        // 2. 握手协商
        co_await handshake(user.has_value());
        
        // 3. 如果需要认证，进行子协商
        if (user) {
            co_await subNegotiation(user->account, user->password);
        }
        
        // 4. 发送连接请求到目标服务器
        co_await socks5ConnectRequest(targetUrl);
    }

private:
    /**
     * @brief 子协商 (用户名/密码认证)
     * 
     * 协议格式：
     *   +----+------+----------+------+----------+
     *   |VER | ULEN |   UNAME  | PLEN |   PASSWD  |
     *   +----+------+----------+------+----------+
     *   | 1  |  1   | Variable |  1   | Variable |
     *   +----+------+----------+------+----------+
     */
    coroutine::Task<> subNegotiation(
        std::string_view username, 
        std::string_view password
    ) {
        std::string authRequest;
        authRequest += static_cast<char>(0x01);        // 子协商版本
        authRequest += static_cast<char>(username.size());
        authRequest += username;
        authRequest += static_cast<char>(password.size());
        authRequest += password;
        
        // 异步发送 (io_uring)
        co_await _io.fullySend(authRequest);
        
        // 异步接收响应
        char authResponse[2];
        co_await _io.fullyRecv(authResponse);
        
        // 检查响应
        if (authResponse[1] != 0x00) {
            throw std::invalid_argument("sub-negotiation failed");
        }
    }

    /**
     * @brief 握手协商
     * 
     * 协议格式：
     *   +----+----------+----------+
     *   |VER | NMETHODS |  METHODS  |
     *   +----+----------+----------+
     *   | 1  |    1     | 1 to 255 |
     *   +----+----------+----------+
     */
    coroutine::Task<> handshake(bool authentication) {
        char handshakeRequest[3] = {
            0x05,  // SOCKS 版本 5
            0x01,  // 支持的认证方法数量
            static_cast<char>(authentication ? 0x02 : 0x00)
        };
        
        // 异步发送
        co_await _io.fullySend(handshakeRequest);
        
        // 异步接收响应
        char handshakeResponse[2];
        co_await _io.fullyRecv(handshakeResponse);
        
        // 检查响应
        if (handshakeResponse[0] != 0x05 ||
            handshakeResponse[1] != (authentication ? 0x02 : 0x00)) {
            throw std::invalid_argument("handshake failed");
        }
    }

    /**
     * @brief 发送代理连接请求
     * 
     * 协议格式：
     *   +----+-----+-------+------+----------+----------+
     *   |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
     *   +----+-----+-------+------+----------+----------+
     *   | 1  |  1  |   1   |  1   | Variable |    2     |
     *   +----+-----+-------+------+----------+----------+
     */
    coroutine::Task<> socks5ConnectRequest(std::string_view targetUrl) {
        std::string connectRequest;
        connectRequest += static_cast<char>(0x05);  // Version 5
        connectRequest += static_cast<char>(0x01);  // CMD: CONNECT
        connectRequest += static_cast<char>(0x00);  // RSV: 保留字段
        connectRequest += static_cast<char>(0x03);  // ATYP: 域名
        
        // 解析目标 URL
        UrlInfoExtractor parser(targetUrl);
        connectRequest += static_cast<char>(parser.getHostname().size());
        connectRequest += parser.getHostname();
        
        // 端口 (网络字节序)
        connectRequest.resize(connectRequest.size() + 2);
        *(reinterpret_cast<uint16_t*>(&connectRequest[connectRequest.size() - 2])) = 
            ::htons(UrlParse::getProtocolPort(parser.getService()));
        
        // 异步发送请求
        co_await _io.fullySend(connectRequest);
        
        // 异步接收响应
        co_await _io.fullyRecv(connectRequest);
        
        // 检查响应
        if (connectRequest[1] != 0x00) {
            throw std::invalid_argument("Connect Request failed");
        }
    }
};
```

### 4.3 使用示例

```cpp
// 文件: examples/Client/01_socks5_proxy_cli.cpp
#include <HXLibs/net/client/HttpClient.hpp>
#include <HXLibs/net/protocol/proxy/Socks5Proxy.hpp>

int main() {
    // 创建 HTTP 客户端，配置 SOCKS5 代理
    HttpClient cli{HttpClientOptions{
        ProxyType<Socks5Proxy>{"socks5://127.0.0.1:2334"}
    }};
    
    // 通过代理访问目标服务器
    auto res = cli.get("http://httpbin.org/get").get().move();
    
    log::hxLog.info("状态码:", res.status);
    log::hxLog.info("body:", res.body);
    return 0;
}
```

---

## 五、对 vmess 代理服务的参考价值

### 5.1 可借鉴的设计模式

#### 5.1.1 协程 + io_uring 的封装模式

**HXLibs 的做法**：
1. **Task<T>**：协程返回类型，封装 `coroutine_handle`
2. **AioTask**：桥接协程与 io_uring，每个异步操作对应一个 AioTask
3. **EventLoop**：事件循环，处理完成队列，恢复协程

**对 vmess 的参考**：
- 可以使用类似的协程封装，让代理协议的实现更直观（顺序写，而不是回调地狱）
- `co_await` 让异步代码看起来像同步代码

```cpp
// 类似 HXLibs 的风格，vmess 可以这样写：
Task<> handleConnection(int clientFd) {
    // 读取 vmess 协议头
    VmessHeader header;
    co_await fullyRecv(clientFd, &header, sizeof(header));
    
    // 解密
    auto decrypted = co_await decrypt(header.encrypted);
    
    // 连接目标服务器
    int targetFd = co_await asyncConnect(decrypted.host, decrypted.port);
    
    // 数据转发
    co_await relayData(clientFd, targetFd);
}
```

#### 5.1.2 完全发送/接收的封装

**HXLibs 的做法**：
```cpp
// 保证完全发送，处理部分发送的情况
coroutine::Task<> fullySend(std::span<char const> buf) {
    while (!buf.empty()) {
        auto sent = co_await _eventLoop.makeAioTask()
            .prepSend(_fd, buf, 0);
        buf = buf.subspan(sent);
    }
}
```

**对 vmess 的参考**：
- vmess 协议需要处理完整的消息（可能很大）
- 使用类似的封装，可以避免手动处理部分读/写

#### 5.1.3 代理协议的实现模式

**HXLibs 的 SOCKS5 实现**：
1. 将协议状态机分解为多个协程函数
2. 每个函数负责一个协议阶段（握手、认证、请求、转发）
3. 使用 `co_await` 等待 IO 完成

**对 vmess 的参考**：
```cpp
class VmessProxy {
public:
    Task<> handleClient(int clientFd) {
        // 1. 读取协议头
        co_await readHeader();
        
        // 2. 解密并解析目标地址
        auto target = co_await parseTarget();
        
        // 3. 连接目标服务器
        co_await connectTarget(target);
        
        // 4. 数据转发
        co_await relay(clientFd, targetFd);
    }
    
private:
    Task<> readHeader();
    Task<TargetInfo> parseTarget();
    Task<> connectTarget(const TargetInfo& target);
    Task<> relay(int fd1, int fd2);
    
    HttpIO _io;  // 封装了 io_uring 异步 IO
};
```

### 5.2 性能优化要点

#### 5.2.1 io_uring 的批量提交

**HXLibs 的做法**：
- 在 `IoUring::run()` 中，批量处理完成队列
- 一次性恢复多个协程

**对 vmess 的参考**：
- 可以在代理服务器中，批量提交多个 IO 操作
- 比如同时处理多个客户端的请求

#### 5.2.2 零拷贝优化

**HXLibs 的做法**：
- 使用 `std::span` 避免拷贝
- `HttpIO` 直接操作缓冲区

**对 vmess 的参考**：
- vmess 解密后可以直接转发，避免中间拷贝
- 使用 `splice()` 或 `sendfile()` 进一步优化

### 5.3 错误处理模式

**HXLibs 的做法**：
```cpp
// 使用 C++ 异常
coroutine::Task<> someAsyncOperation() {
    char buf[1024];
    co_await _io.fullyRecv(buf);
    
    if (buf[0] != 0x05) {
        throw std::invalid_argument("Protocol error");
    }
}

// 上层捕获异常
try {
    co_await socks5Proxy.connect(...);
} catch (const std::exception& e) {
    log::hxLog.error("Proxy connection failed:", e.what());
}
```

**对 vmess 的参考**：
- 使用异常简化错误处理
- 在协程中，异常会自动传播到调用者

---

## 六、总结

### 6.1 HXLibs 的核心优势

1. **现代 C++ 设计**：充分使用 C++20 协程、concepts、span 等特性
2. **高性能**：基于 io_uring，避免系统调用开销
3. **易用性**：协程让异步代码像同步代码一样直观
4. **跨平台**：同时支持 Linux (io_uring) 和 Windows (IOCP)

### 6.2 对 vmess 项目的建议

1. **使用协程封装异步操作**：参考 `AioTask` 和 `EventLoop` 的设计
2. **实现完整的 vmess 协议状态机**：参考 `Socks5Proxy` 的分层设计
3. **优化数据转发性能**：使用 io_uring 的批量操作
4. **错误处理**：使用异常简化代码逻辑

### 6.3 下一步

1. 在 vmess 项目中实现类似的 `EventLoop` 和 `AioTask`
2. 使用协程重写代理协议处理
3. 性能测试：对比回调模式 vs 协程模式

---

## 附录：关键文件索引

| 文件路径 | 说明 |
|---------|------|
| `include/HXLibs/coroutine/task/Task.hpp` | 协程 Task 包装器 |
| `include/HXLibs/coroutine/task/AioTask.hpp` | 异步 IO 任务，桥接协程与 io_uring |
| `include/HXLibs/coroutine/loop/EventLoop.hpp` | 事件循环，调度协程和 IO |
| `include/HXLibs/net/socket/IO.hpp` | 网络 IO 封装 (fullySend/fullyRecv) |
| `include/HXLibs/net/protocol/proxy/Socks5Proxy.hpp` | SOCKS5 代理实现 |
| `lib/liburing/include/liburing.h` | liburing 头文件 (io_uring API) |
| `lib/liburing/liburing.cpp` | liburing 实现 |
