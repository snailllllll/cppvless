# 协议架构设计文档

## 1. 架构设计：Server 与协议分离

### 1.1 当前问题（紧耦合）

典型的紧耦合设计（反例）：
```
┌─────────────────────────────────┐
│     TcpServer (混合了VMess)     │
│  - accept()                     │
│  - VMess握手 (硬编码)           │  ❌ 难以扩展
│  - VMess加密 (硬编码)           │  ❌ 难以测试
│  - VMess转发 (硬编码)           │  ❌ 难以维护
└─────────────────────────────────┘
```

### 1.2 推荐架构（松耦合）

```
┌──────────────────────────────────────────────────┐
│              Server Framework (服务框架)          │
│                                                  │
│  ┌─────────────┐  ┌─────────────┐              │
│  │ Acceptor    │  │ Connection   │              │
│  │ - listen()  │  │ - socket fd  │              │
│  │ - accept()  │  │ - 读写缓冲区 │              │
│  └─────────────┘  │ - IO事件     │              │
│                   └──────┬──────┘              │
│                          │                      │
│                  使用 Protocol 接口             │
│                          │                      │
│  ┌───────────────────────▼──────────────────┐  │
│  │        EventLoop (io_uring)              │  │
│  │  - epoll/io_uring 事件驱动               │  │
│  │  - 协程调度                              │  │
│  └──────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
                        │
                        │ 依赖倒置：依赖抽象，不依赖具体
                        │
        ┌───────────────┴───────────────┐
        │                               │
┌───────▼────────┐              ┌───────▼────────┐
│  VLESS Protocol│              │  VMess Protocol│
│  (轻量协议)    │              │  (复杂协议)     │
│                │              │                │
│  + Handshake() │              │  + Handshake() │
│  + Encrypt()   │              │  + Authenticate│
│  + Decrypt()   │              │  + Encrypt()   │
│  + Relay()     │              │  + Relay()     │
└────────────────┘              └────────────────┘
```

### 1.3 核心设计：协议接口（Protocol Interface）

```cpp
// include/protocol/protocol.h
#pragma once
#include <memory>
#include <string>
#include <cstdint>

namespace proxy {

// 前向声明
class Connection;

// ============================================================================
// 协议接口：所有代理协议必须实现此接口
// ============================================================================
class Protocol {
public:
    virtual ~Protocol() = default;

    // 协议标识
    [[nodiscard]]
    virtual std::string_view name() const = 0;

    // === 握手阶段 ===
    // 处理客户端握手，返回是否成功
    // 可能的协程版本：virtual Task<bool> handshake(Connection& conn) = 0;
    virtual bool handshake(Connection& conn) = 0;

    // === 数据加密/解密 ===
    // 加密数据（用于发送）
    virtual void encrypt(std::uint8_t* data, std::size_t len) = 0;

    // 解密数据（用于接收）
    virtual void decrypt(std::uint8_t* data, std::size_t len) = 0;

    // === 目标地址解析 ===
    // 从握手数据中提取目标地址（SOCKS5风格）
    struct TargetAddress {
        std::string host;
        std::uint16_t port;
        enum { IPv4, IPv6, DOMAIN } type;
    };
    virtual TargetAddress parse_target(Connection& conn) = 0;

    // === 数据转发 ===
    // 在客户端和远端之间转发数据
    virtual void relay(Connection& client_conn, Connection& remote_conn) = 0;
};

// ============================================================================
// 协议工厂：创建协议实例（工厂模式）
// ============================================================================
class ProtocolFactory {
public:
    virtual ~ProtocolFactory() = default;

    // 根据配置/握手数据创建协议实例
    virtual std::unique_ptr<Protocol> create_protocol() = 0;
};

} // namespace proxy
```

### 1.4 Connection 类设计

