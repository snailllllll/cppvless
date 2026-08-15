# 日志改造方案：引入 spdlog

> 状态：Draft（讨论中）
> 日期：2026-08-15
> 目标：解决日志 I/O 成为性能瓶颈的问题，同时增强排障信息

## 1. 背景：当前日志实现与性能问题

### 当前实现（`include/common/log.h`）

```cpp
template<typename... Args>
void log(LogLevel level, const std::string& tag, Args&&... args) {
    auto& logger = Logger::instance();
    if (!logger.shouldLog(level)) return;   // 级别过滤
    std::ostringstream oss;                  // 每次构造
    oss << "[" << tag << "] ";
    (oss << ... << args);
    std::cerr << oss.str() << std::endl;    // 全局流 + 强制 flush
}

#define LOG_INFO(tag, ...) vmess::common::log(vmess::common::LogLevel::INFO, tag, __VA_ARGS__)
```

### 性能问题（按严重程度）

| # | 问题 | 后果 |
|---|---|---|
| 1 | `std::cerr` 全局锁 | **所有 worker 线程写日志互相抢锁**，IO 异步化被日志层串行化 |
| 2 | `std::endl` 每次强制 flush（cerr 默认 unitbuf） | 每条日志一次 write() 系统调用，无缓冲无批量 |
| 3 | 日志参数在调用点提前求值 | 即使级别不输出，`req.addressString()` 等字符串拼接仍执行 |
| 4 | 每次构造 ostringstream / 拷贝 tag | 高频路径分配放大 |
| 5 | debug 级别每个 CQE 打日志 | 日志量 = 包速率，直接打满 |
| 6 | 无时间戳/无线程号 | 无法排查时序（如 connect→RST 间隔） |
| 7 | 无轮转/无落盘 | 只进 docker stderr，量大无法回溯 |

## 2. 方案：spdlog 异步日志

选择理由：spdlog 是 C++ 事实标准的日志库，异步模式把日志 I/O 移到后台线程，天然解决 #1/#2；内置时间戳/线程号/轮转/级别过滤。

### 2.1 集成方式（vendor 锁定版本）

- 工作区已有 `spdlog 1.17.0` 源码（`/data/workspace/spdlog`），拷贝进 `third_party/spdlog`；
- CMake：`add_subdirectory(third_party/spdlog)` 编译为静态库（`SPDLOG_COMPILED_LIB` 编译库模式，避免 header-only 拖慢每次编译）；
- 版本锁定 1.17.0，不受 CI（Ubuntu 22.04 apt 只有 1.8.5）影响；
- CI 无需额外网络/apt 依赖（与 liburing/OpenSSL 处理一致）。

### 2.2 日志架构

```
应用（worker 线程）──spdlog::async_logger──> 环形队列 ──后台线程──> rotating_file_sink + stderr
```

- **异步 logger**：`spdlog::init_thread_pool(8192, 1)`，worker 只入队不阻塞；
- **sink 双写**：文件（轮转，如 10MB × 5）+ stderr（docker 收集）；
- **pattern**：`[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v`（时间戳 + 级别 + 线程号）。

### 2.3 接口兼容策略

现有代码几十处 `LOG_INFO("Tag", "a", x, ...)` 流式调用。**v1 保留宏接口不变**：

```cpp
#define LOG_INFO(tag, ...) vmess::common::log(
    vmess::common::LogLevel::INFO, tag, __VA_ARGS__)
```

内部 `log()` 改为：ostringstream 拼接后调 `asyncLogger->info("{}", oss.str())`。

收益：调用点零改动；获得异步/轮转/时间戳/线程安全/级别过滤。
代价：参数仍在调用点求值（#3 未完全解决）。**高频路径后续改 fmt 风格**：

```cpp
#define LOG_INFO_F(tag, fmt, ...) vmess::common::logFmt(
    vmess::common::LogLevel::INFO, tag, fmt, __VA_ARGS__)
// 用法：LOG_INFO_F("VlessConnection", "target={}", req.addressString());
```

fmt 风格下，字符串格式化仅在级别匹配时执行（比流式省一次求值），但不做 lambda 完全惰性（v1 不做）。

### 2.4 排障增强

- 时间戳 + 线程号（多 worker 区分来源）；
- **模块级别过滤**：per-logger 级别（如只对 `VlessConnection` 开 debug），避免全局 debug 刷屏；
- 落盘 + 轮转，历史可回溯；
- 保留 `setLogLevel`/`parseLogLevel` 入口，映射到 spdlog 级别。

### 2.5 配置

| 项 | CLI / 环境变量 | 默认 |
|---|---|---|
| 日志文件 | `--log-file <path>` / `VLESS_LOG_FILE` | 无（仅 stderr） |
| 级别 | `--log <level>`（现有） | info |
| 轮转大小/个数 | 编译期常量（10MB × 5） | — |

## 3. 分阶段实施

| 阶段 | 内容 | 验证 |
|---|---|---|
| 1 | vendor spdlog + CMake 集成 + 异步 logger 初始化 + LOG_* 宏迁移 | 构建通过，日志有时间戳/线程号，正常落盘 |
| 2 | 排障增强：per-module 级别、fmt 风格宏、高频路径优化 | 生产日志量下降，排查时序可用 |
| 3 | （可选）异步日志参数调优、日志轮转策略验证 | 压测验证 worker 不被日志阻塞 |

## 4. 与 TLS 改造的关系

- 相互独立、解耦：日志改造不动协议/网络层，TLS 改造不动日志层；
- 可并行推进（各自独立提交），建议日志阶段 1 先落地（风险低、收益立竿见影）。

## 5. 待确认

- [ ] vendor 拷贝 vs git submodule（建议拷贝，版本锁定、CI 简单）
- [ ] 默认日志文件路径：`stderr`（docker 收集）or `/var/log/vmess/server.log`？
- [ ] fmt 风格宏（LOG_INFO_F）是否现在就引入？
