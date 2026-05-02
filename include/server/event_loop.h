#ifndef VMESS_SERVER_EVENT_LOOP_H
#define VMESS_SERVER_EVENT_LOOP_H

#include "net/io_uring.h"
#include "coro/uring_awaitable.h"
#include "server/connection.h"

#include <unordered_map>
#include <memory>
#include <atomic>

namespace vmess {
namespace server {

/**
 * @brief 事件循环（统一协程版本）
 *
 * 核心设计：
 * - 协程上下文的 CQE 由 CoroutineRegistry 直接 resume
 * - 握手阶段的 recv CQE 仍由 Connection::onIOComplete 处理
 * - Accept 仍由旧式 UringRequest 处理
 */
class EventLoop {
public:
    explicit EventLoop(unsigned int entries = 2048);
    ~EventLoop() = default;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run(uint16_t listenPort);
    void stop();

private:
    void acceptNewConnection();
    void handleCqe(const net::UringRequest& req, int result, uint32_t flags,
                   uint64_t rawData);
    void cleanupClosedConnections();
    Connection* findConnection(int fd);

    net::IoUring uring_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::atomic<bool> running_{false};
    int listenFd_ = -1;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_EVENT_LOOP_H
