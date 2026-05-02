# VLESS 协议解析方案设计文档

## 一、Go 官方是怎么读的？

### 1.1 代码层面的做法

从 `inbound.go` 和 `encoding.go` 的代码来看，Go 官方的实现可以概括为：

```go
// ① 先读一段数据（不固定长度，能读多少读多少）
first := buf.New()
first.ReadFrom(connection)

// ② 包装成 BufferedReader：先从 first 消费，不够再从 connection 读
reader := &buf.BufferedReader{
    Reader: buf.NewReader(connection),
    Buffer: buf.MultiBuffer{first},
}

// ③ 解码器按需精确读取
request, err := encoding.DecodeRequestHeader(first, reader, validator)
```

### 1.2 `BufferedReader` 的本质

Go 官方没有显式状态机，靠的是 `BufferedReader` 这个**隐式缓冲区管理器**：

```
┌─────────────────────────────────────────┐
│           BufferedReader                 │
│  ┌─────────────────────────────────┐   │
│  │  Buffer（已读但未消费的数据）      │   │ ← 先从这里面取
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │  io.Reader（底层连接）            │   │ ← 不够了再阻塞读
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

**关键行为**：
- `ReadFullFrom(reader, n)` 说"我要 n 字节"
- `BufferedReader` 先看 Buffer 里有够不够：
  - 够 → 直接从 Buffer 返回，**不触发系统调用**
  - 不够 → 从底层 `io.Reader` **阻塞读**，直到凑够 n 字节

### 1.3 解码器的读取流程

VLESS 请求头的解析是**严格顺序、按需定长读取**的：

```
读取步骤                字节数      累计      下一步依赖
─────────────────────────────────────────────────────────
版本                   1 B        1 B       确定分支
UUID                   16 B       17 B      验证用户
addons 长度 M          1 B        18 B      知道 addons 多长
addons                 M B        18+M      （当前可忽略）
指令                   1 B        19+M      确定 TCP/UDP/Mux
端口                   2 B        21+M      
地址类型               1 B        22+M      确定地址长度
地址                   4/16/N B   变长      解析完成
─────────────────────────────────────────────────────────
```

每一步都是 `ReadFullFrom(reader, n)` —— **阻塞直到读满 n 字节**。

### 1.4 为什么 Go 不需要显式状态机？

因为 **Go 的阻塞读本身就是状态机**。

Go 代码看起来是线性的：

```go
version := read(1)      // ① 阻塞直到读到 1B
uuid := read(16)        // ② 阻塞直到读到 16B
addonsLen := read(1)    // ③ 阻塞直到读到 1B
addons := read(addonsLen) // ④ 阻塞直到读到 M B
command := read(1)      // ⑤ 阻塞直到读到 1B
// ...
```

但底层执行时，Go 运行时帮它做了这件事：

```
线程执行 read(1) → 没数据？→ 挂起协程 → 等 epoll 通知 → 恢复协程 → 继续执行下一行
```

**本质上**：Go 协程的挂起/恢复就是一个**由运行时管理的状态机**。只是对程序员来说，代码看起来是同步线性的。

---

## 二、C++ 同步 vs 异步的根本差异

### 2.1 C++ 同步阻塞读

和 Go 一样，直接调 `recv(fd, buf, n)`，内核阻塞直到数据到达：

```cpp
uint8_t version;
recv(fd, &version, 1);     // 阻塞，直到收到 1 字节

uint8_t uuid[16];
recv(fd, uuid, 16);        // 阻塞，直到收到 16 字节

