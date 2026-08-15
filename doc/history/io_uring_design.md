> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：旧回调式 + Buffer Selection 设计，已改为纯协程注册表驱动
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# vmess io_uring 设计与实现文档

## 一、设计目标

为 vmess 项目添加高性能异步 I/O 支持，基于 Linux io_uring 接口实现：

1. **零拷贝**：通过 Buffer Selection 机制，数据直接从网卡到用户缓冲区
2. **批量提交**：一次系统调用提交多个 I/O 操作，减少上下文切换
3. **与 Socket 封装解耦**：io_uring 通过 fd 操作，不依赖 Socket 类的具体实现
4. **渐进式集成**：先提供底层 API，后续扩展 C++20 协程接口

---

## 二、架构设计

```
┌─────────────────────────────────────────┐
│           用户代码                      │
│  - 使用 IoUring + Socket 封装         │
└─────────────────┬───────────────────┘
                  │
┌─────────────────▼───────────────────┐
│        io_uring 封装层                │
│  ┌─────────────────────────────┐   │
│  │  IoUring                    │   │  ← 底层 API（提交/处理完成事件）
│  └─────────────────────────────┘   │
└─────────────────┬───────────────────┘
                  │ fd
┌─────────────────▼───────────────────┐
│         Socket 封装层                 │
│  - ServerSocket (listen/accept)     │
│  - ClientSocket (connect/send/recv) │
└─────────────────┬───────────────────┘
                  │
┌─────────────────▼───────────────────┐
│         Linux 内核 (io_uring)        │
│  - SQ (Submission Queue)            │
│  - CQ (Completion Queue)            │
│  - Buffer Selection                 │
└─────────────────────────────────────┘
```

---

## 三、核心组件

### 3.1 `IoUring` 类（底层 API）

**文件**：`include/net/io_uring.h`, `src/net/io_uring.cpp`

**功能**：
- 封装 `io_uring` 的初始化、SQE 提交、CQE 处理
- 管理预分配的缓冲区组（用于 Buffer Selection）
- 提供**两阶段提交**接口：先准备 SQE，再批量提交

---

#### 核心设计：两阶段提交

io_uring 支持**批量提交** SQE，减少系统调用次数。我们提供两个阶段的接口：

```
阶段 1：准备 SQE（不提交）
    prepareAccept() / prepareRecv() / prepareSend() / prepareClose()
            ↓
    SQE 被添加到内核的 SQ 缓冲区（还未提交给内核）
            ↓
阶段 2：批量提交
    submitAll() 或 submitAndWait()
            ↓
    SQE 被真正提交给内核处理
```

**优势**：
- **减少系统调用**：多次准备，一次提交
- **更好的批处理**：可以在一个批次中提交多个 I/O 操作
- **灵活性**：可以根据需要选择立即提交或延迟提交

---

#### 关键方法

```cpp
// 初始化 io_uring
IoUring(unsigned int entries = 2048);

// 初始化缓冲区组（Buffer Selection）
bool initBuffers(unsigned groupId, size_t bufferSize, size_t bufferCount);

// ============ 阶段 1：准备 SQE（不提交）============
bool prepareAccept(int listenFd, struct sockaddr* clientAddr, 
                  socklen_t* addrLen, unsigned flags = 0);
bool prepareRecv(int fd, unsigned groupId, size_t bufferSize, 
                 unsigned flags = IOSQE_BUFFER_SELECT);
bool prepareRecv(int fd, void* buf, size_t len, unsigned flags = 0);
bool prepareSend(int fd, const void* buf, size_t len, unsigned flags = 0);
bool prepareShutdown(int fd);
bool prepareClose(int fd);

// ============ 阶段 2：提交 SQE ============
// 提交所有待处理的 SQE（不等待完成）
int submitAll();

// 提交所有待处理的 SQE 并等待指定数量的完成事件
int submitAndWait(unsigned waitNum = 1);

// ============ 处理完成事件 ============
void processCompletions(const UringCallback& callback);
bool runOnce(const UringCallback& callback);
```

---

#### 使用示例 1：立即提交（每次准备后立即提交）

