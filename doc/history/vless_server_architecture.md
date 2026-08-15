> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：旧式 prepareIO/onIOComplete 回调事件循环，已删除
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# VLESS 服务端架构设计文档

## 当前决策记录

| 决策项 | 内容 |
|--------|------|
| **加密** | 当前不支持，明文 VLESS |
| **XTLS** | 不考虑 |
| **主循环设计** | 极致精简，只调度 I/O |
| **协议逻辑** | 内聚到 `VlessConnection`，主循环无感知 |
| **协程范围** | 仅握手阶段 |
| **转发阶段** | 纯 io_uring，无协程 |
| **可插拔** | 通过 `Connection` 接口 + 工厂模式实现 |

---

## 一、核心架构

### 1.1 分层设计

```
┌─────────────────────────────────────────────┐
│              主循环（EventLoop）              │
│  - 只认识 Connection 接口                     │
│  - 极致精简：prepareIO → submit → onComplete  │
└─────────────────────────────────────────────┘
                      │
        ┌─────────────┼─────────────┐
        ▼             ▼             ▼
┌──────────────┐ ┌──────────┐ ┌──────────┐
│ VlessConnection│ │ VMessConn│ │ SOCKS5   │
│ (当前实现)     │ │ (未来)   │ │ (未来)   │
└──────────────┘ └──────────┘ └──────────┘
        │
        ▼
┌─────────────────────────────────────────────┐
│           VlessConnection 内部               │
│  ┌─────────────┐    ┌─────────────────┐     │
│  │ 握手阶段     │ →  │  转发阶段        │     │
│  │ (协程驱动)   │    │  (纯 io_uring)   │     │
│  │             │    │                  │     │
│  │ co_await    │    │ recv → send      │     │
│  │ stream.read │    │ (零协程开销)      │     │
│  └─────────────┘    └─────────────────┘     │
└─────────────────────────────────────────────┘
```

### 1.2 关键原则

- **主循环不感知任何协议细节**：不知道 VLESS、HTTP、SOCKS5 等
- **状态切换内聚到连接对象内部**：HANDSHAKE → RELAY → CLOSED
- **协程只存在于握手阶段**：转发阶段纯 io_uring，无协程开销
- **`prepareIO` 由连接自己决定**：需要 recv 还是 send，连接自己最清楚

---

## 二、接口设计

### 2.1 Connection 接口（主循环唯一依赖）

```cpp
class Connection {
public:
    virtual ~Connection() = default;
    
    // 主循环调用：准备 I/O 操作（填 SQE）
    virtual void prepareIO(IoUring& uring) = 0;
    
    // 主循环调用：CQE 到达
    virtual void onIOComplete(int fd, int result) = 0;
    
    // 主循环调用：连接是否已关闭
    virtual bool isClosed() const = 0;
    
    // 获取关联的 fd（用于查找）
    virtual int primaryFd() const = 0;
};
```

### 2.2 主循环（EventLoop）

```cpp
class EventLoop {
    IoUring uring_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    
public:
    void run() {
        while (running_) {
            // 1. 让每个连接准备它需要的 I/O
            for (auto& [fd, conn] : connections_) {
                conn->prepareIO(uring_);
            }
            
            // 2. 批量提交 + 等待
            uring_.submitAndWait(1);
            
            // 3. 分发 CQE
            uring_.processCompletions([&](const UringRequest& req, int res) {
                auto* conn = findConnection(req.fd);
                if (conn) conn->onIOComplete(req.fd, res);
            });
            
            // 4. 清理已关闭的连接
            cleanupClosedConnections();
        }
    }
};
```

**主循环只认识 `Connection` 接口，不认识 `VlessConnection`。**

---

## 三、VlessConnection 实现

### 3.1 状态机

```
ACCEPT
  │
  ▼
HANDSHAKE ──(协程完成)──► RELAY ──(连接关闭)──► CLOSED
  │                          │
  │ (出错)                   │ (出错)
  ▼                          ▼
CLOSED ◄─────────────────────┘
```

### 3.2 类设计

```cpp
class VlessConnection : public Connection {
public:
    enum class State { HANDSHAKE, RELAY, CLOSED };
    
    VlessConnection(int clientFd, IoUring& uring);

    // Connection 接口
    void prepareIO(IoUring& uring) override;
    void onIOComplete(int fd, int result) override;
    bool isClosed() const override;
    int primaryFd() const override;

private:
    // 握手阶段
    void prepareHandshakeIO(IoUring& uring);
    void onHandshakeComplete(int fd, int result);
    Task<int> processHandshake();
    
    // 转发阶段
    void prepareRelayIO(IoUring& uring);
    void onRelayComplete(int fd, int result);
    
    // 通用
    void close();

    int clientFd_;
    int targetFd_ = -1;
    IoUring& uring_;
    UringBufferedStream stream_;
    Task<int> processTask_;
    bool processStarted_ = false;
    State state_ = State::HANDSHAKE;
    
    // 转发缓冲区
    alignas(64) std::array<uint8_t, 4096> clientRecvBuf_;
    alignas(64) std::array<uint8_t, 4096> targetRecvBuf_;
    
    // 待发送队列
    struct PendingSend { int fd; uint8_t* buf; size_t len; };
    std::vector<PendingSend> pendingSends_;
};
```

