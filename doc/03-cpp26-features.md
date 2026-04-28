# C++26 新特性与 std::execution

> 整理时间：2026-04-28

## 1. C++26 概述

C++26 是 C++ 标准的下一个版本，预计 2026 年发布。核心新特性包括：

- **执行控制库（std::execution）**：统一的异步执行框架
- **标准库模块化**：`import std;`
- **反射（静态反射）**：编译期反射能力
- **协程改进**：`std::generator` 等
- **容器增强**：`std::flat_map`、`std::flat_set`
- **格式化增强**：`std::print`、`std::println`

## 2. std::execution 详解

### 2.1 核心概念

`std::execution` 是 C++26 引入的通用异步执行框架，基于 **Sender/Receiver** 模型。

#### 四大核心组件

| 组件            | 概念                    | 作用                           |
|-----------------|-------------------------|--------------------------------|
| Scheduler       | `std::execution::scheduler` | 执行上下文的轻量级句柄       |
| Sender          | `std::execution::sender`    | 对异步工作的描述（懒执行）   |
| Receiver        | `std::execution::receiver`  | 消费异步结果的回调           |
| Operation State | `std::execution::operation_state` | 异步操作的状态对象       |

#### 三种完成通道

Sender 有三条完成通道：
1. **值通道（Value）**：成功完成，传递结果
2. **错误通道（Error）**：执行出错，传递错误
3. **停止通道（Stopped）**：操作被取消

### 2.2 执行流程

```
1. 创建 Sender（描述异步操作，不执行）
2. connect(Sender, Receiver) → OperationState
3. start(OperationState) → 启动异步操作
4. 完成后调用对应回调：
   - set_value(Receiver, 结果...)  // 成功
   - set_error(Receiver, 错误)     // 出错
   - set_stopped(Receiver)         // 取消
```

### 2.3 Sender 工厂

创建基础 Sender：

```cpp
// 创建一个立即返回值的 Sender
auto s1 = std::execution::just(1, 2, 3);

// 创建一个立即返回错误的 Sender
auto s2 = std::execution::just_error(std::error_code{});

// 创建一个立即停止的 Sender
auto s3 = std::execution::just_stopped();

// 从环境查询信息
auto s4 = std::execution::read_env(std::execution::get_scheduler);

// 在指定调度器上调度
auto s5 = std::execution::schedule(scheduler);
```

### 2.4 Sender 适配器

组合和修改 Sender 行为（支持管道操作）：

```cpp
// 成功完成后执行函数
auto s = std::execution::just(1)
       | std::execution::then([](int x) { return x + 1; });

// 指定开始执行的调度器
auto s2 = s | std::execution::starts_on(thread_pool_scheduler);

// 切换后续操作的调度器
auto s3 = s | std::execution::continues_on(io_scheduler);

// 在指定调度器上临时执行
auto s4 = std::execution::on(thread_pool_scheduler, s);

// 错误处理
auto s5 = s | std::execution::upon_error([](auto err) { /* 处理错误 */ });

// 取消处理
auto s6 = s | std::execution::upon_stopped([]() { /* 处理取消 */ });

// 等待所有 Sender 完成
auto s7 = std::execution::when_all(s1, s2, s3);

// 转换为可多次连接的 Sender
auto s8 = std::execution::split(s);

// 批量执行
auto s9 = std::execution::bulk(s, 100, [](int i) { /* 并行执行 */ });
```

### 2.5 Sender 消费者

消费 Sender，获取结果：

```cpp
// 阻塞等待结果
auto result = std::this_thread::sync_wait(sender);
// result 是 std::optional<std::tuple<...>>

// 支持多完成签名的版本
auto result2 = std::this_thread::sync_wait_with_variant(sender);
```

### 2.6 完整示例

