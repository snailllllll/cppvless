#ifndef VLESS_COMMON_LOG_H
#define VLESS_COMMON_LOG_H

#include <sstream>
#include <string>

namespace vless {
namespace common {

enum class LogLevel {
    NONE = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4
};

/**
 * @brief 轻量异步日志器（目标 A 方案，见 doc/dev/design/logging-plan.md）
 *
 * 设计要点：
 *   - 调用点只做拼接 + 入队（一次加锁 push），不做任何 I/O；
 *   - 后台线程批量取出 → 一次 write 合并写入 stderr（可选落盘文件）；
 *   - 去掉 std::endl 逐条 flush：系统调用从"每条 1 次"降为"每批 1 次"；
 *   - 队列满时丢弃并计数（背压），防止日志风暴打爆内存；
 *   - 自带时间戳 + 线程号（排障）。
 *
 * 接口兼容：LOG_ERROR/LOG_WARN/LOG_INFO/LOG_DEBUG 宏保持不变，
 * setLogLevel/parseLogLevel 语义不变。实现位于 src/common/log.cpp。
 */
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const { return level_; }
    bool shouldLog(LogLevel level) const;

    /// 设置落盘文件（空 = 仅 stderr）；在首次写入前调用一次
    void setLogFile(const std::string& path);

    /// 入队一条日志（调用点：拼接 + 加锁 push，极快）
    void enqueue(LogLevel level, std::string&& line);

    /// 停止后台线程并排空剩余队列（进程退出前调用）
    void shutdown();

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void workerLoop();

    struct Impl;
    Impl* impl_;

    LogLevel level_ = LogLevel::INFO;
};

void setLogLevel(LogLevel level);

LogLevel parseLogLevel(const std::string& s);

/// 格式化完整日志行：时间戳 + 线程号 + 级别 + 消息
std::string formatLogLine(LogLevel level, const std::string& message);

template<typename... Args>
void log(LogLevel level, const std::string& tag, Args&&... args) {
    auto& logger = Logger::instance();
    if (!logger.shouldLog(level)) return;

    std::ostringstream oss;
    oss << "[" << tag << "] ";
    (oss << ... << args);
    logger.enqueue(level, std::move(oss.str()));
}

#define LOG_ERROR(tag, ...) ::vless::common::log(::vless::common::LogLevel::ERROR, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  ::vless::common::log(::vless::common::LogLevel::WARN,  tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  ::vless::common::log(::vless::common::LogLevel::INFO,  tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) ::vless::common::log(::vless::common::LogLevel::DEBUG, tag, __VA_ARGS__)

} // namespace common
} // namespace vless

#endif // VLESS_COMMON_LOG_H