```cpp
// include/net/connection.h
#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <any>

namespace proxy {

// ============================================================================
// 连接类：封装一个TCP连接
// ============================================================================
class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection();

    // 禁止拷贝
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 允许移动
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // === IO操作 ===
    // 同步读取（简化版，实际应该用io_uring）
    std::int64_t read(std::uint8_t* buffer, std::size_t len);

    // 同步写入
    std::int64_t write(const std::uint8_t* buffer, std::size_t len);

    // 异步读取（返回Task，用于协程）
    // Task<std::int64_t> async_read(std::uint8_t* buffer, std::size_t len);

    // === 缓冲区访问 ===
    [[nodiscard]]
    std::vector<std::uint8_t>& read_buffer() { return read_buf_; }

    [[nodiscard]]
    std::vector<std::uint8_t>& write_buffer() { return write_buf_; }

    // === 连接属性 ===
    [[nodiscard]]
    int fd() const { return fd_; }

    [[nodiscard]]
    bool is_open() const { return fd_ >= 0; }

    void close();

    // === 上下文存储 ===
    // 用于协议存储临时数据（如握手状态）
    template<typename T>
    void set_context(T&& value) {
        context_ = std::forward<T>(value);
    }

    template<typename T>
    T& get_context() {
        return std::any_cast<T&>(context_);
    }

private:
    int fd_ = -1;
    std::vector<std::uint8_t> read_buf_;
    std::vector<std::uint8_t> write_buf_;
    std::any context_;  // 协议上下文（类型安全的void*）
};

} // namespace proxy
```

### 1.5 Server 框架设计

```cpp
// include/server/tcp_server.h
#pragma once
#include <memory>
#include <vector>
#include <functional>
#include "net/connection.h"
#include "protocol/protocol.h"

namespace proxy {

// ============================================================================
// TCP Server：负责监听、接受连接、管理连接
// 不关心具体协议，通过Protocol接口与协议交互
// ============================================================================
class TcpServer {
public:
    struct Config {
        std::string listen_host = "0.0.0.0";
        std::uint16_t listen_port = 1080;
        std::size_t max_connections = 10000;
        std::size_t worker_threads = 4;
    };

    explicit TcpServer(Config config);
    ~TcpServer();

    // 禁止拷贝和移动
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // === 协议注册 ===
    // 设置协议工厂（依赖注入）
    void set_protocol_factory(std::unique_ptr<ProtocolFactory> factory) {
        protocol_factory_ = std::move(factory);
    }

    // === 生命周期管理 ===
    bool start();
    void stop();
    [[nodiscard]] bool is_running() const { return running_; }

private:
    // === 内部工作流程 ===

    // 主线程：接受新连接
    void accept_loop();

    // Worker线程：处理已接受的连接
    void worker_loop(std::size_t worker_id);

    // 处理单个连接（协程化）
    // Task<void> handle_connection(std::unique_ptr<Connection> client_conn);
    void handle_connection(std::unique_ptr<Connection> client_conn);

    // === 成员变量 ===
    Config config_;
    int listen_fd_ = -1;
    bool running_ = false;

    // 协议工厂（依赖注入）
    std::unique_ptr<ProtocolFactory> protocol_factory_;

    // 连接管理
    std::vector<std::unique_ptr<Connection>> connections_;

    // 线程管理
    std::vector<std::thread> workers_;

    // 连接队列（主线程 → Worker线程）
    // SPSCQueue<std::unique_ptr<Connection>>* conn_queue_;
};

} // namespace proxy
```

---

## 2. 为什么选择 VLESS 作为第一个协议？

### 2.1 VLESS vs VMess 对比

| 特性 | VLESS | VMess |
|------|-------|-------|
| **设计目标** | 轻量、高性能 | 安全、功能丰富 |
| **加密** | 无（依赖TLS） | 有（内置AES-128-GCM等） |
| **认证** | UUID（简单） | 时间戳+UUID（复杂） |
| **握手** | 1-RTT | 1-RTT + 动态端口 |
| **代码复杂度** | ⭐⭐（简单） | ⭐⭐⭐⭐（复杂） |
| **性能** | 更高（无加密开销） | 较低（有加密开销） |
| **适用场景** | TLS前置（VLESS+TLS） | 独立运行 |

### 2.2 VLESS 协议格式（简化）

```
VLESS 请求格式：
┌─────────────────────────────────────────────────┐
│ 1. 认证部分 (Authentication)                     │
│    ┌──────┐                                     │
│    │ UUID │  16字节                             │
│    └──────┘                                     │
│                                                 │
│ 2. 附加数据 (Addons) - 可选                     │
│    ┌──────┬──────┬──────┐                      │
│    │Addons│  Len │ Data │                      │
│    │  Type│      │      │                      │
│    └──────┴──────┴──────┘                      │
│                                                 │
│ 3. 命令部分 (Command)                           │
│    ┌──────┬──────┬──────┬──────┐               │
│    │  Ver │  CMD │  RSV │  ATY │               │
│    │  1B  │  1B  │  1B  │  1B  │               │
│    └──────┴──────┴──────┴──────┘               │
│                                                 │
│ 4. 目标地址 (Target Address)                    │
│    ┌──────┬──────┬──────┐                      │
│    │ IPv4 │ Port │Domain│  (根据ATY决定)       │
│    │ 4B   │  2B  │  NB  │                      │
│    └──────┴──────┴──────┘                      │
│                                                 │
│ 5. 负载 (Payload)                               │
│    ┌─────────────────┐                         │
│    │   Application    │                         │
│    │   Data (加密)    │                         │
│    └─────────────────┘                         │
└─────────────────────────────────────────────────┘
```

