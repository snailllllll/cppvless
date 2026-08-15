> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：Protocol 抽象接口不存在；SOCKS5 格式章节仍可参考（见 README 索引）
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# 协议设计与 SOCKS5 详解

## 1. 问题背景

### 1.1 初始协议设计的问题

最初的协议抽象设计存在以下问题：

```cpp
// 初始设计（存在问题）
class Protocol {
public:
    virtual bool handshake(Connection& conn) = 0;      // ❌ 假设握手是单步操作
    virtual void encrypt(uint8_t* data, size_t len) = 0;  // ❌ 假设所有协议都加密
    virtual void decrypt(uint8_t* data, size_t len) = 0;  // ❌ 同上
    virtual void relay(Connection& client, Connection& remote) = 0;  // ❌ 假设转发模型
};
```

**问题汇总：**

| 问题 | 说明 | 反例 |
|------|------|------|
| **假设有加密** | 不是所有协议都需要加密 | SOCKS5 不加密 |
| **假设单步握手** | 现实中的协议可能有多个协商阶段 | SOCKS5 有协商、认证、请求三阶段 |
| **假设转发模型** | 不同协议的数据流处理方式不同 | HTTP 代理需要解析头部，SOCKS5 透明转发 |
| **没有状态管理** | 协议可能需要维护状态 | VMess 有认证状态、连接状态 |

---

## 2. SOCKS5 协议详解（RFC 1928）

### 2.1 协议概述

SOCKS5 是一个网络代理协议，用于在客户端和服务器之间转发网络流量。

**核心功能：**
- 支持 TCP 和 UDP 代理
- 支持多种认证方法
- 支持 IPv4、IPv6 和域名

**RFC 文档：**
- RFC 1928: SOCKS Protocol Version 5
- RFC 1929: Username/Password Authentication for SOCKS V5

### 2.2 协议流程

```
客户端 ──── 服务端

1. 方法协商
   ──(1)──> 支持的方法列表 [0x05, nmethods, methods...]
   <──(2)── 选择的方法     [0x05, method]
   
2. 认证（如果需要）
   ──(3)──> 认证数据
   <──(4)── 认证结果
   
3. 请求
   ──(5)──> 请求详情 [0x05, cmd, rsv, atyp, dst.addr, dst.port]
   <──(6)── 回复     [0x05, rep, rsv, atyp, bnd.addr, bnd.port]
   
4. 数据转发
   ──(7)──> 应用数据
   <──(8)── 应用数据
```

### 2.3 详细格式

#### 1. 方法协商请求

```
+----+----------+----------+
|VER | NMETHODS | METHODS  |
+----+----------+----------+
| 1  |    1     | 1 to 255 |
+----+----------+----------+
```

- **VER**: 协议版本，固定为 `0x05`
- **NMETHODS**: 客户端支持的方法数量
- **METHODS**: 支持的方法列表
  - `0x00`: 无认证
  - `0x02`: 用户名/密码认证（RFC 1929）

**示例：**
```
0x05, 0x02, 0x00, 0x02  # 支持无认证和用户名/密码认证
```

#### 2. 方法协商回复

```
+----+--------+
|VER | METHOD |
+----+--------+
| 1  |   1    |
+----+--------+
```

**示例：**
```
0x05, 0x00  # 选择无认证
0x05, 0x02  # 选择用户名/密码认证
0x05, 0xFF  # 无支持的方法（连接关闭）
```

#### 3. 请求格式

```
+----+-----+-------+------+----------+----------+
|VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
+----+-----+-------+------+----------+----------+
| 1  |  1  | X'00'|  1   | Variable |    2     |
+----+-----+-------+------+----------+----------+
```

- **CMD**: 命令类型
  - `0x01`: CONNECT（建立 TCP 连接）
  - `0x02`: BIND（绑定端口，用于 FTP 等）
  - `0x03`: UDP ASSOCIATE（UDP 关联）
  
- **RSV**: 保留字段，固定为 `0x00`

- **ATYP**: 地址类型
  - `0x01`: IPv4 地址（4 字节）
  - `0x03`: 域名（第一个字节是长度）
  - `0x04`: IPv6 地址（16 字节）

- **DST.ADDR**: 目标地址
- **DST.PORT**: 目标端口（网络字节序）

**示例（IPv4）：**
```
0x05, 0x01, 0x00, 0x01, 0x7F, 0x00, 0x00, 0x01, 0x00, 0x50
# CONNECT 到 127.0.0.1:80
```

