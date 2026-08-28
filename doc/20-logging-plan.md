# 日志改造方案：轻量异步日志（目标 A）

> 状态：**阶段 1 已实现**（轻量异步队列）；spdlog 列为可选升级路径
> 日期：2026-08-15
> 目标：解决日志 I/O 成为性能瓶颈的问题，同时增强基础排障信息

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
```

### 性能问题（按严重程度）

| # | 问题 | 后果 |
|---|---|---|
| 1 | `std::endl` 每次强制 flush（cerr 默认 unitbuf） | 每条日志一次 write() 系统调用，无缓冲无批量（**收益最大项**） |
| 2 | `std::cerr` 全局锁 | 所有 worker 线程写日志互相抢锁，IO 异步化被日志层串行化 |
| 3 | 日志参数在调用点提前求值 | 即使级别不输出，`req.addressString()` 等字符串拼接仍执行 |
| 4 | 每次构造 ostringstream / 拷贝 tag | 高频路径分配放大 |
| 5 | debug 级别每个 CQE 打日志 | 日志量 = 包速率，直接打满 |
| 6 | 无时间戳/无线程号 | 无法排查时序（如 connect→RST 间隔） |
| 7 | 无轮转/无落盘 | 只进 docker stderr，量大无法回溯 |

## 2. 选型结论：轻量异步优先，spdlog 作为升级路径

### 2.1 需求界定（目标 A：只要异步提性能）

关键洞察：**"异步"不是收益最大项**。收益排序：

```
去掉每条 flush（endl → '\n' + 批量写）   ← 收益最大：系统调用从"每条1次"降为"每批1次"
worker 不阻塞（异步队列）                ← 解决日志风暴/高频路径卡 worker
```

如果只是"异步提性能"，不需要 spdlog（40+ 源文件 + fmt，静态库体积 + 编译时间 ↑）。
100 行轻量异步队列即可达成。

### 2.2 轻量方案（已实现，`src/common/log.cpp`）

```
worker 线程:
  LOG_INFO(...) → 级别检查 → 拼接时间戳+级别+tid+tag+msg+'\n'
                → 加锁 push 到全局队列（deque<string>）

后台日志线程:
  循环：加锁一次批量取走（≤1024 条）→ 解锁 → 整批拼为一块 → 一次 write 到 stderr(+可选文件)
```

设计要点：

- **锁粒度：批量取，不是每条取**——worker push 只碰 mutex 一次，后台线程一次 drain 全队；
- **去 endl 是最大收益**——后台线程 write 系统调用数量级下降；
- **队列满背压**——阈值 100000 条，超出丢弃并计数，防日志风暴打爆内存；
- **时间戳 + 线程号**——`[2026-08-15 20:50:32.455] [ERROR] [tid=3687876] [Tls] ...`；
- **接口零改动**——`log()` 函数体内实现替换，几十处 `LOG_*` 调用点不动；
- 代码量约 130 行，零依赖。

### 2.3 与 spdlog 的对比

| 维度 | 轻量异步（当前） | spdlog |
|---|---|---|
| 依赖 | 零 | vendor 40+ 源文件 + fmt |
| 异步/去 flush | ✅ 已解决 | ✅ |
| 时间戳/线程号 | ✅ 已实现 | ✅ pattern 内置 |
| 轮转 | ❌（外部 logrotate / docker log driver） | ✅ rotating_file_sink |
| per-module 级别过滤 | ❌（要额外写） | ✅ per-logger |
| 落盘 | ✅ `--log-file` / `VLESS_LOG_FILE` | ✅ |

### 2.4 升级路径（何时才需要 spdlog）

当出现以下硬需求时升级 spdlog（接口已稳定，`LOG_*` 宏不变，替换成本低）：

- 生产需要**日志轮转**（单文件无限增长不可接受，且不能依赖 docker log driver）；
- 需要 **per-module 级别过滤**（如只对 `VlessConnection` 开 debug，避免全局 debug 刷屏）。

## 3. 实施记录

### 3.1 已实现（阶段 1）

| 项 | 内容 | 状态 |
|---|---|---|
| `common/log.h` | 重写：`Logger` 类声明 + 宏接口不变 + `log()` 模板调 `enqueue` | ✅ |
| `src/common/log.cpp` | 后台线程 + 队列 + 批量 write + 背压 + 时间戳/线程号 + 落盘 | ✅ |
| CMake | 新增 `common` 库，各库链接 | ✅ |
| `--log-file` | server/client 均支持 + `VLESS_LOG_FILE` 环境变量 | ✅ |
| 验证 | 构建零错误零警告；TLS 端到端 PASS；明文回归 PASS；stderr/落盘格式正确 | ✅ |

### 3.2 已知未解决（对应问题 #3/#5）

- **#3 参数提前求值**：宏参数在进函数前求值，`shouldLog` 挡不住。当前 v1 接受；
  若后续需要，引入 fmt 风格宏（`LOG_INFO_F(tag, fmt, ...)`）在级别匹配后才格式化；
- **#5 debug 每 CQE 打日志**：这是调用点策略问题，非日志框架问题；靠级别控制。

## 4. 与 TLS 改造的关系

- 相互独立、解耦：日志改造不动协议/网络层，TLS 改造不动日志层；
- 已并行落地（TLS 阶段 1-3 完成 + 日志阶段 1 完成）。

## 5. 待确认 / 后续

- [ ] 是否引入 fmt 风格宏（LOG_INFO_F）解决 #3？
- [ ] 是否现在就引入 spdlog（轮转 / per-module 过滤是硬需求时）？
- [ ] 默认日志路径：`stderr`（docker 收集）or `/var/log/vless/server.log`？
