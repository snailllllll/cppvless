#include "server/event_loop.h"
#include "server/vless_connection.h"
#include "coro/uring_awaitable.h"
#include "net/socket.h"
#include "common/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <cstring>

namespace vmess {
namespace server {

EventLoop::EventLoop(unsigned int entries) : uring_(entries) {}

void EventLoop::run(uint16_t listenPort) {
    // 创建监听 socket
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::runtime_error("failed to create listen socket");
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    acceptNewConnection();

    running_ = true;
    LOG_INFO("EventLoop", "Server listening on port ", listenPort);

    while (running_) {
        // 1. 让所有连接准备 I/O（主要是握手阶段的 recv）
        for (auto& [fd, conn] : connections_) {
            conn->prepareIO(uring_);
        }

        // 2. 提交并等待完成
        uring_.submitAndWait(1);

        // 3. 处理完成事件
        uring_.processCompletions([this](const net::UringRequest& req,
                                          int result, uint32_t flags,
                                          uint64_t userData) {
            if (coro::isCoroutineUserData(userData)) {
                // 协程上下文：直接通过 CoroutineRegistry resume
                int fd = coro::userDataFd(userData);
                auto type = coro::userDataType(userData);
                LOG_DEBUG("EventLoop", "Coroutine CQE: fd=", fd,
                          " type=", static_cast<int>(type), " result=", result);
                coro::CoroutineRegistry::instance().takeAndResume(fd, type, result);
            } else {
                // 旧式请求：交给 handleCqe 处理
                handleCqe(req, result, flags, userData);
            }
        });

        // 4. 状态变化的连接需要重新 prepareIO
        bool hasNewIO = false;
        for (auto& [fd, conn] : connections_) {
            if (conn->needsPrepare()) {
                conn->prepareIO(uring_);
                conn->clearNeedsPrepare();
                hasNewIO = true;
            }
        }

        if (hasNewIO) {
            uring_.submitAll();
        }

        // 5. 清理已关闭的连接
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
}

void EventLoop::acceptNewConnection() {
    static sockaddr_in clientAddr;
    static socklen_t addrLen = sizeof(clientAddr);

    uring_.prepareAccept(listenFd_,
                         reinterpret_cast<sockaddr*>(&clientAddr),
                         &addrLen);
}

void EventLoop::handleCqe(const net::UringRequest& req, int result, uint32_t flags,
                           uint64_t rawData) {
    (void)rawData;
    (void)flags;

    switch (static_cast<net::UringEventType>(req.type)) {
        case net::UringEventType::ACCEPT: {
            if (result < 0) {
                LOG_ERROR("EventLoop", "Accept failed: ", result);
                acceptNewConnection();
                return;
            }

            int clientFd = result;
            LOG_INFO("EventLoop", "New connection: fd=", clientFd);

            // 设置非阻塞
            int fl = fcntl(clientFd, F_GETFL, 0);
            if (fl >= 0) {
                fcntl(clientFd, F_SETFL, fl | O_NONBLOCK);
            }

            auto conn = std::make_unique<VlessConnection>(clientFd, uring_);
            connections_[clientFd] = std::move(conn);

            acceptNewConnection();
            break;
        }

        case net::UringEventType::READ:
        case net::UringEventType::WRITE: {
            auto* conn = findConnection(req.fd);
            if (conn) {
                conn->onIOComplete(req.fd, result,
                                   static_cast<net::UringEventType>(req.type));
            } else {
                LOG_DEBUG("EventLoop", "Orphan CQE: no connection for fd=", req.fd);
            }
            break;
        }

        default:
            LOG_DEBUG("EventLoop", "Unhandled CQE type=", req.type);
            break;
    }
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

Connection* EventLoop::findConnection(int fd) {
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        return it->second.get();
    }

    for (auto& [_, conn] : connections_) {
        if (conn->hasFd(fd)) {
            return conn.get();
        }
    }

    return nullptr;
}

} // namespace server
} // namespace vmess