**示例（域名）：**
```
0x05, 0x01, 0x00, 0x03, 0x0B, 'e','x','a','m','p','l','e','.','c','o','m', 0x00, 0x50
# CONNECT 到 example.com:80
```

#### 4. 回复格式

```
+----+-----+-------+------+----------+----------+
|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
+----+-----+-------+------+----------+----------+
| 1  |  1  | X'00'|  1   | Variable |    2     |
+----+-----+-------+------+----------+----------+
```

- **REP**: 回复状态码
  - `0x00`: 成功
  - `0x01`: 一般性失败
  - `0x02`: 规则不允许连接
  - `0x03`: 网络不可达
  - `0x04`: 主机不可达
  - `0x05`: 连接被拒绝
  - `0x06`: TTL 超时
  - `0x07`: 不支持的命令
  - `0x08`: 不支持的地址类型

**示例（成功）：**
```
0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
# 成功，绑定到 0.0.0.0:0
```

### 2.4 认证子协议（RFC 1929）

如果协商选择 `0x02`（用户名/密码认证），则进行以下流程：

**认证请求：**
```
+----+------+----------+------+----------+
|VER | ULEN |  UNAME   | PLEN |  PASSWD  |
+----+------+----------+------+----------+
| 1  |  1   | 1 to 255|  1   | 1 to 255 |
+----+------+----------+------+----------+
```

**认证回复：**
```
+----+--------+
|VER | STATUS |
+----+--------+
| 1  |   1    |
+----+--------+
```

- **STATUS**: 
  - `0x00`: 成功
  - 其他: 失败（连接关闭）

---

## 3. 重新设计：通用协议抽象

### 3.1 设计原则

**核心思想：将复杂性留给协议实现，接口保持简单。**

```cpp
// include/protocol/protocol.h
#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace proxy {

// ============================================================================
// 协议接口：所有代理协议必须实现此接口
// ============================================================================
class Protocol {
public:
    virtual ~Protocol() = default;
    
    // 协议标识
    [[nodiscard]]
    virtual std::string_view name() const = 0;
    
    // ========================================================================
    // 核心接口：处理数据
    // ========================================================================
    
    // 处理从客户端收到的数据
    // 返回需要发送给客户端的数据（可能为空）
    virtual std::vector<uint8_t> on_client_data(std::span<uint8_t> data) = 0;
    
    // 处理从远端收到的数据
    // 返回需要发送给远端的数据（可能为空）
    virtual std::vector<uint8_t> on_remote_data(std::span<uint8_t> data) = 0;
    
    // ========================================================================
    // 可选接口：协议元数据
    // ========================================================================
    
    // 是否需要建立到远端的连接
    [[nodiscard]]
    virtual bool needs_remote_connection() const { return true; }
    
    // 获取目标地址（在握手完成后调用）
    [[nodiscard]]
    virtual std::optional<std::pair<std::string, uint16_t>> target() const {
        return std::nullopt;
    }
    
    // 协议是否已完成握手
    [[nodiscard]]
    virtual bool is_handshake_complete() const { return false; }
};

} // namespace proxy
```

### 3.2 设计优势

| 特性 | 说明 | 示例 |
|------|------|------|
| **简单** | 只有两个核心方法 | `on_client_data`, `on_remote_data` |
| **通用** | 适用于任何代理协议 | SOCKS5、VMess、HTTP 都可以实现 |
| **灵活** | 协议自己管理状态 | SOCKS5 可以维护协商、认证、请求等状态 |
| **可扩展** | 可选接口提供额外功能 | `target()`, `needs_remote_connection()` |

---

## 4. SOCKS5 协议实现

### 4.1 完整实现

