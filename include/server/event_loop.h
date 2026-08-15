#ifndef VMESS_SERVER_EVENT_LOOP_H
#define VMESS_SERVER_EVENT_LOOP_H

#include "net/io_uring.h"
#include "server/connection.h"
#include "coro/task.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <atomic>

namespace vmess {
namespace server {

/**
 * @brief 事件循环（纯协程版本，工厂模式）
 *
 * 核心设计：
 * - 所有 CQE 都通过 UringOp 指针直分发 resume 协程
 * - Accept 也走协程（co_await AsyncAccept）
 * - 连接通过 ConnectionFactory 创建，因此服务端（VLESS）与
 *   客户端（SOCKS5）可共用同一个事件循环
 */
class EventLoop {
public:
    /// 连接工厂：给定 accept 得到的 fd，创建对应的协议连接
    using ConnectionFactory =
        std::function<std::unique_ptr<EventLoopConnection>(int fd, net::IoUring&)>;

    explicit EventLoop(ConnectionFactory factory, unsigned int entries = 2048);
    ~EventLoop() = default;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run(uint16_t listenPort, bool enableReusePort = false);
    void stop();

private:
    /// Accept 协程：循环 co_await AsyncAccept 接受新连接
    coro::Task<void> acceptLoop();

    void cleanupClosedConnections();

    net::IoUring uring_;
    ConnectionFactory factory_;
    std::unordered_map<int, std::unique_ptr<EventLoopConnection>> connections_;
    std::atomic<bool> running_{false};
    int listenFd_ = -1;

    coro::Task<void> acceptTask_;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_EVENT_LOOP_H