```cpp
IoUring uring(1024);
uring.initBuffers(1337, 2048, 4096);

// 准备 accept 并立即提交
uring.prepareAccept(listenFd, ...);
uring.submitAll();  // 或者 uring.submitAndWait(1);

// 运行事件循环
uring.runOnce([](const UringRequest& req, int result, uint32_t flags) {
    if (req.type == (uint16_t)UringEventType::ACCEPT) {
        int clientFd = result;
        
        // 准备 recv 并立即提交
        uring.prepareRecv(clientFd, 1337, 2048, IOSQE_BUFFER_SELECT);
        uring.submitAll();
    }
});
```

---

#### 使用示例 2：批量提交（推荐，性能更好）

```cpp
IoUring uring(1024);
uring.initBuffers(1337, 2048, 4096);

// 准备多个 SQE
for (int i = 0; i < 10; i++) {
    uring.prepareAccept(listenFd, ...);
}

// 一次性提交所有 SQE（只产生 1 次系统调用）
uring.submitAll();

// 处理完成事件
uring.processCompletions([](const UringRequest& req, int result, uint32_t flags) {
    // 处理完成的事件
});
```

---

### 3.2 `UringRequest` 结构

**功能**：在 SQE 的 `user_data` 中传递上下文，在 CQE 中返回

```cpp
struct UringRequest {
    int fd;              // 文件描述符
    uint16_t type;       // 事件类型 (UringEventType)
    uint16_t bid;        // 缓冲区 ID (用于 Buffer Selection)
    
    // 转换为 64 位整数存储到 user_data
    uint64_t toUserData() const;
    
    // 从 user_data 恢复
    static UringRequest fromUserData(uint64_t data);
};
```

**设计要点**：
- `user_data` 是 64 位整数，可以存储一个指针或编码多个字段
- 这里将 `fd`、`type`、`bid` 编码到一个 64 位整数中
- 完成事件时，从 CQE 的 `user_data` 解码出原始请求信息

---

## 四、Buffer Selection 机制

### 4.1 原理

传统 `recv` 需要用户预先分配缓冲区，而 Buffer Selection 允许：
1. 预先向内核注册一组缓冲区
2. 提交 `recv` 时不指定缓冲区，让内核自动选择
3. 完成时通过 CQE 的 `flags` 字段返回缓冲区 ID

### 4.2 实现步骤

**步骤 1：注册缓冲区组**

```cpp
// 分配对齐的内存
char* buffers = new char[BUFFER_COUNT][BUFFER_SIZE];

// 提交 provide_buffers SQE
struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
io_uring_prep_provide_buffers(sqe, buffers, BUFFER_SIZE, BUFFER_COUNT, groupId, 0);

// 等待完成
io_uring_submit_and_wait(ring, 1);
```

**步骤 2：提交 recv 时使用 Buffer Selection**

```cpp
struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
io_uring_prep_recv(sqe, fd, NULL, BUFFER_SIZE, 0);
io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
sqe->buf_group = groupId;  // 指定缓冲区组
```

**步骤 3：完成时获取缓冲区 ID**

```cpp
// 从 CQE 的 flags 字段获取缓冲区 ID
int bid = cqe->flags >> 16;
char* data = buffers[bid];

// 使用完后归还缓冲区
io_uring_prep_provide_buffers(sqe, buffers[bid], BUFFER_SIZE, 1, groupId, bid);
```

---

## 五、事件循环流程

```
┌─────────────────────────────────────────────────────────┐
│                  事件循环 (runOnce)                    │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│  1. submitAndWait(1)                                 │
│     - 提交所有待处理的 SQE                            │
│     - 等待至少一个完成事件                              │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│  2. processCompletions(callback)                       │
│     - 遍历所有已完成的 CQE                            │
│     - 对每个 CQE 调用 callback(req, result, flags)    │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│  3. callback 中处理事件：                             │
│     - ACCEPT: 获取新连接 fd，提交 recv               │
│     - READ: 处理接收到的数据，提交 send               │
│     - WRITE: 发送完成，提交下一次 recv                │
│     - PROV_BUF: 缓冲区回收完成                        │
└─────────────────────────────────────────────────────────┘
```

---

## 六、完整示例：基于 IoUring + Socket 的 Echo Server

本节展示如何使用 `IoUring` 和 `Socket` 封装实现一个完整的 Echo Server。这是理解 vmess 项目 io_uring 封装的最佳入门示例。