**VLESS 的优势**：
1. **无加密**：依赖外层TLS（如VLESS+WebSocket+TLS），简化实现
2. **无状态**：每个请求独立，不需要维护会话状态
3. **UUID认证**：简单有效，易于实现
4. **代码量少**：大约200-300行即可实现基础版本

### 2.3 VLESS 实现示例（框架）

```cpp
// include/protocol/vless/vless_protocol.h
#pragma once
#include "../protocol.h"
#include <uuid/uuid.h>  // 需要安装uuid库

namespace proxy::vless {

// ============================================================================
// VLESS 协议实现
// ============================================================================
class VLESSProtocol : public Protocol {
public:
    // 构造函数：传入UUID列表（支持多用户）
    explicit VLESSProtocol(const std::vector<std::string>& uuid_list);

    // === Protocol接口实现 ===
    [[nodiscard]]
    std::string_view name() const override {
        return "VLESS";
    }

    bool handshake(Connection& conn) override;

    void encrypt(std::uint8_t* data, std::size_t len) override {
        // VLESS无加密，直接返回
        // 加密由外层TLS处理
        (void)data; (void)len;
    }

    void decrypt(std::uint8_t* data, std::size_t len) override {
        // VLESS无解密
        (void)data; (void)len;
    }

    TargetAddress parse_target(Connection& conn) override;

    void relay(Connection& client_conn, Connection& remote_conn) override;

private:
    // UUID白名单
    std::vector<std::array<std::uint8_t, 16>> allowed_uuids_;

    // 验证UUID
    bool verify_uuid(const std::uint8_t* uuid_bytes);

    // 读取UUID（16字节）
    bool read_uuid(Connection& conn, std::uint8_t* uuid_out);

    // 解析目标地址
    bool read_target_address(Connection& conn, TargetAddress& target);
};

// ============================================================================
// VLESS 协议工厂
// ============================================================================
class VLESSProtocolFactory : public ProtocolFactory {
public:
    explicit VLESSProtocolFactory(const std::vector<std::string>& uuid_list)
        : uuid_list_(uuid_list) {}

    std::unique_ptr<Protocol> create_protocol() override {
        return std::make_unique<VLESSProtocol>(uuid_list_);
    }

private:
    std::vector<std::string> uuid_list_;
};

} // namespace proxy::vless
```

