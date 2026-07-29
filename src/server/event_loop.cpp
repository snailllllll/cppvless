#include "server/event_loop.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

namespace vmess {
namespace server {

EventLoop::EventLoop(const proxy::vless::Validator& validator, unsigned int entries)
    : uring_(entries), validator_(validator) {}

void EventLoop::run(uint16_t listenPort, bool enableReusePort) {
    // 创建监听 socket
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::runtime_error("failed to create listen socket");
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (enableReusePort) {
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);
    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to bind listen socket");
    }

    if (::listen(listenFd_, 128) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to listen");
    }

    int flags = fcntl(listenFd_, F_GETFL, 0);
    fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    running_ = true;
    LOG_INFO("EventLoop", "Server listening on port ", listenPort);

    // 启动 Accept 协程
    acceptTask_ = acceptLoop();
    if (!acceptTask_.done()) {
        acceptTask_.h.resume();
    }

    // 主事件循环
    while (running_) {
        uring_.submitAndWait(1);

        // 处理完成事件：全部走协程路径
        uring_.processCompletions([](const net::UringRequest& /*req*/,
                                      int result, uint32_t /*flags*/,
                                      uint64_t userData) {
            if (coro::isCoroutineUserData(userData)) {
                int fd = coro::userDataFd(userData);
                auto type = coro::userDataType(userData);
                LOG_DEBUG("EventLoop", "Coroutine CQE: fd=", fd,
                          " type=", static_cast<int>(type), " result=", result);
                //单例模式
                coro::CoroutineRegistry::instance().takeAndResume(fd, type, result);
            }
        });

        cleanupClosedConnections();
    }

    connections_.clear();
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
}

void EventLoop::stop() {
    running_ = false;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

coro::Task<void> EventLoop::acceptLoop() {
    LOG_INFO("EventLoop", "Accept loop started");

    while (running_) {
        int clientFd = co_await coro::AsyncAccept(listenFd_, uring_);

        if (clientFd < 0) {
            if (!running_) {
                break;
            }
            LOG_ERROR("EventLoop", "Accept failed: ", clientFd);
            continue;
        }

        LOG_INFO("EventLoop", "New connection: fd=", clientFd);

        // 设置非阻塞
        int fl = fcntl(clientFd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(clientFd, F_SETFL, fl | O_NONBLOCK);
        }

        // 创建连接并启动
        auto conn = std::make_unique<VlessConnection>(clientFd, uring_, validator_);
        conn->start();
        connections_[clientFd] = std::move(conn);
    }

    LOG_INFO("EventLoop", "Accept loop ended");
}

void EventLoop::cleanupClosedConnections() {
    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if (it->second->isClosed()) {
            LOG_INFO("EventLoop", "Connection closed: fd=", it->first);
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace server
} // namespace vmess
