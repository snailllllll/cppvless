> **状态：已归档（历史设计）**
> 归档日期：2026-08-15
> 原因：早期模块蓝图（Config/Session/Scheduler 等组件当前并不存在）
> 本文档描述项目早期设计，与当前实现不符，仅作历史参考。
> 当前实现请读：doc/README.md（索引）+ doc/19-current-architecture.md（架构速览）。

# 代码架构设计

> 版本：v1.0 DRAFT
> 整理时间：2026-04-28
> 内容：从代码角度设计模块划分、职责、接口，以及实现路线图

---

## 1. 模块划分总览

### 1.1 模块依赖关系图

```
┌──────────────────────────────────────────────────────────────┐
│                      VMess Server                           │
│                                                              │
│  ┌────────────┐     ┌────────────┐     ┌────────────┐   │
│  │   Main     │────▶│  Server    │────▶│  Worker    │   │
│  │  Thread    │     │  Module    │◀────│  Module    │   │
│  └────────────┘     └────────────┘     └────────────┘   │
│         │                   │                   │            │
│         ▼                   ▼                   ▼            │
│  ┌────────────┐     ┌────────────┐     ┌────────────┐   │
│  │  Config    │     │  Coroutine │     │  IO        │   │
│  │  Module    │     │  Module    │     │  Module    │   │
│  └────────────┘     └────────────┘     └────────────┘   │
│         │                   │                   │            │
│         ▼                   ▼                   ▼            │
│  ┌────────────┐     ┌────────────┐     ┌────────────┐   │
│  │  Logger    │     │  Protocol  │     │  Scheduler │   │
│  │  Module    │     │  Module    │     │  Module    │   │
│  └────────────┘     └────────────┘     └────────────┘   │
│                             │                                │
│                             ▼                                │
│                    ┌────────────┐     ┌────────────┐       │
│                    │  Crypto   │     │  Session   │       │
│                    │  Module   │     │  Manager   │       │
│                    └────────────┘     └────────────┘       │
│                             │               │                │
│                             ▼               ▼                │
│                    ┌────────────┐     ┌────────────┐       │
│                    │  User      │     │  Relay     │       │
│                    │  Manager   │     │  Module    │       │
│                    └────────────┘     └────────────┘       │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 模块列表

| 模块名          | 职责                                      | 依赖模块                   |
|-----------------|-------------------------------------------|----------------------------|
| Config          | 加载和解析配置文件                        | -                          |
| Logger          | 日志记录                                  | Config                     |
| User Manager    | 管理用户（UUID 校验等）                   | Config, Logger             |
| Session Manager | 管理会话（防重放）                       | Logger                     |
| Crypto          | 加密/解密                                 | Logger                     |
| Protocol        | VMess 协议解析                            | Crypto, Logger             |
| IO              | 异步 IO 操作（io_uring 封装）             | Logger                     |
| Coroutine       | 协程支持（Task<> 等）                    | IO                         |
| Scheduler       | 协程调度器                                | Coroutine, IO              |
| Relay           | 数据转发                                  | Protocol, Crypto, IO       |
| Worker          | Worker 线程实现                           | Scheduler, Relay, IO      |
| Server          | 服务端主逻辑                              | Worker, Config, Logger    |
| Main Thread     | 主线程（接受连接、负载均衡）              | Server, Logger            |

---

## 2. 各模块详细设计

### 2.1 Config Module

#### 职责

- 从 YAML 文件加载配置
- 提供配置访问接口
- 验证配置合法性

#### 暴露接口

```cpp
// 配置结构
struct ServerConfig {
    std::string listen_addr;
    uint16_t listen_port;
    size_t max_connections;
    size_t io_uring_queue_depth;
    bool enforce_aead;
    size_t num_workers;  // 新增：Worker 线程数
};

struct UserConfig {
    std::string uuid;
    std::string email;
    std::string security;  // "auto", "aes-128-gcm", "chacha20-poly1305"
};

struct LogConfig {
    std::string level;  // "debug", "info", "warn", "error"
    std::string file;
};

struct RelayConfig {
    bool enabled;
    size_t connect_timeout;
    size_t read_timeout;
};

// Config 类
class Config {
public:
    // 从文件加载配置
    static std::expected<Config, ConfigError> load_from_file(const std::string& path);
    
    // 获取配置
    const ServerConfig& server() const { return server_; }
    const std::vector<UserConfig>& users() const { return users_; }
    const LogConfig& logging() const { return logging_; }
    const RelayConfig& relay() const { return relay_; }
    