```cpp
// src/protocol/vless/vless_protocol.cpp
#include "vless_protocol.h"
#include <uuid/uuid.h>
#include <cstring>
#include <arpa/inet.h>

namespace proxy::vless {

// ============================================================================
// 握手实现
// ============================================================================
bool VLESSProtocol::handshake(Connection& conn) {
    // 1. 读取UUID（16字节）
    std::uint8_t uuid_bytes[16];
    if (!read_uuid(conn, uuid_bytes)) {
        return false;
    }

    // 2. 验证UUID
    if (!verify_uuid(uuid_bytes)) {
        return false;
    }

    // 3. 读取Addons（可选，暂时跳过）
    // TODO: 实现Addons解析

    // 4. 读取命令部分
    std::uint8_t cmd_header[4];
    if (conn.read(cmd_header, 4) != 4) {
        return false;
    }

    std::uint8_t version = cmd_header[0];  // 应该是0x00
    std::uint8_t command = cmd_header[1];  // 0x01=TCP, 0x02=UDP, 0x03=Mux
    std::uint8_t rsv = cmd_header[2];      // 保留字段
    std::uint8_t atype = cmd_header[3];    // 地址类型

    if (version != 0x00) {
        return false;
    }

    // 5. 解析目标地址（存储在conn的context中）
    TargetAddress target;
    if (!read_target_address(conn, target)) {
        return false;
    }
    conn.set_context(target);

    return true;
}

bool VLESSProtocol::read_uuid(Connection& conn, std::uint8_t* uuid_out) {
    // UUID是16字节
    auto n = conn.read(uuid_out, 16);
    return n == 16;
}

bool VLESSProtocol::verify_uuid(const std::uint8_t* uuid_bytes) {
    for (const auto& allowed : allowed_uuids_) {
        if (std::memcmp(uuid_bytes, allowed.data(), 16) == 0) {
            return true;
        }
    }
    return false;
}

bool VLESSProtocol::read_target_address(Connection& conn, TargetAddress& target) {
    // 读取地址类型
    std::uint8_t atype;
    if (conn.read(&atype, 1) != 1) {
        return false;
    }

    target.type = static_cast<decltype(target.type)>(atype);

    // 根据地址类型读取地址
    if (atype == 0x01) {  // IPv4
        std::uint8_t ipv4[4];
        if (conn.read(ipv4, 4) != 4) {
            return false;
        }
        // 转换IPv4为字符串
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, ipv4, buf, sizeof(buf));
        target.host = buf;

    } else if (atype == 0x04) {  // IPv6
        std::uint8_t ipv6[16];
        if (conn.read(ipv6, 16) != 16) {
            return false;
        }
        char buf[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, ipv6, buf, sizeof(buf));
        target.host = buf;

    } else if (atype == 0x03) {  // Domain
        std::uint8_t domain_len;
        if (conn.read(&domain_len, 1) != 1) {
            return false;
        }
        std::vector<char> domain(domain_len + 1);
        if (conn.read(reinterpret_cast<std::uint8_t*>(domain.data()), domain_len) != domain_len) {
            return false;
        }
        domain[domain_len] = '\0';
        target.host = domain.data();

    } else {
        return false;  // 不支持的地址类型
    }

    // 读取端口（2字节，网络字节序）
    std::uint8_t port_bytes[2];
    if (conn.read(port_bytes, 2) != 2) {
        return false;
    }
    target.port = (port_bytes[0] << 8) | port_bytes[1];

    return true;
}

// ============================================================================
// 数据转发实现
// ============================================================================
void VLESSProtocol::relay(Connection& client_conn, Connection& remote_conn) {
    // 简化版：使用select/poll进行双向转发
    // 实际应该用io_uring + 协程

    // TODO: 实现基于io_uring的异步转发
    // co_await async_relay(client_conn, remote_conn);
}

} // namespace proxy::vless
```

---

## 3. 项目结构（更新版）

```
vmess/
├── include/
│   ├── server/                 # 服务框架（不依赖具体协议）
│   │   ├── tcp_server.h
│   │   ├── acceptor.h
│   │   └── connection.h
│   │
│   ├── net/                    # 网络层
│   │   ├── connection.h
│   │   ├── socket_utils.h
│   │   └── io_uring_context.h
│   │
│   ├── protocol/               # 协议抽象层
│   │   ├── protocol.h          # 协议接口（核心）
│   │   ├── protocol_factory.h
│   │   └── vless/              # VLESS协议实现
│   │       ├── vless_protocol.h
│   │       └── vless_protocol_factory.h
│   │
│   ├── coroutine/              # 协程支持
│   │   ├── task.h
│   │   ├── awaitable.h
│   │   └── scheduler.h
│   │
│   └── common/                 # 公共工具
│       ├── logger.h
│       ├── config.h
│       └── error.h
│
├── src/
│   ├── server/
│   │   ├── tcp_server.cpp
│   │   └── acceptor.cpp
│   │
│   ├── net/
│   │   └── connection.cpp
│   │
│   ├── protocol/
│   │   └── vless/
│   │       └── vless_protocol.cpp
│   │
│   └── main.cpp                # 程序入口
│
├── tests/
│   ├── test_vless_protocol.cpp
│   ├── test_tcp_server.cpp
│   └── test_protocol_interface.cpp
│
├── reference/                  # 参考代码（Go实现）
│   └── v2ray-core/
│
├── third_party/                # 第三方库
│   ├── googletest/
│   └── uuid/                   # UUID库
│
├── doc/
│   ├── 00-ToLearn.md
│   ├── 01-architecture.md
│   ├── 02-protocol-vless.md    # VLESS协议文档
│   ├── 03-implementation.md
│   └── 08-protocol-architecture-design.md  # 本文档
│
├── CMakeLists.txt
└── .gitignore
```

---

## 4. 设计优势总结

### 4.1 关注点分离

