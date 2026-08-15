#ifndef VMESS_CORO_URING_AWAITABLE_H
#define VMESS_CORO_URING_AWAITABLE_H

#include "net/io_uring.h"

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace vmess {
namespace coro {

// ── 前向声明 ──

struct AsyncRecvResult;
struct AsyncWriteResult;
struct AsyncAcceptResult;

// ── SQE user_data 编码 ──

/**
 * bit 63: 协程标志（固定置 1，标识协程上下文）
 * bit 48-62: 事件类型
 * bit 0-47: fd
 */
inline uint64_t makeCoroutineUserData(int fd, net::UringEventType type) {
    return 0x8000000000000000ULL |
           (static_cast<uint64_t>(static_cast<int>(type)) << 48) |
           (static_cast<uint64_t>(fd) & 0xFFFFFFFFFFFF);
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

/**
 * @brief AsyncRecv 返回的详细结果
 * 
 * 参考 Go: buf.Copy 中 EOF 视为正常结束，非 EOF 错误才返回 error
 * 
 * 用法:
 *   auto rr = co_await AsyncRecv(fd, uring);
 *   if (rr.eof())      → 对端关闭（正常结束，应半关闭）
 *   if (rr.error())    → 异常错误（应强制中断）
 *   if (rr.ok())       → 正常读取数据
 */
struct RecvResult {
    std::vector<uint8_t> data;
    int result = 0;                  // CQE result: 0=EOF, >0=bytes read, <0=-errno

    bool eof() const { return result == 0; }
    bool error() const { return result < 0; }
    bool ok() const { return result > 0; }
    explicit operator bool() const { return ok(); }
    size_t size() const { return data.size(); }
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
        // 每个线程维护独立 registry，避免多线程下的并发读写和误 resume。
        thread_local CoroutineRegistry reg;
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
 * → 返回 RecvResult（可区分 EOF 和错误）
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

    RecvResult await_resume() {
        RecvResult rr;
        rr.data = std::move(result_.data);
        rr.result = result_.result;
        return rr;
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

/**
 * @brief 异步 Shutdown（半关闭）
 * 
 * co_await AsyncShutdown(fd, uring, SHUT_WR);
 * → 返回 int（0=成功, <0=错误）
 * 
 * 参考 Go: task.Close(writer) → 对端收到 FIN
 * 半关闭 = 优雅通知对端"我不再发送数据"，但仍然可以接收
 */
class AsyncShutdown {
public:
    AsyncShutdown(int fd, net::IoUring& uring, int how = SHUT_WR)
        : fd_(fd), uring_(uring), how_(how) {}

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

        io_uring_prep_shutdown(sqe, fd_, how_);
        sqe->user_data = makeCoroutineUserData(fd_, net::UringEventType::WRITE);
    }

    int await_resume() {
        return result_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    int how_;
    AsyncWriteResult result_;
};

/**
 * @brief 异步 RecvFrom（UDP 使用，可获取源地址）
 *
 * co_await AsyncRecvFrom(fd, uring);
 * → 返回 AsyncRecvFromResult（数据 + 源地址）
 */
struct AsyncRecvFromResult {
    RecvResult rr;
    struct sockaddr_storage srcAddr {};
    socklen_t srcAddrLen = 0;
};

class AsyncRecvFrom {
public:
    AsyncRecvFrom(int fd, net::IoUring& uring, size_t bufSize = 65536)
        : fd_(fd), uring_(uring) {
        recvResult_.buf.resize(bufSize);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        CoroutineRegistry::instance().registerAwaitable(
            fd_, net::UringEventType::READ, &recvResult_, h);

        auto* ring = uring_.ring();
        if (!ring) {
            recvResult_.result = -1;
            h.resume();
            return;
        }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            recvResult_.result = -1;
            h.resume();
            return;
        }

        // liburing 2.4 无 io_uring_prep_recvfrom，用 recvmsg 实现：
        // 需要在协程帧（awaitable）上保存 msghdr/iovec，直到完成
        iov_.iov_base = recvResult_.buf.data();
        iov_.iov_len = recvResult_.buf.size();
        msg_.msg_name = &srcAddr_;
        msg_.msg_namelen = sizeof(srcAddr_);
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;

        io_uring_prep_recvmsg(sqe, fd_, &msg_, 0);
        sqe->user_data = makeCoroutineUserData(fd_, net::UringEventType::READ);
    }

    AsyncRecvFromResult await_resume() {
        AsyncRecvFromResult res;
        res.rr.data = std::move(recvResult_.data);
        res.rr.result = recvResult_.result;
        res.srcAddrLen = msg_.msg_namelen;
        res.srcAddr = srcAddr_;
        return res;
    }

private:
    int fd_;
    net::IoUring& uring_;
    AsyncRecvResult recvResult_;   // buf = 接收缓冲，data = 结果
    struct sockaddr_storage srcAddr_ {};
    struct iovec iov_ {};
    struct msghdr msg_ {};
};

/**
 * @brief 异步 SendTo（UDP 使用，指定目标地址）
 *
 * co_await AsyncSendTo(fd, uring, data, len, addr, addrLen);
 * → 返回 int（发送结果，<=0 表示错误）
 *
 * 注意：使用 io_uring_prep_sendmsg 实现（liburing 2.1 起均可用），
 * 不用 io_uring_prep_sendto（liburing 2.3 才引入，CI 的 22.04 镜像为 2.1）。
 */
class AsyncSendTo {
public:
    AsyncSendTo(int fd, net::IoUring& uring,
                const void* data, size_t len,
                const struct sockaddr* addr, socklen_t addrLen)
        : fd_(fd), uring_(uring), addr_(addr), addrLen_(addrLen) {
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

        // 需要把 msghdr/iovec 保存在协程帧（awaitable）上直到 sendmsg 完成
        iov_.iov_base = buf_.data();
        iov_.iov_len = buf_.size();
        msg_.msg_name = const_cast<struct sockaddr*>(addr_);
        msg_.msg_namelen = addrLen_;
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;

        io_uring_prep_sendmsg(sqe, fd_, &msg_, 0);
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
    std::vector<uint8_t> buf_;
    struct iovec iov_ {};
    struct msghdr msg_ {};
    AsyncWriteResult result_;
};

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_URING_AWAITABLE_H
