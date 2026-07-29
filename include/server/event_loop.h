#ifndef VMESS_SERVER_EVENT_LOOP_H
#define VMESS_SERVER_EVENT_LOOP_H

#include "net/io_uring.h"
#include "server/vless_connection.h"
#include "proxy/vless/validator.h"

#include <unordered_map>
#include <memory>
#include <atomic>

namespace vmess {
namespace server {

/**
 * @brief 事件循环（纯协程版本）
 *
 * 核心设计：
 * - 所有 CQE 都通过 CoroutineRegistry resume 协程
 * - Accept 也走协程（co_await AsyncAccept）
 * - 不再有旧式 UringRequest 回调路径
 */
class EventLoop {
public:
    explicit EventLoop(const proxy::vless::Validator& validator, unsigned int entries = 2048);
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
    const proxy::vless::Validator& validator_;
    std::unordered_map<int, std::unique_ptr<VlessConnection>> connections_;
    std::atomic<bool> running_{false};
    int listenFd_ = -1;

    coro::Task<void> acceptTask_;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_EVENT_LOOP_H
