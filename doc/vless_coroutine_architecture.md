# VLESS 服务端协程架构设计方案

## 一、方案共识确认

以下是我们达成一致的架构决策：

| 决策项 | 共识 | 说明 |
|--------|------|------|
| **协议解析** | 状态机驱动 | `VlessHeaderDecoder` 内部维护显式状态机 + 累积 buffer |
| **同步/异步兼容** | 分层设计 | 解析器提供 `decodeSync()` 和 `feed()` 两套 API，内部共享状态机 |
| **最终方向** | 协程 + io_uring | 每个连接一个 C++20 协程，`co_await` 驱动 I/O |
| **迁移策略** | 渐进式 | 先状态机 → 再协程封装 → 最后纯 awaitable |
| **阻塞读与事件循环** | **严禁混用** | io_uring 事件循环线程里不能调阻塞 `recv` |
| **连接关闭** | 优雅关闭 | 通过异常/标志通知协程退出，事件循环检测 `done()` 后清理 |

---

## 二、目标架构：io_uring + C++20 协程

### 2.1 核心设计

每个 TCP 连接对应一个 C++20 协程。协程内部用 `co_await` 发起 I/O 操作，io_uring 事件循环负责在 CQE 到达时恢复协程。

```
┌─────────────────────────────────────────────────────────────┐
│                  io_uring 事件循环（单线程）                   │
│                                                             │
│   while (running) {                                         │
│       uring.submitAndWait(1);                               │
│       uring.processCompletions([](cqe) {                    │
│           fd = cqe.fd;                                      │
│           handle = coroutineMap[fd];      ← 查协程句柄       │
│           promise.result = cqe.res;       ← 存入结果         │
│           handle.resume();                ← 恢复协程         │
│           if (handle.done()) {            ← 协程结束？       │
│               handle.destroy();                             │
│               coroutineMap.erase(fd);                       │
│           }                                                 │
│       });                                                   │
│   }                                                         │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  协程 A (fd=3) │    │  协程 B (fd=5) │    │  协程 C (fd=7) │
│               │    │               │    │               │
│ co_await read │    │ co_await send │    │ co_await read │
│      ↓        │    │      ↓        │    │      ↓        │
│   挂起等待     │    │   挂起等待     │    │   挂起等待     │
│      ↑        │    │      ↑        │    │      ↑        │
│   resume恢复   │    │   resume恢复   │    │   resume恢复   │
│   继续执行     │    │   继续执行     │    │   继续执行     │
└───────────────┘    └───────────────┘    └───────────────┘
```

### 2.2 协程 = 隐式状态机

协程的挂起点天然对应状态转换点：

```cpp
task<void> handleConnection(int clientFd) {
    // 挂起点 1：等 VLESS 请求头
    auto request = co_await vlessReadRequest(clientFd);
    
    // 挂起点 2：等目标服务器连接完成
    int targetFd = co_await asyncConnect(request.address, request.port);
    
    // 挂起点 3：等双向转发任意一方完成
    auto [done, which] = co_await whenAny(
        copyStream(clientFd, targetFd),
        copyStream(targetFd, clientFd)
    );
    
    // 清理
    co_await asyncClose(targetFd);
    co_await asyncClose(clientFd);
}
```

对比显式状态机：

| 显式状态机 | 协程 |
|-----------|------|
| `state = READ_VERSION` | `uint8_t v = co_await readByte(fd)` |
| `state = READ_UUID` | `auto uuid = co_await readBytes(fd, 16)` |
| `state = READ_ADDONS_LEN` | `uint8_t m = co_await readByte(fd)` |
| `buffer_.erase(...)` | 协程局部变量自动管理 |
| `if (!hasEnough(n)) return false` | `co_await` 自动挂起 |

**关键洞察**：协程的局部变量就是状态，挂起/恢复就是状态转换。

---

## 三、渐进式迁移路径

### 3.1 阶段 1：状态机核心（现在）

**目标**：验证 VLESS 协议解析正确性。