```
┌──────────────────────────────────────┐
│ Server Framework                    │
│ (不关心协议细节)                     │
│  ✅ 稳定、通用                       │
│  ✅ 可复用于其他项目                  │
└──────────────┬───────────────────────┘
               │
               │ 通过抽象接口交互
               │
┌──────────────▼───────────────────────┐
│ Protocol Implementations            │
│ (可插拔、独立开发)                   │
│  ✅ VLESS (轻量)                    │
│  ✅ VMess (复杂)                     │
│  ✅ Trojan (未来)                    │
│  ✅ Shadowsocks (未来)               │
└──────────────────────────────────────┘
```

### 4.2 开闭原则（Open-Closed Principle）

```cpp
// 添加新协议时，不需要修改Server代码！
// 只需要：
// 1. 实现Protocol接口
// 2. 实现ProtocolFactory
// 3. 注册到Server

// main.cpp
auto server = std::make_unique<TcpServer>(config);

// 添加VLESS协议
auto vless_factory = std::make_unique<vless::VLESSProtocolFactory>(uuid_list);
server->set_protocol_factory(std::move(vless_factory));

// 未来添加VMess协议（不需要改Server代码）
// auto vmess_factory = std::make_unique<vmess::VMessProtocolFactory>(config);
// server->set_protocol_factory(std::move(vmess_factory));

server->start();
```

### 4.3 可测试性

```cpp
// tests/test_vless_protocol.cpp
TEST(VLESSProtocolTest, HandshakeSuccess) {
    // 1. 创建Mock Connection
    MockConnection conn;

    // 2. 写入正确的UUID
    conn.set_read_data(valid_uuid_bytes);

    // 3. 测试握手
    VLESSProtocol protocol({valid_uuid_str});
    EXPECT_TRUE(protocol.handshake(conn));
}

TEST(VLESSProtocolTest, HandshakeFail_InvalidUUID) {
    // 测试无效UUID
    MockConnection conn;
    conn.set_read_data(invalid_uuid_bytes);

    VLESSProtocol protocol({valid_uuid_str});
    EXPECT_FALSE(protocol.handshake(conn));
}
```

---

## 5. 下一步建议

### 5.1 第一阶段：搭建框架（1-2天）

1. 实现 `Protocol` 接口（纯虚类）
2. 实现 `Connection` 类（基础IO）
3. 实现 `TcpServer` 框架（简化版，单线程）
4. 编写单元测试

### 5.2 第二阶段：实现VLESS协议（2-3天）

1. 实现 `VLESSProtocol`（握手、地址解析）
2. 实现简单的数据转发（用 `select` 临时实现）
3. 测试：用V2Ray客户端连接

### 5.3 第三阶段：引入io_uring和协程（3-5天）

1. 将 `Connection` 的IO操作改为异步（io_uring）
2. 实现协程调度器
3. 将 `handshake`、`relay` 改为协程版本
4. 性能测试

### 5.4 第四阶段：支持VMess协议（未来）

1. 实现 `VMessProtocol`（复杂加密、动态端口）
2. 复用相同的 `TcpServer` 框架
3. 对比VLESS和VMess的性能

---

## 6. 关键设计决策

### 6.1 同步 vs 异步协议实现

**选项A：同步实现（先简单）**
```cpp
class Protocol {
    virtual bool handshake(Connection& conn);  // 同步阻塞
};
```
- ✅ 简单、易于理解
- ❌ 性能差（每个连接占用一个线程）

**选项B：异步实现（最终目标）**
```cpp
class Protocol {
    virtual Task<bool> handshake(Connection& conn);  // 异步协程
};
```
- ✅ 高性能（少量线程处理大量连接）
- ❌ 复杂性高（需要io_uring + 协程）

**建议**：先实现同步版本（验证协议正确性），再改为异步版本（提升性能）。

### 6.2 加密策略

**VLESS**：无内置加密，依赖TLS
- 实现简单
- 需要外层TLS（如 `VLESS + WebSocket + TLS`）

**VMess**：内置加密
- 实现复杂（AES-128-GCM、Chacha20-Poly1305等）
- 可独立运行（不需要TLS）

**建议**：先实现VLESS（无加密），后续添加TLS支持（用OpenSSL或BoringSSL）。

---

## 7. 总结

本架构设计的核心思想是**关注点分离**和**依赖倒置**：

1. **Server框架**不依赖具体协议，只依赖抽象接口
2. **协议实现**可以独立开发、测试、替换
3. **新增协议**不需要修改Server代码（开闭原则）
4. **VLESS**作为第一个协议，代码简单、性能高，适合快速验证架构

这样的设计使得项目具有很好的**可扩展性**和**可维护性**，为未来支持多种代理协议打下坚实基础。