    // 验证配置
    std::expected<void, ConfigError> validate() const;
    
private:
    ServerConfig server_;
    std::vector<UserConfig> users_;
    LogConfig logging_;
    RelayConfig relay_;
};
```

#### 内部设计

- 使用 YAML 库（如 yaml-cpp）解析配置文件
- 提供默认值
- 错误处理使用 `std::expected`

---

### 2.2 Logger Module

#### 职责

- 提供日志记录功能
- 支持不同日志级别
- 支持输出到文件和控制台

#### 暴露接口

```cpp
// 日志级别
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

// Logger 类
class Logger {
public:
    // 初始化
    static void init(const LogConfig& config);
    
    // 日志输出
    template<typename... Args>
    static void debug(fmt::format_string<Args...> fmt, Args&&... args);
    
    template<typename... Args>
    static void info(fmt::format_string<Args...> fmt, Args&&... args);
    
    template<typename... Args>
    static void warn(fmt::format_string<Args...> fmt, Args&&... args);
    
    template<typename... Args>
    static void error(fmt::format_string<Args...> fmt, Args&&... args);
    
private:
    static LogLevel level_;
    static std::ofstream file_;
    static std::mutex mutex_;  // 线程安全
};
```

#### 内部设计

- 使用 fmt 库进行格式化
- 线程安全（使用互斥锁）
- 支持异步日志（可选）

---

### 2.3 User Manager Module

#### 职责

- 管理用户列表
- 根据 UUID 查找用户
- 根据 Auth ID 查找用户（AEAD 模式）

#### 暴露接口

```cpp
// 用户结构
struct User {
    uuid_t uuid;
    std::string email;
    SecurityType security;
    std::array<uint8_t, 16> cmd_key;  // AEAD CmdKey
};

// UserManager 类
class UserManager {
public:
    // 从配置加载用户
    bool load_from_config(const std::vector<UserConfig>& config);
    
    // 根据 UUID 查找用户
    std::optional<std::reference_wrapper<const User>> 
    find_user(const uuid_t& uuid) const;
    
    // 根据 Auth ID 查找用户（AEAD 模式）
    std::optional<std::reference_wrapper<const User>> 
    find_user_by_auth_id(const std::array<uint8_t, 16>& auth_id) const;
    
private:
    std::unordered_map<std::string, User> users_;
    std::unordered_map<std::string, std::string> auth_id_index_;  // auth_id → uuid
};
```

#### 内部设计

- 使用哈希表存储用户
- 为 AEAD 模式建立 Auth ID 索引
- 线程安全（只读，不需要锁）

---

### 2.4 Session Manager Module

#### 职责

- 管理会话 ID
- 防重放攻击
- 定期清理过期会话

#### 暴露接口

```cpp
// 会话 ID 结构
struct SessionId {
    uuid_t user_uuid;
    int64_t timestamp;
    std::array<uint8_t, 16> random_iv;
    
    bool operator==(const SessionId&) const = default;
};

// SessionManager 类
class SessionManager {
public:
    // 添加会话（如果不存在则返回 true）
    bool add_if_not_exists(const SessionId& id);
    
    // 清理过期会话
    void cleanup_expired();
    
    // 启动定期清理任务（协程版本）
    Task<void> cleanup_loop();
    
private:
    // 会话 ID 存储
    std::unordered_set<std::string> sessions_;
    
    // 时间戳 → 会话 ID 列表（用于高效清理）
    std::multimap<int64_t, std::string> expiry_index_;
    
    // 清理间隔：30 秒
    // 有效期：3 分钟
    static constexpr auto EXPIRY_DURATION = std::chrono::minutes(3);
    static constexpr auto CLEANUP_INTERVAL = std::chrono::seconds(30);
};
```

#### 内部设计

- 使用哈希表存储会话 ID
- 使用多映射存储时间戳索引，便于清理
- 需要锁保护（因为会并发访问）

---

### 2.5 Crypto Module

#### 职责

- 提供加密/解密功能
- 支持多种加密算法（AES-128-GCM、ChaCha20-Poly1305 等）
- 计算 FNV 哈希

#### 暴露接口

```cpp
// 加密上下文
struct CryptoContext {
    SecurityType security;
    std::array<uint8_t, 16> iv;
    std::array<uint8_t, 16> key;
    
    // AEAD 相关
    std::array<uint8_t, 12> nonce;  // AES-GCM/ChaCha20 nonce
};

// Crypto 类
class Crypto {
public:
    // AEAD 加密
    static std::vector<uint8_t> seal_aead(
        std::span<const uint8_t> plaintext,
        const CryptoContext& ctx);
    
    // AEAD 解密
    static std::expected<std::vector<uint8_t>, CryptoError>
    open_aead(std::span<const uint8_t> ciphertext,
              const CryptoContext& ctx);
    
