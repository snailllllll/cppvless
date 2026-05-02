#ifndef VMESS_COMMON_LOG_H
#define VMESS_COMMON_LOG_H

#include <iostream>
#include <sstream>
#include <string>

namespace vmess {
namespace common {

enum class LogLevel {
    NONE = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    bool shouldLog(LogLevel level) const {
        return static_cast<int>(level) <= static_cast<int>(level_);
    }

private:
    LogLevel level_ = LogLevel::INFO;
};

inline void setLogLevel(LogLevel level) {
    Logger::instance().setLevel(level);
}

inline LogLevel parseLogLevel(const std::string& s) {
    if (s == "none" || s == "NONE") return LogLevel::NONE;
    if (s == "error" || s == "ERROR") return LogLevel::ERROR;
    if (s == "warn" || s == "WARN") return LogLevel::WARN;
    if (s == "info" || s == "INFO") return LogLevel::INFO;
    if (s == "debug" || s == "DEBUG") return LogLevel::DEBUG;
    return LogLevel::INFO;
}

template<typename... Args>
void log(LogLevel level, const std::string& tag, Args&&... args) {
    auto& logger = Logger::instance();
    if (!logger.shouldLog(level)) return;

    std::ostringstream oss;
    oss << "[" << tag << "] ";
    (oss << ... << args);
    std::cerr << oss.str() << std::endl;
}

#define LOG_ERROR(tag, ...) vmess::common::log(vmess::common::LogLevel::ERROR, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  vmess::common::log(vmess::common::LogLevel::WARN,  tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  vmess::common::log(vmess::common::LogLevel::INFO,  tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) vmess::common::log(vmess::common::LogLevel::DEBUG, tag, __VA_ARGS__)

} // namespace common
} // namespace vmess

#endif // VMESS_COMMON_LOG_H
