#ifndef VMESS_CORO_URING_AWAITABLE_H
#define VMESS_CORO_URING_AWAITABLE_H

#include "net/io_uring.h"
#include "common/log.h"

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace vmess {
namespace coro {

// ── 前向声明 ──

struct AsyncRecvResult;
struct AsyncWriteResult;
struct AsyncAcceptResult;

// ── SQE user_data 编码 ──

/**
 * bit 63: 协程标志（1 = 协程上下文，0 = 旧式 UringRequest）
 * bit 48-62: 事件类型
 * bit 0-47: fd
 */
inline uint64_t makeCoroutineUserData(int fd, net::UringEventType type) {
    return 0x8000000000000000ULL |
           (static_cast<uint64_t>(static_cast<int>(type)) << 48) |
           (static_cast<uint64_t>(fd) & 0xFFFFFFFFFFFF);
}

inline bool isCoroutineUserData(uint64_t userData) {
    return (userData & 0x8000000000000000ULL) != 0;
}

inline int userDataFd(uint64_t userData) {
    return static_cast<int>(userData & 0xFFFFFFFFFFFF);
}

inline net::UringEventType userDataType(uint64_t userData) {
    return static_cast<net::UringEventType>((userData >> 48) & 0x7FFF);
}

// ── Awaitable 结果结构 ──

struct AsyncRecvResult {
    std::vector<uint8_t> buf;       // recv 缓冲区
    std::vector<uint8_t> data;      // recv 结果数据
    int result = 0;                  // CQE result
};

struct AsyncWriteResult {
    int result = 0;                  // CQE result (send/connect)
};

struct AsyncAcceptResult {
    int result = 0;                  // CQE result (accepted fd)
};

// ── 协程挂起注册表 ──

/**
 * 管理 "fd + 事件类型 → (Awaitable指针, 协程句柄)" 的映射。
 * io_uring 主循环收到 CQE 后：
 *   1. 通过 user_data 识别是否为协程上下文
 *   2. 从注册表取出 Awaitable 指针，setResult(result)
 *   3. 从注册表取出协程句柄，resume()
 */
class CoroutineRegistry {
public:
    struct Entry {
        void* awaitable;            // 指向 result 结构体
        std::coroutine_handle<> handle;
    };

    static CoroutineRegistry& instance() {
        static CoroutineRegistry reg;
        return reg;
    }

    void registerAwaitable(int fd, net::UringEventType type, void* awaitable, std::coroutine_handle<> h) {
        auto key = makeKey(fd, type);
        entries_[key] = {awaitable, h};
    }

    /// 只移除条目，不 resume（用于协程即将被销毁时）
    void erase(int fd, net::UringEventType type) {
        auto key = makeKey(fd, type);
        entries_.erase(key);
    }

    /// 移除某个 fd 的所有注册（READ + WRITE + ACCEPT）
    void eraseAll(int fd) {
        erase(fd, net::UringEventType::READ);
        erase(fd, net::UringEventType::WRITE);
        erase(fd, net::UringEventType::ACCEPT);
    }

    /// 查找并移除条目，设置结果并 resume 协程
    /// 返回 true 表示找到并处理了条目
    bool takeAndResume(int fd, net::UringEventType type, int cqeResult) {
        auto key = makeKey(fd, type);
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;

        auto [awaitable, handle] = it->second;
        entries_.erase(it);

        // 设置结果
        switch (type) {
            case net::UringEventType::READ: {
                auto* r = static_cast<AsyncRecvResult*>(awaitable);
                r->result = cqeResult;
                if (cqeResult > 0) {
                    r->data.assign(r->buf.begin(), r->buf.begin() + cqeResult);
                }
                break;
            }
            case net::UringEventType::WRITE: {
                auto* w = static_cast<AsyncWriteResult*>(awaitable);
                w->result = cqeResult;
                break;
            }
            case net::UringEventType::ACCEPT: {
                auto* a = static_cast<AsyncAcceptResult*>(awaitable);
                a->result = cqeResult;
                break;
            }
            default:
                break;
        }

        // 只在协程未结束时 resume
        if (handle && !handle.done()) {
            handle.resume();
        }
        return true;
    }

private:
    CoroutineRegistry() = default;

    static uint64_t makeKey(int fd, net::UringEventType type) {
        return (static_cast<uint64_t>(static_cast<int>(type)) << 32) |
               (static_cast<uint64_t>(fd) & 0xFFFFFFFF);
    }

    std::unordered_map<uint64_t, Entry> entries_;
};

// ── 可等待对象 ──

/**
 * @brief 异步 Recv
 * 
 * co_await AsyncRecv(fd, uring);
 * → 返回 std::vector<uint8_t>（空表示 EOF/错误）
 * 
 * 关键：recv 缓冲区 buf_ 是协程帧上的局部变量，
 * send 不会与 recv 共用缓冲区，彻底消除缓冲区竞争。
 */
class AsyncRecv {
public:
    AsyncRecv(int fd, net::IoUring& uring, size_t bufSize = 4096)
        : fd_(fd), uring_(uring) {
        result_.buf.resize(bufSize);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        CoroutineRegistry::instance().registerAwaitable(
            fd_, net::UringEventType::READ, &result_, h);

        auto* ring = uring_.ring();
        if (!ring) {
            result_.result = -1;
            h.resume();
            return;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            result_.result = -1;
            h.resume();
            return;
        }

        io_uring_prep_recv(sqe, fd_, result_.buf.data(), result_.buf.size(), 0);
        sqe->user_data = makeCoroutineUserData(fd_, net::UringEventType::READ);
    }

    std::vector<uint8_t> await_resume() {
        return std::move(result_.data);
    }

private:
    int fd_;
    net::IoUring& uring_;
    AsyncRecvResult result_;
};

/**
 * @brief 异步 Send
 * 
 * co_await AsyncSend(fd, uring, data, len);
 * → 返回 int（发送结果，<=0 表示错误）
 * 
 * 关键：数据拷贝到协程帧上的 buf_，send 完成前不会被覆写。
 */
class AsyncSend {
public:
    AsyncSend(int fd, net::IoUring& uring, const void* data, size_t len)
        : fd_(fd), uring_(uring) {
        buf_.assign(static_cast<const uint8_t*>(data),
                    static_cast<const uint8_t*>(data) + len);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        CoroutineRegistry::instance().registerAwaitable(
            fd_, net::UringEventType::WRITE, &result_, h);

        auto* ring = uring_.ring();
        if (!ring) {
            result_.result = -1;
            h.resume();
            return;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            result_.result = -1;
            h.resume();
            return;
        }

        io_uring_prep_send(sqe, fd_, buf_.data(), buf_.size(), 0);
        sqe->user_data = makeCoroutineUserData(fd_, net::UringEventType::WRITE);
    }

    int await_resume() {
        return result_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    std::vector<uint8_t> buf_;
    AsyncWriteResult result_;
};

/**
 * @brief 异步 Connect
 * 
 * co_await AsyncConnect(fd, uring, addr, addrLen);
 * → 返回 int（连接结果，0=成功，<0=错误）
 */
class AsyncConnect {
public:
    AsyncConnect(int fd, net::IoUring& uring,
                 const struct sockaddr* addr, socklen_t addrLen)
        : fd_(fd), uring_(uring), addr_(addr), addrLen_(addrLen) {}

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        CoroutineRegistry::instance().registerAwaitable(
            fd_, net::UringEventType::WRITE, &result_, h);

        auto* ring = uring_.ring();
        if (!ring) {
            result_.result = -1;
            h.resume();
            return;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            result_.result = -1;
            h.resume();
            return;
        }

        io_uring_prep_connect(sqe, fd_, addr_, addrLen_);
        sqe->user_data = makeCoroutineUserData(fd_, net::UringEventType::WRITE);
    }

    int await_resume() {
        return result_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    const struct sockaddr* addr_;
    socklen_t addrLen_;
    AsyncWriteResult result_;
};

/**
 * @brief 异步 Accept
 * 
 * co_await AsyncAccept(listenFd, uring);
 * → 返回 int（新连接的 fd，<0 表示错误）
 */
class AsyncAccept {
public:
    AsyncAccept(int listenFd, net::IoUring& uring)
        : listenFd_(listenFd), uring_(uring) {
        memset(&clientAddr_, 0, sizeof(clientAddr_));
        addrLen_ = sizeof(clientAddr_);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        CoroutineRegistry::instance().registerAwaitable(
            listenFd_, net::UringEventType::ACCEPT, &result_, h);

        auto* ring = uring_.ring();
        if (!ring) {
            result_.result = -1;
            h.resume();
            return;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            result_.result = -1;
            h.resume();
            return;
        }

        io_uring_prep_accept(sqe, listenFd_,
                             reinterpret_cast<sockaddr*>(&clientAddr_),
                             &addrLen_, 0);
        sqe->user_data = makeCoroutineUserData(listenFd_, net::UringEventType::ACCEPT);
    }

    int await_resume() {
        return result_.result;
    }

private:
    int listenFd_;
    net::IoUring& uring_;
    struct sockaddr_in clientAddr_{};
    socklen_t addrLen_;
    AsyncAcceptResult result_;
};

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_URING_AWAITABLE_H
