# V2Ray 代理接口设计分析

## 1. V2Ray Proxy 接口概述

V2Ray 的代理接口设计采用了 **Inbound/Outbound 分离架构**，这是一个更加成熟和模块化的设计。

### 1.1 核心接口

```go
// Inbound: 处理入站连接（客户端 -> 代理服务器）
type Inbound interface {
    Network() []net.Network
    Process(context.Context, net.Network, internet.Connection, routing.Dispatcher) error
}

// Outbound: 处理出站连接（代理服务器 -> 目标服务器）
type Outbound interface {
    Process(context.Context, *transport.Link, internet.Dialer) error
}
```

### 1.2 设计理念

**分离关注点：**
- **Inbound**: 负责接收客户端连接，处理协议特定的握手/认证/加密
- **Outbound**: 负责连接到目标服务器，处理协议特定的封装/加密
- **Dispatcher**: 路由层，决定流量如何转发

**双向代理支持：**
- 一个代理协议可以同时有 Inbound 和 Outbound 实现
- Inbound: 服务端接收客户端连接
- Outbound: 客户端通过代理协议连接到目标

---

## 2. 为什么需要 Inbound/Outbound 分离？

### 2.1 单一接口的问题

```cpp
// 旧设计：单一接口
class Protocol {
public:
    virtual void process_client(Connection& client) = 0;  // 处理客户端
    virtual void process_remote(Connection& remote) = 0;   // 处理远端
};
```

**问题：**
- 混淆了"接收连接"和"发起连接"两种操作
- 无法支持链式代理（Proxy Chaining）
- 难以扩展新的使用场景

### 2.2 Inbound/Outbound 的优势

```
场景1：标准代理（客户端 -> 代理服务器 -> 目标）

客户端                代理服务器                目标服务器
  │                     │                       │
  │── Inbound ─────────►│                       │
  │   (接收连接)        │── Outbound ──────────►│
  │                     │   (发起连接)           │
  │                     │                       │
  │◄── Inbound ─────────│                       │
  │   (返回数据)        │◄── Outbound ──────────│
  │                     │   (接收数据)           │
```

```
场景2：链式代理（客户端 -> 代理1 -> 代理2 -> 目标）

客户端      代理服务器1        代理服务器2         目标服务器
  │            │                  │                  │
  │──Outbound─►│                  │                  │
  │            │── Outbound ────►│                  │
  │            │                  │── Outbound ────►│
  │            │                  │                  │
  │◄─Inbound──│                  │                  │
  │            │◄── Inbound ─────│                  │
  │            │                  │◄── Inbound ─────│
```

**关键洞察：**
- **Outbound** 是"如何连接到下一个节点"
- **Inbound** 是"如何接收上一个节点的连接"
- 两者可以独立实现和测试

---

## 3. V2Ray 接口详细分析

### 3.1 Inbound 接口

```go
type Inbound interface {
    // 返回支持的网络类型（tcp, udp, unix）
    Network() []net.Network
    
    // 处理入站连接
    // - ctx: 上下文（取消、超时）
    // - network: 网络类型
    // - conn: 客户端连接
    // - dispatcher: 路由分发器（用于转发到 Outbound）
    Process(context.Context, net.Network, internet.Connection, routing.Dispatcher) error
}
```

**Inbound 的职责：**
1. 接收客户端连接
2. 处理协议握手（如 SOCKS5 协商、VMess 认证）
3. 解密/解封装数据
4. 将解密后的数据通过 Dispatcher 转发到合适的 Outbound

**示例：VMess Inbound**
```go
type VMessInbound struct {
    // 配置：端口、用户列表等
}

func (in *VMessInbound) Process(ctx context.Context, network net.Network, conn internet.Connection, dispatcher routing.Dispatcher) error {
    // 1. 读取客户端握手
    header, err := ReadVMessHeader(conn)
    if err != nil {
        return err
    }
    
    // 2. 验证用户
    user, err := ValidateUser(header.UserID)
    if err != nil {
        return err
    }
    
    // 3. 解密数据
    decryptor := NewDecryptor(user, header)
    
    // 4. 解析目标地址
    target, err := ParseTarget(header)
    if err != nil {
        return err
    }
    
    // 5. 创建到目标的数据流
    link := &transport.Link{
        Reader: NewDecryptReader(conn, decryptor),
        Writer: NewEncryptWriter(conn, decryptor),
    }
    
    // 6. 通过 Dispatcher 转发到 Outbound
    return dispatcher.Dispatch(ctx, target, link)
}
```