uint8_t addonsLen;
recv(fd, &addonsLen, 1);   // 阻塞，直到收到 1 字节
// ...
```

**优点**：代码和 Go 一样直观，无需额外设计。

**缺点**：每次读都是系统调用；线程阻塞无法处理其他连接（需要每个连接一个线程）。

### 2.2 C++ 异步事件驱动

io_uring/epoll 通知你"有数据可读"，但**不保证读到的数据量**：

```cpp
// 你准备了 512B 的 buffer
prepareRecv(fd, buf, 512);
submitAndWait(1);
// CQE 告诉你：实际读了 37 字节
```

**37 字节可能是什么？**

| 场景 | 内容 | 处理 |
|------|------|------|
| A | 完整请求头（~25B）+ 12B 请求数据 | 解析头，剩余 12B 是数据流的一部分 |
| B | 只有部分请求头（如前 17B = 版本+UUID） | 需要继续读取，拼凑完整头部 |
| C | 恰好完整请求头 | 解析完成后，后续再 recv 请求数据 |

**核心问题**：异步 recv 返回的数据边界 ≠ 协议字段边界。你必须自己维护 buffer，做拆包/粘包处理。

---

## 三、推荐的 C++ 实现方案

### 3.1 方案对比

| 方案 | 做法 | 优点 | 缺点 | 适用阶段 |
|------|------|------|------|----------|
| A 纯同步阻塞读 | 直接 `recv(fd, buf, n)` | 最简单，和 Go 代码一致 | 每连接一线程，系统调用多 | 快速验证 |
| B 同步封装+内部状态机 | 外层阻塞读，内部状态机解析 | 同步时简单，异步可复用 | 设计稍复杂 | **推荐** |
| C 纯异步状态机 | 事件驱动，回调推进状态 | 最高性能，单线程多连接 | 设计最复杂，调试困难 | 生产环境 |

### 3.2 推荐方案 B 详解

**核心思想**：

> 解析器内部维护一个**显式状态机** + **累积 buffer**。外层可以用同步阻塞读调用，也可以用异步回调调用，**解析器本身不关心**。

```
┌─────────────────────────────────────────────────────────┐
│                     外层调用方式                          │
│  ┌─────────────┐              ┌─────────────────────┐  │
│  │ 同步版本     │              │ 异步版本             │  │
│  │ recv → feed │              │ CQE → feed          │  │
│  └──────┬──────┘              └──────────┬──────────┘  │
│         │                                │             │
│         └────────────┬───────────────────┘             │
│                      ▼                                  │
│  ┌─────────────────────────────────────────────────┐   │
│  │           VlessHeaderDecoder（状态机）            │   │
│  │  ┌─────────────────────────────────────────┐   │   │
│  │  │  State: READ_VERSION → READ_UUID → ...  │   │   │
│  │  └─────────────────────────────────────────┘   │   │
│  │  ┌─────────────────────────────────────────┐   │   │
│  │  │  Buffer: 累积未解析的原始字节              │   │   │
│  │  └─────────────────────────────────────────┘   │   │
│  │  ┌─────────────────────────────────────────┐   │   │
│  │  │  Result: 解析完成的 VlessRequest          │   │   │
│  │  └─────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### 3.3 类的设计

```cpp
namespace vmess::protocol {

// VLESS 请求头数据结构
struct VlessRequest {
    uint8_t version;                    // 协议版本（当前为 0）
    std::array<uint8_t, 16> uuid;       // 用户 UUID（大端二进制）
    uint8_t command;                    // 1=TCP, 2=UDP, 3=Mux
    uint16_t port;                      // 目标端口（大端）
    
    // 目标地址
    enum class AddressType : uint8_t { IPv4 = 1, Domain = 2, IPv6 = 3 };
    AddressType addrType;
    std::variant<std::array<uint8_t, 4>,   // IPv4
                 std::array<uint8_t, 16>,  // IPv6
                 std::string>              // Domain
        address;
};

// 协议头解码器
class VlessHeaderDecoder {
public:
    VlessHeaderDecoder() = default;

    // ========== 同步 API：直接阻塞读取并解析 ==========
    // 
    // 内部实现：
    //   while (!complete_) {
    //       n = socket.recv(tempBuf, bytesNeeded());
    //       feed(tempBuf, n);
    //   }
    //   return result_;
    //
    // 这是 MVP 阶段的主要使用方式，代码和 Go 一样直观。
    std::optional<VlessRequest> decodeSync(Socket& socket);

    // ========== 异步 API：外部驱动状态机 ==========
    //
    // 使用方式（io_uring 场景）：
    //   1. prepareRecv(fd, buf, 512);
    //   2. submitAndWait(1);
    //   3. CQE 回调中：decoder.feed(buf, cqe->res);
    //   4. if (decoder.isComplete()) { 处理请求 }
    //   5. else { 继续 prepareRecv }
    //
    // 使用方式（epoll 场景）：
    //   1. EPOLLIN 触发
    //   2. recv(fd, buf, sizeof(buf));
    //   3. decoder.feed(buf, n);
    //   4. if (decoder.isComplete()) { 处理请求 }

    // 喂入数据，驱动状态机。返回 true 表示解析完成。
    bool feed(const uint8_t* data, size_t len);
    
    // 是否已解析完成
    bool isComplete() const;
    
    // 获取解析结果（仅在 isComplete() == true 时有效）
    const VlessRequest& result() const;
    
    // 当前状态还需要多少字节才能完成
    // 用于异步场景：指导下一次 recv 的 buffer 大小
    size_t bytesNeeded() const;

    // 重置状态机，复用 decoder 解析下一个连接
    void reset();

private:
    enum class State {
        READ_VERSION,       // 需 1 B
        READ_UUID,          // 需 16 B
        READ_ADDONS_LEN,    // 需 1 B
        READ_ADDONS,        // 需 M B（M 由上一状态确定）
        READ_COMMAND,       // 需 1 B
        READ_PORT,          // 需 2 B
        READ_ADDR_TYPE,     // 需 1 B
        READ_ADDR_LEN,      // 需 1 B（仅 Domain 类型）
        READ_ADDR,          // 需 4B/16B/N B
        COMPLETE
    };

    State state_ = State::READ_VERSION;
    std::vector<uint8_t> buffer_;   // 累积未解析的原始字节
    VlessRequest result_;
    
    // 状态机内部变量
    uint8_t addonsLen_ = 0;         // addons 长度（READ_ADDONS 阶段需要）
    uint8_t addrLen_ = 0;           // Domain 长度（READ_ADDR 阶段需要）

    // 尝试用 buffer_ 中的数据推进状态机
    bool parse();
    
    // 从 buffer_ 头部消费 n 字节到目标
    bool consume(void* dst, size_t n);
    
    // 当前 buffer_ 是否至少有 n 字节
    bool hasEnough(size_t n) const;
};

} // namespace vmess::protocol
```