    // 计算 FNV 哈希
    static uint32_t fnv_hash(std::span<const uint8_t> data);
    
    // 根据类型选择加密方法
    static SecurityType select_security();
    
private:
    // 具体的加密算法实现
    static std::vector<uint8_t> aes_128_gcm_encrypt(...);
    static std::vector<uint8_t> chacha20_poly1305_encrypt(...);
};
```

#### 内部设计

- 使用 OpenSSL 或 BoringSSL 库
- 提供统一的加密/解密接口
- 错误处理使用 `std::expected`

---

### 2.6 Protocol Module

#### 职责

- 解析 VMess 请求头
- 编码 VMess 响应头
- 处理 VMess 协议细节

#### 暴露接口

```cpp
// VMess 请求头结构
struct VmessRequest {
    uint8_t version;
    std::array<uint8_t, 16> body_iv;
    std::array<uint8_t, 16> body_key;
    uint8_t response_cmd;
    SecurityType security;
    uint8_t option;
    CommandType command;
    Address target_addr;
    uint16_t target_port;
    std::vector<uint8_t> padding;
    uint32_t fnv_hash;
};

// VMess 响应头结构
struct VmessResponse {
    uint8_t version;
    uint8_t response_cmd;
    uint8_t option;
    uint8_t reserved;
    uint32_t fnv_hash;
};

// VmessParser 类
class VmessParser {
public:
    // 解析请求头（AEAD 模式）
    static std::expected<VmessRequest, VmessError>
    parse_request_aead(std::span<const uint8_t> buf,
                      const UserManager& users);
    
    // 解析请求头（遗留模式）
    static std::expected<VmessRequest, VmessError>
    parse_request_legacy(std::span<const uint8_t> buf,
                         const UserManager& users);
    
    // 编码响应头
    static std::vector<uint8_t> encode_response(
        const VmessResponse& resp,
        const CryptoContext& ctx);
    
private:
    // 具体的解析实现
    static VmessRequest parse_aead_impl(...);
    static VmessRequest parse_legacy_impl(...);
};
```

#### 内部设计

- 提供静态方法，不需要实例化
- 错误处理使用 `std::expected`
- 依赖 Crypto Module 进行解密

---

### 2.7 IO Module

#### 职责

- 封装 io_uring 操作
- 提供异步 IO 接口
- 管理 io_uring 实例

#### 暴露接口

```cpp
// IO 类（封装 io_uring）
class IO {
public:
    // 构造：初始化 io_uring
    explicit IO(unsigned queue_depth = 256);
    ~IO();
    
    // 禁止拷贝
    IO(const IO&) = delete;
    IO& operator=(const IO&) = delete;
    
    // 移动构造
    IO(IO&& other);
    IO& operator=(IO&& other);
    
    // 提交 IO 操作
    void submit();
    
    // 等待 IO 完成
    int wait(unsigned min_complete = 1);
    
    // 获取 io_uring 实例（供 Scheduler 使用）
    io_uring* ring() { return &ring_; }
    
    // 异步读取
    Task<size_t> async_read(int fd, std::span<uint8_t> buf);
    
    // 异步写入
    Task<size_t> async_write(int fd, std::span<const uint8_t> buf);
    
    // 异步 accept
    Task<int> async_accept(int listen_fd);
    
    // 异步 connect
    Task<int> async_connect(const std::string& addr, uint16_t port);
    
    // 异步关闭
    Task<void> async_close(int fd);
    
private:
    io_uring ring_;
    bool initialized_ = false;
};
```

#### 内部设计

- 封装 liburing 库
- 提供 C++ 风格的接口
- 与 Coroutine Module 集成

---

### 2.8 Coroutine Module

#### 职责

- 提供协程支持
- 定义 Task<> 等协程返回类型
- 提供协程工具函数

#### 暴露接口

```cpp
// Task 类（协程返回类型）
template<typename T = void>
class Task {
public:
    struct promise_type {
        Task get_return_object() {
            return Task{Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T value) { value_ = std::move(value); }
        void unhandled_exception() { std::terminate(); }
        
        // 支持 co_await 一个 Sender
        auto await_transform(auto&& sender) {
            return std::forward<decltype(sender)>(sender);
        }
        
    private:
        T value_;
    };
    
    using Handle = std::coroutine_handle<promise_type>;
    