### 6.1 设计思路

Echo Server 的核心流程：
1. 使用 `ServerSocket` 建立监听
2. 使用 `IoUring` 异步处理 `accept/recv/send/close`
3. 通过 `UringRequest` 在 SQE/CQE 间传递上下文
4. 利用 Buffer Selection 机制管理接收缓冲区

### 6.2 完整代码

```cpp
#include "net/io_uring.h"
#include "net/socket.h"
#include <iostream>
#include <cstring>

using namespace vmess::net;

class UringEchoServer {
public:
    UringEchoServer(uint16_t port) : port_(port) {}

    bool start() {
        // 1. 使用 Socket 封装建立监听
        if (!serverSocket_.listen(port_)) {
            std::cerr << "Failed to listen on port " << port_ << std::endl;
            return false;
        }
        serverSocket_.setNonBlocking(true);
        std::cout << "[Server] Listening on port " << port_ << "..." << std::endl;

        // 2. 初始化 io_uring
        uring_ = std::make_unique<IoUring>(2048);

        // 3. 初始化 Buffer Selection 缓冲区组
        if (!uring_->initBuffers(1337, 4096, 1024)) {
            std::cerr << "Failed to init buffers" << std::endl;
            return false;
        }

        // 4. 准备第一个 accept（不提交，等事件循环中统一提交）
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        uring_->prepareAccept(serverSocket_.fd(),
                              (struct sockaddr*)&clientAddr,
                              &addrLen);

        return true;
    }

    void run() {
        running_ = true;
        while (running_) {
            // 提交所有待处理的 SQE，并等待至少一个 CQE
            uring_->submitAndWait(1);

            // 处理所有完成的 CQE
            uring_->processCompletions(
                [this](const UringRequest& req, int result, uint32_t flags) {
                    handleCompletion(req, result, flags);
                });
        }
    }

    void stop() { running_ = false; }

private:
    void handleCompletion(const UringRequest& req, int result, uint32_t flags) {
        switch ((UringEventType)req.type) {
            case UringEventType::ACCEPT:
                handleAccept(result);
                break;
            case UringEventType::READ:
                handleRead(req, result, flags);
                break;
            case UringEventType::WRITE:
                handleWrite(req, result);
                break;
            case UringEventType::PROV_BUF:
                // 缓冲区已归还，无需处理
                break;
            default:
                break;
        }
    }

    void handleAccept(int clientFd) {
        if (clientFd < 0) {
            std::cerr << "Accept failed: " << clientFd << std::endl;
            return;
        }

        std::cout << "[Server] New connection: fd=" << clientFd << std::endl;

        // 为新连接提交 recv（使用 Buffer Selection）
        uring_->prepareRecv(clientFd, 1337, 4096, IOSQE_BUFFER_SELECT);

        // 重新提交 accept，接受下一个连接
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        uring_->prepareAccept(serverSocket_.fd(),
                              (struct sockaddr*)&clientAddr,
                              &addrLen);
    }

    void handleRead(const UringRequest& req, int result, uint32_t flags) {
        if (result <= 0) {
            // 客户端断开或出错
            std::cout << "[Server] Client disconnected: fd=" << req.fd << std::endl;
            uring_->prepareClose(req.fd);
            return;
        }

        // 从 CQE flags 获取缓冲区 ID
        uint16_t bid = flags >> 16;
        char* buffer = uring_->getBuffer(bid);

        std::cout << "[Server] Received " << result << " bytes from fd=" << req.fd << std::endl;

        // Echo：将收到的数据发回
        // 注意：这里为了简化直接复用同一个 buffer，实际应用中需要注意生命周期
        uring_->prepareSend(req.fd, buffer, result);

        // 归还缓冲区
        uring_->provideBuffer(1337, bid);
    }

    void handleWrite(const UringRequest& req, int result) {
        if (result < 0) {
            std::cerr << "[Server] Send failed for fd=" << req.fd << std::endl;
            uring_->prepareClose(req.fd);
            return;
        }

        std::cout << "[Server] Sent " << result << " bytes to fd=" << req.fd << std::endl;

        // 发送完成后，继续提交 recv 等待下一次数据
        uring_->prepareRecv(req.fd, 1337, 4096, IOSQE_BUFFER_SELECT);
    }

    uint16_t port_;
    ServerSocket serverSocket_;
    std::unique_ptr<IoUring> uring_;
    bool running_ = false;
};

// ============ 主函数 ============
int main() {
    UringEchoServer server(9999);
    if (server.start()) {
        server.run();
    }
    return 0;
}
```