### 3.2 Outbound 接口

```go
type Outbound interface {
    // 处理出站连接
    // - ctx: 上下文
    // - link: 数据流（Reader 读入站数据，Writer 写入站数据）
    // - dialer: 拨号器（用于建立连接）
    Process(context.Context, *transport.Link, internet.Dialer) error
}
```

**Outbound 的职责：**
1. 建立到目标服务器的连接
2. 封装/加密数据
3. 转发数据到目标
4. 接收目标返回的数据并传回 Inbound

**示例：VMess Outbound**
```go
type VMessOutbound struct {
    // 配置：服务器地址、端口、用户ID等
}

func (out *VMessOutbound) Process(ctx context.Context, link *transport.Link, dialer internet.Dialer) error {
    // 1. 连接到 VMess 服务器
    conn, err := dialer.Dial(ctx, out.serverAddress)
    if err != nil {
        return err
    }
    defer conn.Close()
    
    // 2. 发送 VMess 握手
    header := BuildVMessHeader(out.user)
    if _, err := conn.Write(header); err != nil {
        return err
    }
    
    // 3. 加密数据并发送
    encryptor := NewEncryptor(out.user, header)
    
    // 4. 从 link.Reader 读取数据，加密后写入 conn
    go func() {
        defer conn.Close()
        io.Copy(NewEncryptWriter(conn, encryptor), link.Reader)
    }()
    
    // 5. 从 conn 读取数据，解密后写入 link.Writer
    io.Copy(link.Writer, NewDecryptReader(conn, encryptor))
    
    return nil
}
```

### 3.3 transport.Link 数据结构

```go
// Link 是 Inbound 和 Outbound 之间的数据流通道
type Link struct {
    Reader Reader  // 读取入站数据
    Writer Writer  // 写入出站数据
}
```

**数据流向：**
```
Inbound                        Outbound
   │                              │
   │  link.Reader ◄──────────────│
   │                              │
   │  link.Writer ──────────────►│
   │                              │
```

---

## 4. 应用到 VMess 项目

### 4.1 新的接口设计

```cpp
// include/proxy/inbound.h
#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <span>
#include <functional>

namespace proxy {

// 网络类型
enum class NetworkType {
    TCP,
    UDP
};

// 连接抽象
class Connection {
public:
    virtual ~Connection() = default;
    
    virtual int fd() const = 0;
    virtual std::span<uint8_t> read_buffer() = 0;
    virtual void advance_read(int n) = 0;
    virtual void write(std::span<uint8_t> data) = 0;
};

// Dispatcher: 将流量分发到合适的 Outbound
class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    
    // 分发流量到目标
    virtual void dispatch(
        std::string_view target_host,
        uint16_t target_port,
        Connection& client_conn
    ) = 0;
};

// Inbound 接口
class Inbound {
public:
    virtual ~Inbound() = default;
    
    // 协议名称
    [[nodiscard]]
    virtual std::string_view name() const = 0;
    
    // 支持的网络类型
    [[nodiscard]]
    virtual std::vector<NetworkType> networks() const {
        return {NetworkType::TCP};
    }
    
    // 处理入站连接
    virtual void process(
        Connection& client_conn,
        Dispatcher& dispatcher
    ) = 0;
};

} // namespace proxy
```

```cpp
// include/proxy/outbound.h
#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <span>

namespace proxy {

// Outbound 接口
class Outbound {
public:
    virtual ~Outbound() = default;
    
    // 协议名称
    [[nodiscard]]
    virtual std::string_view name() const = 0;
    
    // 处理出站连接
    virtual void process(
        std::string_view target_host,
        uint16_t target_port,
        Connection& client_conn
    ) = 0;
};

} // namespace proxy
```