```cpp
// include/protocol/socks5.h
#pragma once
#include "protocol.h"
#include <array>
#include <string>

namespace proxy {

// SOCKS5 协议实现
class Socks5Protocol : public Protocol {
public:
    Socks5Protocol() : state_(State::NEGOTIATION) {}
    
    std::string_view name() const override {
        return "socks5";
    }
    
    // 处理客户端数据
    std::vector<uint8_t> on_client_data(std::span<uint8_t> data) override {
        switch (state_) {
            case State::NEGOTIATION:
                return handle_negotiation(data);
            case State::AUTHENTICATION:
                return handle_authentication(data);
            case State::REQUEST:
                return handle_request(data);
            case State::FORWARDING:
                // 转发阶段：直接返回数据
                return std::vector<uint8_t>(data.begin(), data.end());
            default:
                return {};  // 错误状态
        }
    }
    
    // 处理远端数据
    std::vector<uint8_t> on_remote_data(std::span<uint8_t> data) override {
        // SOCKS5 不修改远端数据，直接返回
        return std::vector<uint8_t>(data.begin(), data.end());
    }
    
    bool needs_remote_connection() const override {
        return state_ == State::REQUEST_DONE;
    }
    
    std::optional<std::pair<std::string, uint16_t>> target() const override {
        if (state_ == State::REQUEST_DONE || state_ == State::FORWARDING) {
            return std::make_pair(target_host_, target_port_);
        }
        return std::nullopt;
    }
    
    bool is_handshake_complete() const override {
        return state_ == State::FORWARDING;
    }
    
private:
    // 协议状态
    enum class State {
        NEGOTIATION,       // 方法协商
        AUTHENTICATION,    // 认证
        REQUEST,           // 请求
        REQUEST_DONE,      // 请求完成
        FORWARDING         // 转发
    };
    
    State state_;
    std::string target_host_;
    uint16_t target_port_;
    
    // 处理方法协商
    std::vector<uint8_t> handle_negotiation(std::span<uint8_t> data) {
        // 检查数据是否足够
        if (data.size() < 2) {
            return {};  // 需要更多数据
        }
        
        uint8_t ver = data[0];
        uint8_t nmethods = data[1];
        
        if (ver != 0x05 || data.size() < 2 + nmethods) {
            // 协议错误
            return {0x05, 0xFF};
        }
        
        // 检查支持的方法（这里简单选择 0x00：无认证）
        bool no_auth_supported = false;
        for (int i = 0; i < nmethods; i++) {
            if (data[2 + i] == 0x00) {
                no_auth_supported = true;
                break;
            }
        }
        
        if (!no_auth_supported) {
            state_ = State::AUTHENTICATION;
            return {0x05, 0x02};  // 选择用户名/密码认证
        }
        
        // 选择无认证
        state_ = State::REQUEST;
        return {0x05, 0x00};
    }
    
    // 处理认证（简化版，假设无认证）
    std::vector<uint8_t> handle_authentication(std::span<uint8_t> data) {
        // 如果方法是 0x00，跳过认证
        state_ = State::REQUEST;
        return {};
    }
    
    // 处理请求
    std::vector<uint8_t> handle_request(std::span<uint8_t> data) {
        // 检查头部是否完整
        if (data.size() < 4) {
            return {};  // 需要更多数据
        }
        
        uint8_t ver = data[0];
        uint8_t cmd = data[1];
        uint8_t atyp = data[3];
        
        if (ver != 0x05 || cmd != 0x01) {
            // 不支持的命令
            return build_error_reply(0x07);
        }
        
        // 解析目标地址
        size_t addr_len = 0;
        if (atyp == 0x01) {  // IPv4
            addr_len = 4;
        } else if (atyp == 0x03) {  // 域名
            if (data.size() < 5) return {};
            addr_len = data[4] + 1;
        } else if (atyp == 0x04) {  // IPv6
            addr_len = 16;
        } else {
            return build_error_reply(0x08);  // 不支持的地址类型
        }
        
        size_t total_len = 4 + addr_len + 2;
        if (data.size() < total_len) {
            return {};  // 需要更多数据
        }
        
        // 提取目标地址和端口
        if (atyp == 0x03) {
            target_host_ = std::string(data.begin() + 5, data.begin() + 5 + data[4]);
        } else if (atyp == 0x01) {
            // IPv4
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &data[4], ip, sizeof(ip));
            target_host_ = ip;
        }
        
        target_port_ = (data[total_len - 2] << 8) | data[total_len - 1];
        
        // 状态转换
        state_ = State::REQUEST_DONE;
        
        // 返回成功回复
        return build_success_reply();
    }
    
    std::vector<uint8_t> build_success_reply() {
        // 简化版：返回 0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        return {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
    
    std::vector<uint8_t> build_error_reply(uint8_t error_code) {
        return {0x05, error_code, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
};

} // namespace proxy
```

### 4.2 使用示例

