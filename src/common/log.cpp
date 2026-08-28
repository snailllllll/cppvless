#include "common/log.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace vless {
namespace common {

namespace {

constexpr size_t kMaxQueueSize = 100000;  // 队列上限（背压阈值）
constexpr size_t kBatchLimit = 1024;      // 单批最大条数（防单次 write 过大）

long threadId() {
#ifdef SYS_gettid
    return static_cast<long>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

// 时间戳：YYYY-MM-DD HH:MM:SS.mmm
std::string timestamp() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(tv.tv_usec / 1000));
    return std::string(buf);
}

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
        default:              return "NONE";
    }
}

} // namespace

struct Logger::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool stop = false;
    size_t dropped = 0;   // 背压丢弃计数
    int fileFd = -1;      // 落盘文件 fd（-1 = 不落盘）
    std::thread worker;
};

Logger::Logger() : impl_(new Impl) {
    impl_->worker = std::thread([this] { workerLoop(); });
}

Logger::~Logger() {
    shutdown();
    if (impl_->fileFd >= 0) {
        ::close(impl_->fileFd);
        impl_->fileFd = -1;
    }
    delete impl_;
    impl_ = nullptr;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    level_ = level;
}

bool Logger::shouldLog(LogLevel level) const {
    return static_cast<int>(level) <= static_cast<int>(level_);
}

void Logger::setLogFile(const std::string& path) {
    if (path.empty()) return;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;  // 打开失败：回退到仅 stderr
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->fileFd >= 0) {
        ::close(impl_->fileFd);
    }
    impl_->fileFd = fd;
}

void Logger::enqueue(LogLevel level, std::string&& line) {
    std::string full = formatLogLine(level, line);
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->queue.size() >= kMaxQueueSize) {
            ++impl_->dropped;  // 背压：丢弃并计数
            return;
        }
        impl_->queue.push_back(std::move(full));
    }
    impl_->cv.notify_one();
}

void Logger::shutdown() {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->stop) return;
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void Logger::workerLoop() {
    std::deque<std::string> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(impl_->mtx);
            impl_->cv.wait(lock, [this] {
                return impl_->stop || !impl_->queue.empty();
            });
            if (impl_->queue.empty()) {
                if (impl_->stop) break;
                continue;
            }
            // 批量取走（限量，防止单次 write 过大）
            size_t take = std::min(impl_->queue.size(), kBatchLimit);
            batch.clear();
            for (size_t i = 0; i < take; ++i) {
                batch.push_back(std::move(impl_->queue.front()));
                impl_->queue.pop_front();
            }
        }

        // 解锁后批量写：整批拼为一块，一次 write
        size_t total = 0;
        for (const auto& s : batch) {
            total += s.size();
        }
        std::string out;
        out.reserve(total);
        for (auto& s : batch) {
            out += s;
        }

        ::write(STDERR_FILENO, out.data(), out.size());
        int fd = impl_->fileFd;
        if (fd >= 0) {
            ::write(fd, out.data(), out.size());
        }

        // 背压丢弃提示（防静默）
        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            dropped = impl_->dropped;
            impl_->dropped = 0;
        }
        if (dropped > 0) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "[Logger] dropped %zu log lines (queue overflow)\n", dropped);
            ::write(STDERR_FILENO, buf, static_cast<size_t>(n));
        }
    }
}

void setLogLevel(LogLevel level) {
    Logger::instance().setLevel(level);
}

LogLevel parseLogLevel(const std::string& s) {
    if (s == "none" || s == "NONE") return LogLevel::NONE;
    if (s == "error" || s == "ERROR") return LogLevel::ERROR;
    if (s == "warn" || s == "WARN") return LogLevel::WARN;
    if (s == "info" || s == "INFO") return LogLevel::INFO;
    if (s == "debug" || s == "DEBUG") return LogLevel::DEBUG;
    return LogLevel::INFO;
}

std::string formatLogLine(LogLevel level, const std::string& message) {
    std::string out;
    out.reserve(message.size() + 64);
    out += '[';
    out += timestamp();
    out += "] [";
    out += levelName(level);
    out += "] [tid=";
    out += std::to_string(threadId());
    out += "] ";
    out += message;
    out += '\n';
    return out;
}

} // namespace common
} // namespace vless