```cpp
#include <cstdio>
#include <execution>
#include <string>
#include <thread>
#include <utility>

int main() {
    // 1. 创建手动驱动的执行上下文
    std::execution::run_loop loop;

    // 2. 启动工作线程
    std::jthread worker([&](std::stop_token st) {
        std::stop_callback cb{st, [&] { loop.finish(); }};
        loop.run();
    });

    // 3. 创建 Sender 链
    auto hello = std::execution::just(std::string("hello world"));
    auto print = std::move(hello)
               | std::execution::then([](std::string msg) {
                     return std::puts(msg.c_str());
                 });

    // 4. 指定在 IO 线程执行
    auto work = std::execution::on(loop.get_scheduler(), std::move(print));

    // 5. 阻塞等待结果
    auto [result] = std::this_thread::sync_wait(std::move(work)).value();

    return result;
}
```

## 3. 在 VMess 项目中的应用设想

### 3.1 IO 模型设计

结合 `epoll` 和 `std::execution`：

```cpp
// 自定义 epoll 调度器
class EpollScheduler {
public:
    // std::execution::scheduler 概念实现
    EpollSender schedule() noexcept;
    
    // 事件循环
    void run();
    
    // 注册文件描述符
    void register_fd(int fd, uint32_t events, EpollCallback cb);
};

// 使用方式
auto read_sender = epoll_scheduler.async_read(fd, buffer, size);
auto process_sender = read_sender
                   | std::execution::then([](size_t bytes_read) {
                         return process_vmess_header(buffer, bytes_read);
                     })
                   | std::execution::then([](VmessHeader header) {
                         return forward_to_target(header);
                     });

std::execution::start(std::execution::connect(process_sender, my_receiver));
```

### 3.2 连接处理流程

```
新连接到达
  → epoll 触发 EPOLLIN
  → async_read() Sender 创建
  → then() 解析 VMess 头
  → then() 连接到目标
  → then() 启动双向转发
  → when_all() 等待读写完成
  → 清理连接
```

### 3.3 优势

1. **类型安全**：编译期检查完成签名
2. **组合性**：通过管道操作组合异步操作
3. **调度灵活**：可指定不同阶段的执行上下文
4. **与协程集成**：可使用 `co_await` 等待 Sender

## 4. 编译器支持情况

> **重要提示**：C++26 尚未正式发布，主流编译器支持有限。

### 4.1 当前状态（2026 年初）

- **GCC**：实验性支持部分特性
- **Clang**：通过 `libc++` 实验性支持
- **MSVC**：部分支持

### 4.2 替代方案

在编译器完全支持之前，可以使用：

1. **stdexec**：NVIDIA 的 `std::execution` 参考实现
   - GitHub：https://github.com/NVIDIA/stdexec
   - 可用于生产和测试

2. **libunifex**：Facebook 的 Sender/Receiver 实现
   - GitHub：https://github.com/facebookexperimental/libunifex

3. **Asio**：Boost.Asio 的异步模型
   - 与 Sender/Receiver 模型类似
   - 可用于过渡

### 4.3 项目建议

考虑到 `std::execution` 的支持情况，建议：

1. **第一阶段**：使用 `epoll` + 手写状态机，不使用 `std::execution`
2. **第二阶段**：封装 `epoll` 为自定义 Sender，使用 `stdexec`
3. **第三阶段**：切换到 `std::execution`（当编译器支持时）

## 5. 其他 C++26 特性

### 5.1 格式化输出

```cpp
std::print("VMess connection from {}\n", client_addr);
std::println("Error: {}", error_msg);
```

### 5.2 std::expected

更好的错误处理：

```cpp
std::expected<VmessHeader, VmessError> parse_header(...) {
    // ...
    return VmessHeader{...};
}

auto result = parse_header(buffer);
if (!result) {
    std::println("Parse error: {}", result.error());
}
```

### 5.3 std::generator

简化迭代器：

```cpp
std::generator<VmessChunk> read_chunks(int fd) {
    while (has_data(fd)) {
        co_yield read_chunk(fd);
    }
}
```

## 6. 参考资源

- cppreference：https://en.cppreference.com/cpp/execution
- cppreference 中文：https://cppreference.cn/w/cpp/execution
- stdexec GitHub：https://github.com/NVIDIA/stdexec
- P2300 提案：Sender/Receiver 模型标准提案