    // 等待 Task 完成
    auto operator co_await() {
        return Awaitable{handle_};
    }
    
private:
    Handle handle_;
};

// 启动协程
void co_spawn(Task<void> task);

// Awaitable 类（用于 co_await）
template<typename T>
class Awaitable {
public:
    bool await_ready() { /* ... */ }
    void await_suspend(std::coroutine_handle<> handle) { /* ... */ }
    T await_resume() { /* ... */ }
};
```

#### 内部设计

- 实现 C++20 协程规范
- 与 IO Module 和 Scheduler Module 集成
- 提供类型安全的协程接口

---

### 2.9 Scheduler Module

#### 职责

- 协程调度器
- 管理就绪协程队列
- 恢复协程执行

#### 暴露接口

```cpp
// Scheduler 类
class Scheduler {
public:
    // 构造
    explicit Scheduler(IO& io);
    
    // 运行调度器（主循环）
    void run();
    
    // 停止调度器
    void stop();
    
    // 将协程加入就绪队列
    void ready(std::coroutine_handle<> handle);
    
    // 获取 IO 实例
    IO& io() { return io_; }
    
private:
    IO& io_;
    std::queue<std::coroutine_handle<>> ready_queue_;
    std::mutex mutex_;
    std::atomic<bool> running_{true};
};
```

#### 内部设计

- 管理就绪协程队列
- 与 IO Module 集成（处理 io_uring 完成事件）
- 线程安全（支持多线程）

---

### 2.10 Relay Module

#### 职责

- 数据转发
- 双向中转
- 处理加密/解密

#### 暴露接口

```cpp
// Relay 类
class Relay {
public:
    // 启动双向中转协程
    static Task<void> start_relay(int client_fd, int target_fd,
                                  SecurityType sec,
                                  IO& io);
    
private:
    // 单向转发
    static Task<void> relay_one_direction(int from_fd, int to_fd,
                                         SecurityType sec,
                                         IO& io);
};
```

#### 内部设计

- 使用协程实现双向中转
- 依赖 Protocol Module 和 Crypto Module
- 与 IO Module 集成

---

### 2.11 Worker Module

#### 职责

- Worker 线程实现
- 管理自己的 IO 实例
- 处理分配给自己的连接

#### 暴露接口

```cpp
// Worker 类
class Worker {
public:
    // 构造
    explicit Worker(size_t id);
    
    // 启动 Worker 线程
    void start();
    
    // 停止 Worker 线程
    void stop();
    
    // 等待 Worker 线程退出
    void join();
    
    // 获取 Worker ID
    size_t id() const { return id_; }
    
    // 获取连接数
    size_t connection_count() const { return connection_count_.load(); }
    
    // 添加新连接（主线程调用）
    void add_connection(int client_fd);
    
    // 获取 eventfd
    int eventfd() const { return eventfd_; }
    
private:
    // Worker 线程主函数
    void run();
    
    // 处理新连接
    Task<void> handle_new_connection(int client_fd);
    
    // 处理连接
    Task<void> handle_connection(int client_fd);
    
    size_t id_;
    std::thread thread_;
    IO io_;
    Scheduler scheduler_;
    int eventfd_;
    LockFreeQueue<int> pending_queue_;  // SPSC 无锁队列
    std::atomic<size_t> connection_count_{0};
    std::atomic<bool> running_{true};
};
```

#### 内部设计

- 每个 Worker 有自己的 IO 实例和 Scheduler
- 使用 eventfd + SPSC 无锁队列接收新连接
- 线程安全

---

### 2.12 Server Module

#### 职责

- 服务端主逻辑
- 管理 Worker 线程
- 启动和停止服务

#### 暴露接口

```cpp
// Server 类
class Server {
public:
    // 构造
    explicit Server(const Config& config);
    
    // 启动服务
    bool start();
    
    // 停止服务
    void stop();
    
    // 等待服务退出
    void wait();
    
private:
    // 主线程函数
    void main_thread();
    
    // 接受新连接
    void accept_loop();
    
    // 负载均衡：选择 Worker
    Worker* select_worker();
    