### 3.4 `decodeSync` 的实现（MVP 阶段）

```cpp
std::optional<VlessRequest> VlessHeaderDecoder::decodeSync(Socket& socket) {
    // 分配一个临时 buffer，用于阻塞 recv
    // 大小取 bytesNeeded() 和某个合理值的较大者
    std::vector<uint8_t> tempBuf(256);
    
    while (!isComplete()) {
        size_t need = std::max(bytesNeeded(), size_t(1));
        
        // 阻塞读：精确读取 need 字节
        int n = socket.recv(tempBuf.data(), need);
        if (n <= 0) {
            return std::nullopt;  // 连接断开或出错
        }
        
        feed(tempBuf.data(), n);
    }
    
    return result_;
}
```

**和 Go 的等价关系**：

| Go 代码 | C++ 同步版本 |
|---------|-------------|
| `buffer.ReadFullFrom(reader, 1)` | `socket.recv(buf, 1)` |
| `buffer.ReadFullFrom(reader, 16)` | `socket.recv(buf, 16)` |
| `BufferedReader` 内部 buffer | `VlessHeaderDecoder::buffer_` |
| 协程挂起/恢复 | 线程阻塞/唤醒 |

### 3.5 `feed` + 状态机的实现（异步兼容）

```cpp
bool VlessHeaderDecoder::feed(const uint8_t* data, size_t len) {
    // 将新数据追加到累积 buffer
    buffer_.insert(buffer_.end(), data, data + len);
    
    // 循环尝试推进状态机，直到数据不够或解析完成
    while (parse()) {
        if (state_ == State::COMPLETE) {
            return true;
        }
    }
    return false;  // 数据不够，等待下一次 feed
}

bool VlessHeaderDecoder::parse() {
    switch (state_) {
        case State::READ_VERSION:
            if (!hasEnough(1)) return false;
            consume(&result_.version, 1);
            if (result_.version != 0) {
                // 不支持其他版本
                // 可选择报错或重置
            }
            state_ = State::READ_UUID;
            return true;
            
        case State::READ_UUID:
            if (!hasEnough(16)) return false;
            consume(result_.uuid.data(), 16);
            state_ = State::READ_ADDONS_LEN;
            return true;
            
        case State::READ_ADDONS_LEN:
            if (!hasEnough(1)) return false;
            consume(&addonsLen_, 1);
            state_ = State::READ_ADDONS;
            return true;
            
        case State::READ_ADDONS:
            if (!hasEnough(addonsLen_)) return false;
            // 目前 addons 无实际内容，直接跳过
            buffer_.erase(buffer_.begin(), buffer_.begin() + addonsLen_);
            state_ = State::READ_COMMAND;
            return true;
            
        case State::READ_COMMAND:
            if (!hasEnough(1)) return false;
            consume(&result_.command, 1);
            state_ = State::READ_PORT;
            return true;
            
        case State::READ_PORT:
            if (!hasEnough(2)) return false;
            uint8_t portBytes[2];
            consume(portBytes, 2);
            result_.port = (portBytes[0] << 8) | portBytes[1];  // 大端
            state_ = State::READ_ADDR_TYPE;
            return true;
            
        case State::READ_ADDR_TYPE:
            if (!hasEnough(1)) return false;
            uint8_t type;
            consume(&type, 1);
            result_.addrType = static_cast<VlessRequest::AddressType>(type);
            
            if (result_.addrType == VlessRequest::AddressType::IPv4) {
                addrLen_ = 4;
                state_ = State::READ_ADDR;
            } else if (result_.addrType == VlessRequest::AddressType::IPv6) {
                addrLen_ = 16;
                state_ = State::READ_ADDR;
            } else if (result_.addrType == VlessRequest::AddressType::Domain) {
                state_ = State::READ_ADDR_LEN;
            } else {
                // 未知类型，报错
                return false;
            }
            return true;
            
        case State::READ_ADDR_LEN:
            if (!hasEnough(1)) return false;
            consume(&addrLen_, 1);
            state_ = State::READ_ADDR;
            return true;
            
        case State::READ_ADDR:
            if (!hasEnough(addrLen_)) return false;
            // 根据类型读取地址
            if (result_.addrType == VlessRequest::AddressType::IPv4) {
                std::array<uint8_t, 4> ipv4;
                consume(ipv4.data(), 4);
                result_.address = ipv4;
            } else if (result_.addrType == VlessRequest::AddressType::IPv6) {
                std::array<uint8_t, 16> ipv6;
                consume(ipv6.data(), 16);
                result_.address = ipv6;
            } else {
                std::string domain(addrLen_, '\0');
                consume(domain.data(), addrLen_);
                result_.address = domain;
            }
            state_ = State::COMPLETE;
            return true;
            
        case State::COMPLETE:
            return false;  // 已完成，不再解析
    }
    return false;
}
```