```cpp
class VlessHeaderDecoder {
public:
    // 同步 API：MVP 阶段直接使用
    std::optional<VlessRequest> decodeSync(Socket& socket);
    
    // 异步 API：为后续预留
    bool feed(const uint8_t* data, size_t len);
    bool isComplete() const;
    const VlessRequest& result() const;
    size_t bytesNeeded() const;
    void reset();

private:
    enum class State {
        READ_VERSION, READ_UUID, READ_ADDONS_LEN,
        READ_ADDONS, READ_COMMAND, READ_PORT,
        READ_ADDR_TYPE, READ_ADDR_LEN, READ_ADDR,
        COMPLETE
    };
    State state_ = State::READ_VERSION;
    std::vector<uint8_t> buffer_;
    VlessRequest result_;
    uint8_t addonsLen_ = 0;
    uint8_t addrLen_ = 0;
};
```

**使用方式**：
```cpp
// 每个连接一个线程，线程里阻塞读
threadPool.submit([fd]() {
    Socket socket(fd);
    VlessHeaderDecoder decoder;
    auto req = decoder.decodeSync(socket);
    // ...
});
```

### 3.2 阶段 2：协程 + 缓冲层（下一步）

**目标**：外层代码看起来像协程，内部复用状态机，底层通过 `UringBufferedStream` 聚合小量读。

```cpp
task<VlessRequest> vlessReadRequest(UringBufferedStream& stream) {
    VlessHeaderDecoder decoder;
    
    while (!decoder.isComplete()) {
        // 从缓冲流读取：若 buffer 够则零 syscall 返回，否则挂起等主循环
        auto data = co_await stream.read(decoder.bytesNeeded());
        decoder.feed(data.data(), data.size());
    }
    
    co_return decoder.result();
}
```

**好处**：
- 外层 `handleConnection` 用 `co_await vlessReadRequest(stream)`，代码线性
- 内部复用已验证的 `VlessHeaderDecoder`，不重复造轮子
- 小量读（1B/2B）大多命中 buffer，**不产生 io_uring SQE**
- 底层始终大块读（4KB），最大化 io_uring 吞吐量

### 3.3 阶段 3：纯协程 awaitable（未来）

**目标**：所有 I/O 操作都是 `co_await stream.xxx()`，不再依赖显式状态机。

```cpp
task<VlessRequest> vlessReadRequest(UringBufferedStream& stream) {
    VlessRequest req;
    
    req.version = (co_await stream.read(1))[0];
    req.uuid = co_await stream.read(16);
    
    uint8_t addonsLen = (co_await stream.read(1))[0];
    co_await stream.read(addonsLen);  // 跳过 addons
    
    req.command = (co_await stream.read(1))[0];
    auto portBytes = co_await stream.read(2);
    req.port = (portBytes[0] << 8) | portBytes[1];
    
    uint8_t addrType = (co_await stream.read(1))[0];
    // ... 地址变长解析
    
    co_return req;
}
```

**好处**：代码最简洁，状态完全隐含在协程栈中；底层仍由 `UringBufferedStream` 聚合 I/O。

**代价**：需要完善的 `stream.read(n)` awaitable 封装（已在阶段 2 完成）。

---

## 四、UringBufferedStream 缓冲层设计

### 4.1 为什么必须加缓冲层

VLESS 请求头包含大量 1B/2B 小字段：

| 字段 | 长度 | 若为独立 SQE 的代价 |
|------|------|-------------------|
| version | 1B | 1 次 SQE/CQE |
| UUID | 16B | 16 次 SQE/CQE（若逐字节） |
| addons len | 1B | 1 次 SQE/CQE |
| command | 1B | 1 次 SQE/CQE |
| port | 2B | 2 次 SQE/CQE |
| addr type | 1B | 1 次 SQE/CQE |

若阶段 3 直接 `co_await readByte(fd)` 且无缓冲，一次请求头解析可能触发 **20+ 次** io_uring 往返，性能完全不可接受。

**解决方案**：`UringBufferedStream` 中间层。
- 主循环始终发起**大块读**（如 4KB）
- 业务协程从 buffer 中**小量消费**
- buffer 足够时 **`await_ready()` 直接返回，零 syscall**

### 4.2 类设计