```cpp
#include "protocol/socks5.h"
#include <iostream>

int main() {
    Socks5Protocol protocol;
    
    // 1. 方法协商
    std::vector<uint8_t> negotiation_req = {0x05, 0x01, 0x00};
    auto reply = protocol.on_client_data(negotiation_req);
    std::cout << "Negotiation reply: ";
    for (auto b : reply) std::cout << std::hex << (int)b << " ";
    std::cout << std::endl;
    
    // 2. 请求
    std::vector<uint8_t> request = {
        0x05, 0x01, 0x00, 0x01,  // VER, CMD, RSV, ATYP
        0x7F, 0x00, 0x00, 0x01,  // 127.0.0.1
        0x00, 0x50                 // Port 80
    };
    reply = protocol.on_client_data(request);
    std::cout << "Request reply: ";
    for (auto b : reply) std::cout << std::hex << (int)b << " ";
    std::cout << std::endl;
    
    // 3. 获取目标地址
    auto target = protocol.target();
    if (target) {
        std::cout << "Target: " << target->first << ":" << target->second << std::endl;
    }
    
    // 4. 转发数据
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    auto forwarded = protocol.on_client_data(data);
    std::cout << "Forwarded data size: " << forwarded.size() << std::endl;
    
    return 0;
}
```

---

## 5. VMess 协议实现（对比）

```cpp
// include/protocol/vmess.h
#pragma once
#include "protocol.h"

namespace proxy {

class VmessProtocol : public Protocol {
public:
    VmessProtocol() : state_(State::HANDSHAKE) {}
    
    std::string_view name() const override {
        return "vmess";
    }
    
    std::vector<uint8_t> on_client_data(std::span<uint8_t> data) override {
        switch (state_) {
            case State::HANDSHAKE:
                return handle_handshake(data);
            case State::DATA:
                return handle_data(data);
            default:
                return {};
        }
    }
    
    std::vector<uint8_t> on_remote_data(std::span<uint8_t> data) override {
        // VMess：需要加密后再发给客户端
        return encrypt_data(data);
    }
    
    bool needs_remote_connection() const override {
        return state_ == State::DATA && !target_host_.empty();
    }
    
    std::optional<std::pair<std::string, uint16_t>> target() const override {
        if (!target_host_.empty()) {
            return std::make_pair(target_host_, target_port_);
        }
        return std::nullopt;
    }
    
    bool is_handshake_complete() const override {
        return state_ == State::DATA;
    }
    
private:
    enum class State {
        HANDSHAKE,
        DATA
    };
    
    State state_;
    std::string target_host_;
    uint16_t target_port_;
    
    std::vector<uint8_t> handle_handshake(std::span<uint8_t> data) {
        // VMess 握手逻辑
        // ...
        state_ = State::DATA;
        return {};  // 握手阶段不需要返回数据
    }
    
    std::vector<uint8_t> handle_data(std::span<uint8_t> data) {
        // 解密数据
        auto decrypted = decrypt_data(data);
        
        // 解析目标地址（从解密后的数据中提取）
        auto [host, port] = parse_target(decrypted);
        target_host_ = host;
        target_port_ = port;
        
        // 返回需要转发到远端的数据（提取 payload）
        return extract_payload(decrypted);
    }
    
    std::vector<uint8_t> decrypt_data(std::span<uint8_t> data) {
        // VMess 解密逻辑
        // ...
        return {};
    }
    
    std::vector<uint8_t> encrypt_data(std::span<uint8_t> data) {
        // VMess 加密逻辑
        // ...
        return {};
    }
    
    std::pair<std::string, uint16_t> parse_target(std::span<uint8_t> data) {
        // 从 VMess 数据中提取目标地址
        // ...
        return {"example.com", 80};
    }
    
    std::vector<uint8_t> extract_payload(std::span<uint8_t> data) {
        // 从 VMess 数据中提取有效负载
        // ...
        return {};
    }
};

} // namespace proxy
```

---

## 6. 总结

### 6.1 设计演进

| 版本 | 设计 | 优点 | 缺点 |
|------|------|------|------|
| **v1（初始）** | 具体方法（handshake, encrypt, relay） | 直观 | 不通用，假设太多 |
| **v2（新设计）** | 数据驱动（on_client_data, on_remote_data） | 通用、灵活、简单 | 需要协议自己管理状态 |

### 6.2 新设计的优势

**对于 SOCKS5：**
- ✅ 可以轻松实现多阶段握手
- ✅ 不需要实现不需要的方法（如 encrypt）
- ✅ 协议自己管理状态

**对于 VMess：**
- ✅ 可以在 `on_client_data` 中处理握手和数据
- ✅ 可以在 `on_remote_data` 中加密数据
- ✅ 灵活的状态管理

**对于未来协议：**
- ✅ 任何代理协议都可以实现这个接口
- ✅ 不需要修改接口代码

### 6.3 下一步

1. 实现新的 `Protocol` 接口
2. 实现 SOCKS5 协议
3. 实现 VMess 协议
4. 在 `Connection` 中集成协议处理
