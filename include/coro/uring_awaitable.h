#ifndef VLESS_CORO_URING_AWAITABLE_H
#define VLESS_CORO_URING_AWAITABLE_H

#include "net/io_uring.h"

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace vless {
namespace coro {

// ── 前向声明 ──

struct UringOp;
struct RecvOp;
struct WriteOp;
struct AcceptOp;

/**
 * @brief 操作上下文（CQE → 协程的桥）
 *
 * 与 cpp-http-server 收敛的指针直分发模型：
 * SQE 的 user_data 直接存 &op（操作对象地址），CQE 到达时零查表，
 * 通过 completeFromCqe 调用 op 自带的完成回调，回调里写结果并 resume 协程。
 */
struct UringOp {
    using CompleteFn = void (*)(UringOp* self, int res, uint32_t flags) noexcept;

    CompleteFn onComplete = nullptr;      // 完成回调（子类静态函数）
    std::coroutine_handle<> handle{};     // 挂起的协程句柄
    int fd = -1;                          // 关联 fd（取消/追踪用）
    int result = 0;                       // CQE result（成功 >0 / EOF=0 / <0=-errno）

    /// 统一入口：事件循环收到 CQE 后调用
    static void completeFromCqe(uint64_t userData, int res, uint32_t flags) {
        if (userData == 0) return;
        auto* op = reinterpret_cast<UringOp*>(userData);
        if (!op || !op->onComplete) return;   // 已取消：置空回调后直接忽略
        op->onComplete(op, res, flags);
    }
};

/**
 * @brief 挂起操作追踪表（取消模型）
 *
 * 按 fd 索引所有进行中的操作。连接关闭时 cancelFd 作废该 fd 的
 * 全部挂起操作（置空 onComplete/handle），迟到的 CQE 会被
 * completeFromCqe 直接忽略，不会 resume 已销毁的协程帧。
 *
 * 注意：必须是 thread_local（每个事件循环线程独立），
 * 与 cpp-http-server 的普通 static 不同——vless 是多 worker 线程。
 */
class PendingUringOps {
public:
    static PendingUringOps& instance() {
        // 每个线程维护独立的追踪表，避免跨线程误 resume。
        thread_local PendingUringOps ops;
        return ops;
    }

    /// 登记进行中的操作
    void add(UringOp* op) {
        if (!op || op->fd < 0) return;
        byFd_[op->fd].insert(op);
    }

    /// 操作完成/取消后移除
    void remove(UringOp* op) {
        if (!op || op->fd < 0) return;
        auto it = byFd_.find(op->fd);
        if (it == byFd_.end()) return;
        it->second.erase(op);
        if (it->second.empty()) {
            byFd_.erase(it);
        }
    }

    /// 取消某个 fd 的所有挂起操作：置空回调与句柄，迟到 CQE 被忽略
    void cancelFd(int fd) {
        if (fd < 0) return;
        auto it = byFd_.find(fd);
        if (it == byFd_.end()) return;
        for (UringOp* op : it->second) {
            if (op) {
                op->onComplete = nullptr;
                op->handle = {};
            }
        }
        byFd_.erase(it);
    }

private:
    PendingUringOps() = default;
    std::unordered_map<int, std::unordered_set<UringOp*>> byFd_;
};

/// 错误/取消路径：写错误结果、移除追踪、唤醒协程
inline void abortOp(UringOp* op, int err) {
    op->result = err;
    PendingUringOps::instance().remove(op);
    op->onComplete = nullptr;
    if (op->handle && !op->handle.done()) {
        op->handle.resume();
    }
}

// ── 操作实现（每个 op 自带完成回调）──

/// Recv 操作（AsyncRecv / AsyncRecvFrom 共用）
struct RecvOp : UringOp {
    std::vector<uint8_t> buf;   // recv 缓冲区（协程帧上存活）
    std::vector<uint8_t> data;  // recv 结果数据

    static void complete(UringOp* self, int res, uint32_t /*flags*/) noexcept {
        auto* op = static_cast<RecvOp*>(self);
        op->result = res;
        if (res > 0) {
            op->data.assign(op->buf.begin(), op->buf.begin() + res);
        }
        auto h = op->handle;
        PendingUringOps::instance().remove(op);
        op->onComplete = nullptr;
        if (h && !h.done()) {
            h.resume();
        }
    }
};

/// Write 操作（AsyncSend / AsyncConnect / AsyncShutdown / AsyncSendTo 共用）
struct WriteOp : UringOp {
    static void complete(UringOp* self, int res, uint32_t /*flags*/) noexcept {
        auto* op = static_cast<WriteOp*>(self);
        op->result = res;
        auto h = op->handle;
        PendingUringOps::instance().remove(op);
        op->onComplete = nullptr;
        if (h && !h.done()) {
            h.resume();
        }
    }
};

/// Accept 操作（AsyncAccept 使用）
struct AcceptOp : UringOp {
    struct sockaddr_in clientAddr {};   // accept 客户端地址（协程帧上存活）
    socklen_t addrLen = sizeof(sockaddr_in);

    static void complete(UringOp* self, int res, uint32_t /*flags*/) noexcept {
        auto* op = static_cast<AcceptOp*>(self);
        op->result = res;
        auto h = op->handle;
        PendingUringOps::instance().remove(op);
        op->onComplete = nullptr;
        if (h && !h.done()) {
            h.resume();
        }
    }
};

// ── Awaitable 结果结构 ──

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

// ── 可等待对象 ──

/**
 * @brief 异步 Recv
 * 
 * co_await AsyncRecv(fd, uring);
 * → 返回 RecvResult（可区分 EOF 和错误）
 * 
 * 关键：recv 缓冲区 op_.buf 是协程帧上的局部变量，
 * send 不会与 recv 共用缓冲区，彻底消除缓冲区竞争。
 */
class AsyncRecv {
public:
    AsyncRecv(int fd, net::IoUring& uring, size_t bufSize = 4096)
        : fd_(fd), uring_(uring) {
        op_.buf.resize(bufSize);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &RecvOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        io_uring_prep_recv(sqe, fd_, op_.buf.data(), op_.buf.size(), 0);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    RecvResult await_resume() {
        RecvResult rr;
        rr.data = std::move(op_.data);
        rr.result = op_.result;
        return rr;
    }

private:
    int fd_;
    net::IoUring& uring_;
    RecvOp op_;
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
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &WriteOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        io_uring_prep_send(sqe, fd_, buf_.data(), buf_.size(), 0);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    int await_resume() {
        return op_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    std::vector<uint8_t> buf_;
    WriteOp op_;
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
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &WriteOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        io_uring_prep_connect(sqe, fd_, addr_, addrLen_);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    int await_resume() {
        return op_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    const struct sockaddr* addr_;
    socklen_t addrLen_;
    WriteOp op_;
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
        : listenFd_(listenFd), uring_(uring) {}

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op_.fd = listenFd_;
        op_.handle = h;
        op_.onComplete = &AcceptOp::complete;
        memset(&op_.clientAddr, 0, sizeof(op_.clientAddr));
        op_.addrLen = sizeof(op_.clientAddr);
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        io_uring_prep_accept(sqe, listenFd_,
                             reinterpret_cast<sockaddr*>(&op_.clientAddr),
                             &op_.addrLen, 0);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    int await_resume() {
        return op_.result;
    }

private:
    int listenFd_;
    net::IoUring& uring_;
    AcceptOp op_;
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
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &WriteOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        io_uring_prep_shutdown(sqe, fd_, how_);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    int await_resume() {
        return op_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    int how_;
    WriteOp op_;
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
        op_.buf.resize(bufSize);
    }

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &RecvOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        // liburing 2.4 无 io_uring_prep_recvfrom，用 recvmsg 实现：
        // 需要在协程帧（awaitable）上保存 msghdr/iovec，直到完成
        iov_.iov_base = op_.buf.data();
        iov_.iov_len = op_.buf.size();
        msg_.msg_name = &srcAddr_;
        msg_.msg_namelen = sizeof(srcAddr_);
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;

        io_uring_prep_recvmsg(sqe, fd_, &msg_, 0);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    AsyncRecvFromResult await_resume() {
        AsyncRecvFromResult res;
        res.rr.data = std::move(op_.data);
        res.rr.result = op_.result;
        res.srcAddrLen = msg_.msg_namelen;
        res.srcAddr = srcAddr_;
        return res;
    }

private:
    int fd_;
    net::IoUring& uring_;
    RecvOp op_;                       // buf = 接收缓冲，data = 结果
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
        op_.fd = fd_;
        op_.handle = h;
        op_.onComplete = &WriteOp::complete;
        PendingUringOps::instance().add(&op_);

        auto* ring = uring_.ring();
        if (!ring) { abortOp(&op_, -1); return; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) { abortOp(&op_, -1); return; }

        // 需要把 msghdr/iovec 保存在协程帧（awaitable）上直到 sendmsg 完成
        iov_.iov_base = buf_.data();
        iov_.iov_len = buf_.size();
        msg_.msg_name = const_cast<struct sockaddr*>(addr_);
        msg_.msg_namelen = addrLen_;
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;

        io_uring_prep_sendmsg(sqe, fd_, &msg_, 0);
        sqe->user_data = reinterpret_cast<uint64_t>(&op_);
    }

    int await_resume() {
        return op_.result;
    }

private:
    int fd_;
    net::IoUring& uring_;
    const struct sockaddr* addr_;
    socklen_t addrLen_;
    std::vector<uint8_t> buf_;
    struct iovec iov_ {};
    struct msghdr msg_ {};
    WriteOp op_;
};

} // namespace coro
} // namespace vless

#endif // VLESS_CORO_URING_AWAITABLE_H