```cpp
class UringBufferedStream {
public:
    explicit UringBufferedStream(int fd) : fd_(fd) {}

    // 业务协程使用
    auto read(size_t need) {
        struct ReadAwaitable {
            UringBufferedStream& s;
            size_t need;

            // 关键优化：buffer 够就不挂起
            bool await_ready() const {
                return s.available() >= need;
            }

            void await_suspend(std::coroutine_handle<> h) {
                s.pendingNeed_ = need;
                s.pendingHandle_ = h;
                s.needRead_ = true;  // 通知主循环：需要底层读
            }

            std::span<uint8_t> await_resume() {
                return s.consume(need);
            }
        };
        return ReadAwaitable{*this, need};
    }

    auto readByte() { return read(1); }

    // 主循环使用
    void feed(const uint8_t* data, size_t len) {
        buffer_.insert(buffer_.end(), data, data + len);

        // buffer 够了？恢复等待中的业务协程
        if (pendingHandle_ && available() >= pendingNeed_) {
            auto h = pendingHandle_;
            pendingHandle_ = nullptr;
            h.resume();
        }
    }

    bool needsRead() const { return needRead_; }
    void clearNeedRead() { needRead_ = false; }
    int fd() const { return fd_; }

    // 供主循环 prepareRecv 的接收缓冲区
    uint8_t* recvBuffer() { return recvBuf_.data(); }
    static constexpr size_t recvBufferSize() { return 4096; }

private:
    size_t available() const {
        return buffer_.size() - consumed_;
    }

    std::span<uint8_t> consume(size_t n) {
        auto* ptr = buffer_.data() + consumed_;
        consumed_ += n;

        // 全部消费完则回收内存
        if (consumed_ == buffer_.size()) {
            buffer_.clear();
            consumed_ = 0;
        }
        return {ptr, n};
    }

    int fd_;

    // 已缓冲数据
    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;

    // 底层接收缓冲区（供 io_uring 直接 DMA）
    alignas(alignof(std::max_align_t))
    std::array<uint8_t, 4096> recvBuf_;

    // pending 读请求
    size_t pendingNeed_ = 0;
    std::coroutine_handle<> pendingHandle_ = nullptr;
    bool needRead_ = false;
};
```

### 4.3 主循环配合

```cpp
class UringCoroutineServer {
    IoUring uring_;
    std::unordered_map<int, std::coroutine_handle<>> coroutineMap_;
    std::unordered_map<int, std::unique_ptr<UringBufferedStream>> streams_;

public:
    void run() {
        while (running_) {
            // 1. 为 buffer 空了但业务还在读的连接 prepare 大块读
            for (auto& [fd, stream] : streams_) {
                if (stream->needsRead()) {
                    uring_.prepareRecv(fd, stream->recvBuffer(),
                                       UringBufferedStream::recvBufferSize());
                    stream->clearNeedRead();
                }
            }

            // 2. 批量提交 + 等待至少 1 个 CQE
            uring_.submitAndWait(1);

            // 3. CQE 分发
            uring_.processCompletions([this](const UringRequest& req,
                                              int result, uint32_t flags) {
                handleCqe(req, result, flags);
            });
        }
    }

    void handleCqe(const UringRequest& req, int result, uint32_t flags) {
        int fd = req.fd;

        switch (static_cast<UringEventType>(req.type)) {
            case UringEventType::ACCEPT: {
                int clientFd = result;
                // 创建连接对应的 BufferedStream
                streams_[clientFd] =
                    std::make_unique<UringBufferedStream>(clientFd);
                // 启动业务协程
                auto task = handleConnection(*streams_[clientFd]);
                break;
            }
            case UringEventType::READ: {
                auto it = streams_.find(fd);
                if (it == streams_.end()) return;

                if (result > 0) {
                    // 数据入 buffer；feed 内部若满足 pending 则 resume 协程
                    it->second->feed(it->second->recvBuffer(), result);
                } else {
                    // 对端关闭/出错
                    closeConnection(fd);
                }
                break;
            }
            // ... WRITE, CLOSE 处理
        }
    }

    task<void> handleConnection(UringBufferedStream& stream) {
        // VLESS 处理逻辑，内部 co_await stream.read(n)
    }
};
```

### 4.4 执行时序示例

```
业务协程:  co_await stream.read(1)   // 读 version（1B）
             |
             v
ReadAwaitable: await_ready()? buffer 空 -> false
             |
             v
await_suspend: pendingNeed=1, needRead=true, 挂起
             |
             v
主循环:     prepareRecv(fd, 4096)      // 大块读
            submitAndWait()
             |
             v
内核:       收到 4KB 数据
             |
             v
主循环 CQE: stream.feed(4KB)
            buffer 现在有 4096B >= pendingNeed(1)
            -> resume 业务协程！
             |
             v
业务协程:   await_resume() 返回 1B
            继续: co_await stream.read(16)  // 读 UUID
             |
             v
ReadAwaitable: await_ready()? 4095B >= 16 -> true！
            *** 不挂起，直接返回 16B ***
             |
             v
业务协程:   继续: co_await stream.read(1)   // addonLen
            await_ready()? 4079B >= 1 -> true，直接返回
            继续: co_await stream.read(2)   // port
            await_ready()? 4078B >= 2 -> true，直接返回
            ...
            连续消费几十字节，零 syscall！
```