### 6.3 关键点解析

#### (1) 两阶段提交的应用

```cpp
// 阶段 1：准备所有 SQE（不产生系统调用）
uring_->prepareAccept(...);   // 接受新连接
uring_->prepareRecv(...);     // 接收数据
uring_->prepareSend(...);     // 发送数据

// 阶段 2：一次性提交所有 SQE（只产生 1 次系统调用）
uring_->submitAndWait(1);
```

在回调中，我们可能同时准备了 `accept`、`recv`、`send`、`provideBuffer` 等多个 SQE，它们会在 `submitAndWait()` 时一次性提交给内核。

#### (2) 生命周期管理

| 阶段 | 操作 | 说明 |
|------|------|------|
| 连接建立 | `prepareAccept` → CQE 返回 `clientFd` | 新连接就绪 |
| 接收数据 | `prepareRecv` → CQE 返回 `result` 字节 | 数据写入内核分配的缓冲区 |
| 发送数据 | `prepareSend` → CQE 返回发送字节数 | 数据从缓冲区发送 |
| 继续接收 | `prepareRecv` | 循环等待下一次数据 |
| 连接关闭 | `prepareClose` | 清理资源 |

#### (3) Buffer Selection 的工作流程

```
1. initBuffers()      → 向内核注册 1024 个 4KB 缓冲区
2. prepareRecv()      → 提交 recv，不指定缓冲区（IOSQE_BUFFER_SELECT）
3. CQE 完成           → 内核选择缓冲区，flags >> 16 返回 bid
4. getBuffer(bid)     → 获取缓冲区指针，读取数据
5. provideBuffer()    → 使用完后归还缓冲区到内核池
```

#### (4) 为什么在处理 ACCEPT 时要立即重新提交 accept？

```cpp
void handleAccept(int clientFd) {
    // 处理新连接...
    uring_->prepareRecv(clientFd, ...);  // 为新连接提交 recv

    // 关键：立即重新提交 accept，才能接受下一个连接
    uring_->prepareAccept(serverSocket_.fd(), ...);
}
```

如果只提交一次 `accept`，服务器只能接受一个连接。每处理完一个 `ACCEPT` 事件，必须立即重新提交 `accept` SQE，才能持续接受新连接。

---

## 七、与 Socket 封装的关系

### 7.1 职责分离

| 组件 | 职责 |
|------|------|
| `Socket` / `ServerSocket` / `ClientSocket` | 同步 I/O 封装：创建、绑定、监听、连接、设置选项 |
| `IoUring` | 异步 I/O 引擎：SQE 准备、批量提交、CQE 处理、Buffer Selection |

**设计原则**：`IoUring` 只操作 `fd`，不感知 `Socket` 类的存在。这种解耦允许：
- 用 `ServerSocket` 建立监听，获取 `fd` 后交给 `IoUring` 处理
- 或者直接使用原生 `socket()` 创建的 `fd`

### 7.2 使用方式示例

**推荐方式：Socket 封装 + IoUring**

```cpp
// 1. 使用 Socket 封装建立监听（处理所有同步 setup）
ServerSocket server;
server.listen(9999);
server.setNonBlocking(true);
int listenFd = server.fd();

// 2. 使用 IoUring 做异步 I/O
IoUring uring(2048);
uring.initBuffers(1337, 4096, 1024);

// 3. 准备 accept（两阶段提交）
struct sockaddr_in clientAddr;
socklen_t addrLen = sizeof(clientAddr);
uring.prepareAccept(listenFd, (struct sockaddr*)&clientAddr, &addrLen);
uring.submitAndWait(1);

// 4. 在回调中处理完成事件...
```

---

## 八、性能优势

### 8.1 与传统方式的对比

