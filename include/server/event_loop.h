#ifndef VMESS_SERVER_EVENT_LOOP_H
#define VMESS_SERVER_EVENT_LOOP_H

#include "net/io_uring.h"
#include "server/connection.h"

#include <unordered_map>
#include <memory>
#include <atomic>

namespace vmess {
namespace server {

/**
 * @brief 极致精简的事件循环
 *
 * 核心设计：
 * - 只认识 Connection 接口，不感知任何协议
 * - 每轮迭代：prepareIO -> submit -> onComplete -> cleanup
 */
class EventLoop {
public:
    explicit EventLoop(unsigned int entries = 2048);
    ~EventLoop() = default;

    // 禁用拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 启动事件循环（阻塞）
    void run(uint16_t listenPort);

    // 停止事件循环
    void stop();

private:
    void acceptNewConnection();
    void handleCqe(const net::UringRequest& req, int result, uint32_t flags);
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