### 4.5 零拷贝优化：解析后的剩余数据

VLESS 请求头解析完成后，`stream` 的 `buffer_` 中可能还剩余客户端早早发来的 payload。直接移交，避免再 `recv`：

```cpp
task<void> handleConnection(UringBufferedStream& clientStream) {
    auto request = co_await vlessReadRequest(clientStream);
    int targetFd = co_await asyncConnect(request.address, request.port);

    // 交出 clientStream 中剩余未消费的 payload
    auto remaining = clientStream.drainRemaining();
    if (!remaining.empty()) {
        co_await asyncSend(targetFd, remaining.data(), remaining.size());
    }

    // 双向转发
    co_await whenAny(
        copyStream(clientStream, targetFd),
        copyStream(targetFd, clientStream)
    );
}
```

---

## 五、io_uring Awaitable 封装设计

### 4.1 核心 Awaitable：UringOpAwaitable

```cpp
template<typename PrepareFn>
struct UringOpAwaitable {
    IoUring& uring;
    int fd;
    PrepareFn prepare;
    
    bool await_ready() const { return false; }
    
    void await_suspend(std::coroutine_handle<> h) {
        // 1. 准备 SQE
        prepare(uring, fd);
        // 2. 注册：fd → coroutine_handle
        coroutineMap[fd] = h;
        // 3. 不立即 submit，等事件循环批量提交
    }
    
    int await_resume() const {
        return lastCqeResult;  // 从线程局部变量或 promise 获取
    }
};

// 便捷函数
task<int> asyncRecv(int fd, void* buf, size_t len) {
    co_return co_await UringOpAwaitable{
        uring, fd,
        [buf, len](IoUring& u, int fd) {
            u.prepareRecv(fd, buf, len);
        }
    };
}

task<int> asyncSend(int fd, const void* buf, size_t len) {
    co_return co_await UringOpAwaitable{
        uring, fd,
        [buf, len](IoUring& u, int fd) {
            u.prepareSend(fd, buf, len);
        }
    };
}
```

### 4.2 事件循环中的 CQE 分发

```cpp
class UringCoroutineServer {
    IoUring uring_;
    std::unordered_map<int, std::coroutine_handle<>> coroutineMap_;
    
public:
    void run() {
        while (running_) {
            uring_.submitAndWait(1);
            
            uring_.processCompletions([this](const UringRequest& req, int result, uint32_t flags) {
                handleCqe(req, result, flags);
            });
        }
    }
    
    void handleCqe(const UringRequest& req, int result, uint32_t flags) {
        int fd = req.fd;
        
        switch ((UringEventType)req.type) {
            case UringEventType::ACCEPT: {
                int clientFd = result;
                // 创建新协程，第一个 co_await 会自动注册到 coroutineMap
                auto task = handleConnection(clientFd);
                break;
            }
            case UringEventType::READ:
            case UringEventType::WRITE: {
                auto it = coroutineMap_.find(fd);
                if (it != coroutineMap_.end()) {
                    lastCqeResult_ = result;
                    it->second.resume();
                    
                    if (it->second.done()) {
                        it->second.destroy();
                        coroutineMap_.erase(it);
                    }
                }
                break;
            }
            case UringEventType::CLOSE: {
                auto it = coroutineMap_.find(fd);
                if (it != coroutineMap_.end()) {
                    it->second.destroy();
                    coroutineMap_.erase(it);
                }
                break;
            }
        }
    }
    
    task<void> handleConnection(int clientFd) {
        // ... VLESS 处理逻辑
    }
};
```

---

## 五、连接关闭的处理策略

### 5.1 优雅关闭（推荐）