### 3.3 握手阶段（协程驱动）

```cpp
void VlessConnection::prepareHandshakeIO(IoUring& uring) {
    if (!processStarted_) {
        processStarted_ = true;
        processTask_ = processHandshake();  // 启动协程
    }
    
    if (stream_.needsRead()) {
        uring.prepareRecv(clientFd_, stream_.recvBuffer(), 4096);
        stream_.clearNeedRead();
    }
}

void VlessConnection::onHandshakeComplete(int fd, int result) {
    if (result <= 0) { close(); return; }
    
    stream_.feed(stream_.recvBuffer(), result);
    
    if (processTask_.done()) {
        targetFd_ = processTask_.result();
        if (targetFd_ < 0) { close(); return; }
        state_ = State::RELAY;
    }
}

Task<int> VlessConnection::processHandshake() {
    try {
        auto req = co_await decodeRequestHeader(stream_, validator_);
        int target = co_await asyncConnect(req.address, req.port);
        co_await asyncSendResponse(clientFd_, req);
        co_return target;
    } catch (...) {
        co_return -1;
    }
}
```

### 3.4 转发阶段（纯 io_uring）

```cpp
void VlessConnection::prepareRelayIO(IoUring& uring) {
    // 处理上一轮的 pending sends
    for (auto& ps : pendingSends_) {
        uring.prepareSend(ps.fd, ps.buf, ps.len);
    }
    pendingSends_.clear();
    
    // 准备 recv
    uring.prepareRecv(clientFd_, clientRecvBuf_.data(), 4096);
    uring.prepareRecv(targetFd_, targetRecvBuf_.data(), 4096);
}

void VlessConnection::onRelayComplete(int fd, int result) {
    if (result <= 0) { close(); return; }
    
    if (fd == clientFd_) {
        pendingSends_.push_back({targetFd_, clientRecvBuf_.data(), result});
    } else {
        pendingSends_.push_back({clientFd_, targetRecvBuf_.data(), result});
    }
}
```

---

## 四、UringBufferedStream 设计

```cpp
class UringBufferedStream {
public:
    explicit UringBufferedStream(int fd);

    // 协程使用
    auto read(size_t need);
    auto readByte() { return read(1); }

    // 主循环使用
    void feed(const uint8_t* data, size_t len);
    bool needsRead() const;
    void clearNeedRead();
    uint8_t* recvBuffer();
    static constexpr size_t recvBufferSize() { return 4096; }

private:
    size_t available() const;
    std::span<uint8_t> consume(size_t n);

    int fd_;
    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;
    alignas(64) std::array<uint8_t, 4096> recvBuf_;
    
    size_t pendingNeed_ = 0;
    std::coroutine_handle<> pendingHandle_ = nullptr;
    bool needRead_ = false;
};
```

---

## 五、协程基础设施

### 5.1 Task<T>

```cpp
template<typename T = void>
struct Task {
    struct promise_type {
        T value{};
        std::coroutine_handle<> continuation;
        
        Task get_return_object();
        std::suspend_always initial_suspend();
        auto final_suspend() noexcept;
        void return_value(T v);
        void unhandled_exception();
    };
    
    std::coroutine_handle<promise_type> h;
    bool await_ready() const;
    void await_suspend(std::coroutine_handle<> caller);
    T await_resume();
    bool done() const;
};
```

### 5.2 asyncConnect / asyncSend

```cpp
Task<int> asyncConnect(const Address& addr, uint16_t port);
Task<void> asyncSend(int fd, const void* buf, size_t len);
```

---

## 六、未来可插拔设计

```cpp
class ConnectionFactory {
public:
    virtual std::unique_ptr<Connection> create(int fd, IoUring& uring) = 0;
};

class VlessConnectionFactory : public ConnectionFactory {
public:
    std::unique_ptr<Connection> create(int fd, IoUring& uring) override {
        return std::make_unique<VlessConnection>(fd, uring);
    }
};

// 主循环一行不改，只换 Factory
void onAccept(int clientFd) {
    auto conn = currentFactory_->create(clientFd, uring_);
    connections_[clientFd] = std::move(conn);
}
```

---

## 七、测试计划

### 7.1 单元测试

1. **UringBufferedStream 测试**
   - feed 数据后 read 命中 buffer
   - feed 不够时协程挂起，再 feed 后恢复

2. **VLESS 请求头解析测试**
   - 构造二进制 VLESS 请求头
   - 验证 `decodeRequestHeader` 解析正确

### 7.2 集成测试

1. **Echo 测试**
   - 客户端发送数据，服务端原样返回
   - 验证握手 + 转发流程

2. **Shadowrocket / Xray 客户端联调**
   - 使用 xray 客户端连接服务端
   - 验证真实场景下的协议兼容性

---

**文档版本**: v1.0  
**最后更新**: 2026-04-30