---

## 四、后续切异步时的改动范围

从同步切到异步，**只需要改外层调用代码**，解析器本身完全不用动：

### 同步版本（当前）

```cpp
// 每个连接一个线程
void handleConnection(int clientFd) {
    Socket socket(clientFd);
    
    VlessHeaderDecoder decoder;
    auto request = decoder.decodeSync(socket);  // 内部循环 recv
    
    if (!request) { close(clientFd); return; }
    
    // 连接目标服务器...
    // 双向转发...
}
```

### 异步版本（未来）

```cpp
// 单线程事件循环
void onCqe(const UringRequest& req, int result, uint32_t flags) {
    auto& conn = connections[req.fd];
    
    if (conn.phase == Connection::READING_HEADER) {
        // 拿到数据，喂给状态机
        bool complete = conn.decoder.feed(
            uring.getBuffer(flags >> 16), result);
        
        if (complete) {
            auto& request = conn.decoder.result();
            // 解析完成，连接目标服务器...
            conn.phase = Connection::PROXYING;
        } else {
            // 头还没收完，继续 recv
            uring.prepareRecv(req.fd, groupId, 4096, IOSQE_BUFFER_SELECT);
        }
    }
    // ...
}
```

**改动点只有**：
- 把 `decoder.decodeSync(socket)` 换成 `decoder.feed()` + `isComplete()` 判断
- 解析器内部的状态机、buffer 管理、字段解析**零改动**

---

## 五、和现有项目的集成

### 文件布局建议

```
include/protocol/vless.h      # VlessRequest + VlessHeaderDecoder 声明
src/protocol/vless.cpp        # 实现

tests/vless_decoder_test.cpp  # 单元测试（用 feed API 喂二进制数据测试）
tests/vless_server_test.cpp   # 集成测试（用 Socket 做端到端测试）
```

### 与 `Socket` 封装的关系

- `decodeSync(Socket&)` 直接依赖 `Socket::recv()`
- `feed()` 完全不依赖 Socket，纯内存操作，可独立测试

### 与 `IoUring` 的关系

- 当前：**无关**。同步版本直接用 `Socket::recv()`。
- 未来：`feed()` 接收 `IoUring` 的 CQE 数据，驱动同一个状态机。

---

## 六、总结

| 问题 | 答案 |
|------|------|
| Go 怎么读的？ | 用 `io.Reader` + `BufferedReader` 做流式按需阻塞读 |
| Go 有状态机吗？ | **没有显式的**。Go 协程的挂起/恢复就是隐式状态机 |
| C++ 同步怎么读？ | 直接 `recv(fd, buf, n)` 阻塞读，和 Go 一样直观 |
| C++ 异步怎么读？ | 需要显式状态机 + 累积 buffer，处理数据边界不对齐 |
| 推荐方案是什么？ | **同步封装 + 内部状态机**。同步时好用，异步时复用 |
| 切异步要改多少代码？ | **只改外层调用**（5-10 行），解析器内部零改动 |

**下一步**：可以先写 `VlessHeaderDecoder` 的实现 + 单元测试，验证能正确解析 Shadowrocket 发送的请求头。

---

**文档版本**：v1.0  
**最后更新**：2026-04-30