```cpp
task<void> handleConnection(int clientFd) {
    try {
        auto request = co_await vlessReadRequest(clientFd);
        int targetFd = co_await asyncConnect(request.address, request.port);
        
        auto t1 = copyStream(clientFd, targetFd);
        auto t2 = copyStream(targetFd, clientFd);
        co_await whenAny(t1, t2);
        
        co_await asyncClose(targetFd);
    } catch (const ConnectionClosedException& e) {
        // 对端关闭或出错，协程自然退出
    }
    
    co_await asyncClose(clientFd);
    // 协程结束，事件循环检测到 done() 后 destroy
}
```

事件循环中：
```cpp
// READ 返回 0 = 对端正常关闭
// READ 返回负值 = 连接错误
if (result <= 0) {
    setCoroutineException(fd, result == 0 ? ECONNRESET : -result);
    handle.resume();  // 协程 catch 异常后退出
}
```

### 5.2 强制销毁（紧急场景）

```cpp
// 直接 destroy，但不推荐常规使用
handle.destroy();
coroutineMap_.erase(fd);
// 风险：协程栈上的 RAII 对象可能不调用析构
```

---

## 六、与现有代码的关系

| 现有组件 | 在协程架构中的角色 | 是否需要修改 |
|---------|------------------|------------|
| `IoUring` | 底层 I/O 引擎，协程通过它提交/等待 | **否** |
| `Socket` / `ServerSocket` | 阶段 1 同步测试用，阶段 2+ 不再直接使用 | 否 |
| `VlessHeaderDecoder` | **核心复用组件**，协程内部调用 `feed()` | **否** |
| `UringRequest` / `UringEventType` | CQE 上下文，事件循环分发用 | 否 |

**新增组件**：
- `UringBufferedStream`：**缓冲层**，聚合小量读、减少 io_uring 往返
- `UringCoroutineServer`：事件循环 + 协程映射表 + stream 管理
- `UringOpAwaitable`：io_uring 操作的 awaitable 封装（非读操作）
- `task<T>`：协程的 promise_type 封装

---

## 七、潜在风险与应对

| 风险 | 影响 | 应对 |
|------|------|------|
| C++20 协程编译器 bug | GCC < 10 支持不完整 | 要求 GCC 11+ 或 Clang 15+ |
| 协程内存分配开销 | 每个连接一个堆分配 | 自定义 `operator new` 用内存池 |
| 协程调试困难 | 挂起/恢复打断调用栈 | 日志记录协程 id 和挂起状态 |
| 批量提交延迟 | 每个 co_await 产生一个 SQE | `UringBufferedStream` 聚合小量读，buffer 命中时不产生 SQE |
| 异常传播 | 协程内抛异常需正确处理 | promise_type 中实现 `unhandled_exception` |

---

## 八、实施计划

### 第一步（本周）：状态机核心

1. 实现 `VlessHeaderDecoder` 类
2. 写单元测试：构造二进制 VLESS 请求头，验证 `feed()` 解析正确
3. 用 `socket_echo_test` 或 nc 做端到端验证

### 第二步（下周）：协程 + 缓冲层

1. 实现 `UringBufferedStream` 类（含 `ReadAwaitable`）
2. 实现 `task<T>` promise_type
3. 实现 `UringOpAwaitable`（仅 `asyncSend` / `asyncConnect` / `asyncClose`）
4. 实现 `UringCoroutineServer` 事件循环（带 `streams_` 管理）
5. 用协程重写 `handleConnection`，内部 `co_await stream.read(n)` + `VlessHeaderDecoder::feed()`

### 第三步（可选）：纯协程优化

1. 移除 `VlessHeaderDecoder`，`vlessReadRequest` 完全用 `co_await stream.read(n)` 驱动
2. 性能基准测试（对比阶段 1/2/3）

---

## 九、关键结论

1. **协程架构是可行的**，且与现有 io_uring 封装完全兼容
2. **渐进式迁移是最务实的路径**：先状态机验证正确性，再套协程语法糖
3. **显式状态机不会被浪费**：阶段 2 中协程内部仍复用它，阶段 3 才完全替换
4. **阻塞读和事件循环严禁混用**：同步测试必须跑在线程池里，不能卡在事件循环线程
5. **连接关闭走优雅路径**：异常传播 + `done()` 检测，避免强制 `destroy`
6. **BufferedStream 是性能关键**：没有它，阶段 3 的细粒度 `co_await` 会导致 io_uring 往返爆炸
7. **prepare/submit 分离**：协程负责 prepare SQE，事件循环负责批量 submit，两者不矛盾

---

**文档版本**：v1.1  
**最后更新**：2026-04-30