### 4.2 VMess Inbound 实现

```cpp
// include/proxy/vmess/inbound.h
#pragma once
#include "../inbound.h"
#include <memory>

namespace proxy {

class VMessInbound : public Inbound {
public:
    VMessInbound(std::string user_id, std::string password)
        : user_id_(std::move(user_id))
        , password_(std::move(password))
    {}
    
    std::string_view name() const override {
        return "vmess-inbound";
    }
    
    void process(Connection& client_conn, Dispatcher& dispatcher) override {
        // 1. 读取并验证 VMess 握手
        auto header = read_vmess_header(client_conn);
        if (!validate_user(header)) {
            throw std::runtime_error("Invalid user");
        }
        
        // 2. 解析目标地址
        auto [target_host, target_port] = parse_target(header);
        
        // 3. 创建解密器
        auto decryptor = create_decryptor(header);
        
        // 4. 分发到 Outbound
        dispatcher.dispatch(target_host, target_port, client_conn);
    }
    
private:
    std::string user_id_;
    std::string password_;
    
    VMessHeader read_vmess_header(Connection& conn);
    bool validate_user(const VMessHeader& header);
    std::pair<std::string, uint16_t> parse_target(const VMessHeader& header);
    std::unique_ptr<Decryptor> create_decryptor(const VMessHeader& header);
};

} // namespace proxy
```

### 4.3 VMess Outbound 实现

```cpp
// include/proxy/vmess/outbound.h
#pragma once
#include "../outbound.h"
#include <memory>

namespace proxy {

class VMessOutbound : public Outbound {
public:
    VMessOutbound(std::string server_host, uint16_t server_port, std::string user_id)
        : server_host_(std::move(server_host))
        , server_port_(server_port)
        , user_id_(std::move(user_id))
    {}
    
    std::string_view name() const override {
        return "vmess-outbound";
    }
    
    void process(
        std::string_view target_host,
        uint16_t target_port,
        Connection& client_conn
    ) override {
        // 1. 连接到 VMess 服务器
        auto server_conn = connect_to_server();
        
        // 2. 构建并发送 VMess 握手
        auto header = build_vmess_header(target_host, target_port);
        server_conn->write(header);
        
        // 3. 创建加密器
        auto encryptor = create_encryptor(header);
        
        // 4. 启动双向数据转发
        start_relay(client_conn, *server_conn, *encryptor);
    }
    
private:
    std::string server_host_;
    uint16_t server_port_;
    std::string user_id_;
    
    std::unique_ptr<Connection> connect_to_server();
    VMessHeader build_vmess_header(std::string_view host, uint16_t port);
    std::unique_ptr<Encryptor> create_encryptor(const VMessHeader& header);
    void start_relay(Connection& client, Connection& server, Encryptor& encryptor);
};

} // namespace proxy
```

### 4.4 SOCKS5 Inbound 实现

```cpp
// include/proxy/socks5/inbound.h
#pragma once
#include "../inbound.h"

namespace proxy {

class Socks5Inbound : public Inbound {
public:
    Socks5Inbound() {}
    
    std::string_view name() const override {
        return "socks5-inbound";
    }
    
    void process(Connection& client_conn, Dispatcher& dispatcher) override {
        // 1. 方法协商
        auto methods = read_methods(client_conn);
        if (methods.empty()) {
            throw std::runtime_error("No supported methods");
        }
        send_method_selection(0x00);  // 选择无认证
        
        // 2. 处理请求
        auto request = read_request(client_conn);
        if (request.cmd != 0x01) {  // 只支持 CONNECT
            send_reply(0x07);  // 不支持的命令
            throw std::runtime_error("Unsupported command");
        }
        
        // 3. 回复成功
        send_reply(0x00);
        
        // 4. 分发到 Outbound
        dispatcher.dispatch(request.host, request.port, client_conn);
    }
    
private:
    struct Request {
        uint8_t cmd;
        std::string host;
        uint16_t port;
    };
    
    std::vector<uint8_t> read_methods(Connection& conn);
    void send_method_selection(uint8_t method);
    Request read_request(Connection& conn);
    void send_reply(uint8_t status);
};

} // namespace proxy
```