    Config config_;
    int listen_fd_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{true};
};
```

#### 内部设计

- 管理 Worker 线程
- 主线程接受新连接并分派给 Worker
- 负载均衡

---

### 2.13 Main Thread Module

#### 职责

- 程序入口
- 解析命令行参数
- 启动 Server

#### 暴露接口

```cpp
// main.cpp
int main(int argc, char* argv[]) {
    // 1. 解析命令行参数
    // 2. 加载配置
    // 3. 初始化日志
    // 4. 启动 Server
    // 5. 等待退出信号
}
```

---

## 3. 模块内部分层设计

### 3.1 IO Module 内部分层

```
┌─────────────────────────────────────────────┐
│            IO Module                       │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │          Public API                    │  │
│  │  - async_read()                       │  │
│  │  - async_write()                      │  │
│  │  - async_accept()                     │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          Sender/Receiver               │  │
│  │  - ReadSender                         │  │
│  │  - WriteSender                        │  │
│  │  - AcceptSender                       │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          io_uring Wrapper              │  │
│  │  - submit_sqe()                       │  │
│  │  - peek_cqe()                         │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          liburing                       │  │
│  └────────────────────────────────────────┘  │
│                                              │
└─────────────────────────────────────────────┘
```

### 3.2 Protocol Module 内部分层

```
┌─────────────────────────────────────────────┐
│            Protocol Module                  │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │          Public API                    │  │
│  │  - parse_request_aead()               │  │
│  │  - parse_request_legacy()             │  │
│  │  - encode_response()                  │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          Parser Implementation          │  │
│  │  - parse_aead_impl()                  │  │
│  │  - parse_legacy_impl()                │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          Crypto Module                  │  │
│  └────────────────────────────────────────┘  │
│                                              │
└─────────────────────────────────────────────┘
```

### 3.3 Coroutine Module 内部分层

```
┌─────────────────────────────────────────────┐
│            Coroutine Module                │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │          Public API                    │  │
│  │  - Task<T>                           │  │
│  │  - co_spawn()                        │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          Awaitable                     │  │
│  │  - await_ready()                      │  │
│  │  - await_suspend()                    │  │
│  │  - await_resume()                     │  │
│  └────────────────────────────────────────┘  │
│                   │                            │
│                   ▼                            │
│  ┌────────────────────────────────────────┐  │
│  │          Scheduler                     │  │
│  └────────────────────────────────────────┘  │
│                                              │
└─────────────────────────────────────────────┘
```

---

## 4. 实现路线图

### 4.1 阶段 1：基础框架

**目标**：搭建项目结构，实现基础模块

1. **项目结构和构建系统**
   - 创建目录结构
   - 编写 CMakeLists.txt
   - 配置依赖库（yaml-cpp, OpenSSL, liburing, fmt）

2. **Config Module**
   - 实现配置加载
   - 编写单元测试

3. **Logger Module**
   - 实现日志功能
   - 编写单元测试

4. **IO Module**
   - 封装 io_uring
   - 实现异步 IO 接口

5. **Coroutine Module**
   - 实现 Task<>
   - 实现 Awaitable

### 4.2 阶段 2：协议实现

**目标**：实现 VMess 协议解析和加密

1. **Crypto Module**
   - 实现 AES-128-GCM
   - 实现 ChaCha20-Poly1305
   - 编写单元测试

2. **Protocol Module**
   - 实现 VMess 请求头解析
   - 实现 VMess 响应头编码
   - 编写单元测试

### 4.3 阶段 3：业务模块

**目标**：实现用户管理、会话管理、数据转发

1. **User Manager Module**
   - 实现用户加载和查找
   - 编写单元测试

2. **Session Manager Module**
   - 实现会话管理
   - 实现防重放
   - 编写单元测试

3. **Relay Module**
   - 实现数据转发
   - 编写单元测试

### 4.4 阶段 4：多线程和集成

**目标**：实现多线程和集成测试

1. **Scheduler Module**
   - 实现协程调度器
   - 与 IO Module 集成

2. **Worker Module**
   - 实现 Worker 线程
   - 实现 eventfd + SPSC 无锁队列

3. **Server Module**
   - 实现服务端主逻辑
   - 实现负载均衡

4. **集成测试**
   - 测试完整流程
   - 性能测试

---

## 5. 总结

### 5.1 模块划分

- **基础模块**：Config, Logger, IO, Coroutine
- **业务模块**：Crypto, Protocol, User Manager, Session Manager, Relay
- **架构模块**：Scheduler, Worker, Server, Main Thread

### 5.2 实现顺序

1. **从底层到上层**：先实现基础模块，再实现业务模块，最后实现架构模块
2. **从简单到复杂**：先实现单线程版本，再实现多线程版本
3. **测试驱动**：每个模块都编写单元测试

### 5.3 模块内部分层

- **IO Module**：Public API → Sender/Receiver → io_uring Wrapper → liburing
- **Protocol Module**：Public API → Parser Implementation → Crypto Module
- **Coroutine Module**：Public API → Awaitable → Scheduler

---

## 6. 参考资料

- C++20 协程教程：https://lewissbaker.github.io/
- io_uring 指南：https://kernel.dk/io_uring.pdf
- VMess 协议：https://www.v2ray.com/eng/protocols/vmess.html
- yaml-cpp：https://github.com/jbeder/yaml-cpp
- fmt：https://github.com/fmtlib/fmt