| 特性 | 传统 Socket | io_uring |
|------|------------|----------|
| 系统调用 | 每次 I/O 都需要 | 批量提交，减少系统调用 |
| 数据拷贝 | 用户态↔内核态 | 共享内存，零拷贝 |
| 阻塞模型 | 阻塞/非阻塞+epoll | 纯异步，事件驱动 |
| 缓冲区管理 | 用户分配 | 内核自动选择（Buffer Selection） |
| 性能 | 大量系统调用开销 | 高吞吐量，低延迟 |

### 8.2  benchmarks（待补充）

- 连接建立速率
-  echo 吞吐量
- CPU 利用率对比

---

## 九、后续扩展

### 9.1 C++20 协程集成

参考 `co-uring-WebServer` 项目，使用 C++20 协程让异步代码看起来像同步：

```cpp
// 自定义 awaitable
task handleRequest(int fd) {
    char* buf;
    size_t n = co_await async_recv(fd, &buf);   // 异步读
    n = co_await async_send(fd, buf, n);        // 异步写
    co_await async_close(fd);                    // 异步关闭
}
```

### 9.2 线程池支持

- 将连接分配到多个线程
- 每个线程有自己的 io_uring 实例
- 使用 `IORING_SETUP_SQPOLL` 减少系统调用

### 9.3 更多协议支持

- HTTP/1.1 解析
- WebSocket 升级
- TLS 支持（通过 `IORING_OP_ACCEPT` + 用户态 TLS）

---

## 十、编译与测试

### 10.1 编译 Echo Server 示例

```bash
cd /data/workspace/vmess

# 编译 Echo Server 示例（假设文件为 tests/uring_echo_server.cpp）
g++ -std=c++20 -I include \
    tests/uring_echo_server.cpp \
    src/net/io_uring.cpp \
    src/net/socket.cpp \
    -o uring_echo_server \
    -lpthread -luring

# 或者使用 CMake
cd build
cmake ..
make -j4
```

### 10.2 运行测试

```bash
# 启动 Echo Server
./uring_echo_server
# 输出：[Server] Listening on port 9999...

# 在另一个终端使用 nc 或 telnet 连接测试
nc 127.0.0.1 9999

# 或者使用项目自带的 Socket Echo 测试客户端
./socket_echo_test client 127.0.0.1 9999
```

### 10.3 使用 Socket Echo Test 客户端交互测试

项目提供了基于同步 Socket 封装的 Echo 客户端，可用于测试 io_uring Echo Server：

```bash
# 终端 1：启动服务器
./socket_echo_test server 9999

# 终端 2：启动交互式客户端
./socket_echo_test client 127.0.0.1 9999

# 客户端交互示例
> Hello, io_uring!
[Client] Sent: Hello, io_uring! (16 bytes)
[Client] Received echo: Hello, io_uring! (16 bytes)
[Client] Echo verified: OK

> 中文测试
[Client] Sent: 中文测试 (12 bytes)
[Client] Received echo: 中文测试 (12 bytes)
[Client] Echo verified: OK

> quit
[Client] Disconnecting...
```

---

## 十一、参考资料

1. [io_uring 官方文档](https://kernel.dk/io_uring.pdf)
2. [co-uring-WebServer 项目](https://github.com/)
3. [Liburing 库文档](https://github.com/axboe/liburing)
4. [C++20 协程教程](https://en.cppreference.com/w/cpp/language/coroutines)

---

## 附录：关键数据结构

### A.1 `UringEventType`

```cpp
enum class UringEventType : uint16_t {
    ACCEPT = 0,
    READ = 1,
    WRITE = 2,
    PROV_BUF = 3,
    TIMEOUT = 4
};
```

### A.2 `UringCallback`

```cpp
using UringCallback = std::function<void(const UringRequest& req, int result, uint32_t flags)>;
```

**参数说明**：
- `req`：原始请求信息（fd、type、bid）
- `result`：操作结果（成功时为正值，失败时为负错误码）
- `flags`：CQE 标志（包含缓冲区 ID 等信息）

---

**文档版本**：v1.1  
**最后更新**：2026-04-30  
**作者**：vmess 项目团队

### 变更记录

- **v1.1**：移除 `UringEchoServer` 高级封装，改为使用 `IoUring` + `Socket` 封装的完整 Echo Server 示例；移除已废弃的兼容旧接口文档描述。