---

## 5. Dispatcher 实现

### 5.1 简单 Dispatcher

```cpp
// include/proxy/dispatcher.h
#pragma once
#include "inbound.h"
#include "outbound.h"
#include <unordered_map>
#include <memory>

namespace proxy {

// 简单 Dispatcher 实现
class SimpleDispatcher : public Dispatcher {
public:
    void register_outbound(std::string name, std::unique_ptr<Outbound> outbound) {
        outbounds_[name] = std::move(outbound);
    }
    
    void dispatch(
        std::string_view target_host,
        uint16_t target_port,
        Connection& client_conn
    ) override {
        // 简单策略：使用第一个可用的 Outbound
        // 实际应该根据路由规则选择
        
        if (outbounds_.empty()) {
            throw std::runtime_error("No outbound available");
        }
        
        auto& outbound = *outbounds_.begin()->second;
        outbound.process(target_host, target_port, client_conn);
    }
    
private:
    std::unordered_map<std::string, std::unique_ptr<Outbound>> outbounds_;
};

} // namespace proxy
```

### 5.2 使用场景

```cpp
#include "proxy/vmess/inbound.h"
#include "proxy/vmess/outbound.h"
#include "proxy/socks5/inbound.h"
#include "proxy/dispatcher.h"

int main() {
    // 创建 Dispatcher
    SimpleDispatcher dispatcher;
    
    // 注册 Outbound（可以注册多个，根据路由规则选择）
    dispatcher.register_outbound(
        "vmess",
        std::make_unique<VMessOutbound>("server.com", 443, "user-id-123")
    );
    
    // 创建 Inbound
    VMessInbound vmess_inbound("user-id-123", "password");
    Socks5Inbound socks5_inbound;
    
    // 处理连接
    // 当有新的客户端连接时：
    // vmess_inbound.process(client_conn, dispatcher);
    
    return 0;
}
```

---

## 6. 设计对比

### 6.1 三种设计对比

| 设计 | 接口 | 优点 | 缺点 | 适用场景 |
|------|------|------|------|---------|
| **v1（初始）** | 单一 Protocol 接口 | 简单直观 | 不通用，假设太多 | 快速原型 |
| **v2（数据驱动）** | on_client_data, on_remote_data | 通用灵活 | 需要协议自己管理状态 | 简单代理 |
| **v3（Inbound/Outbound）** | Inbound + Outbound | 模块化，支持链式代理 | 接口较多，复杂度高 | 生产级代理 |

### 6.2 推荐方案

**对于 VMess 项目：**

```
阶段1：快速原型
  └─ 使用 v2 设计（数据驱动）
  └─ 快速验证协议实现

阶段2：生产就绪
  └─ 迁移到 v3 设计（Inbound/Outbound）
  └─ 支持路由、链式代理等高级功能
```

---

## 7. 总结

### 7.1 V2Ray 设计的精髓

1. **关注点分离**：Inbound 和 Outbound 独立实现
2. **双向支持**：一个协议可以同时是 Inbound 和 Outbound
3. **路由集成**：通过 Dispatcher 实现灵活的流量路由
4. **链式代理**：支持多级代理串联

### 7.2 应用到 VMess

**核心改变：**
```cpp
// 旧设计：单一接口
class Protocol {
    virtual void process(Connection& client, Connection& remote) = 0;
};

// 新设计：分离接口
class Inbound {
    virtual void process(Connection& client, Dispatcher& dispatcher) = 0;
};

class Outbound {
    virtual void process(std::string_view target, Connection& client) = 0;
};
```

**优势：**
- ✅ 支持链式代理
- ✅ 支持复杂的路由规则
- ✅ 协议实现更加模块化
- ✅ 易于测试和扩展

### 7.3 下一步

1. 实现 Inbound/Outbound 接口
2. 实现 VMess Inbound
3. 实现 VMess Outbound
4. 实现 Dispatcher
5. 集成到 Connection 处理流程
