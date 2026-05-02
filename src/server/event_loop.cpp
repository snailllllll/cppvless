#include "server/event_loop.h"
#include "server/vless_connection.h"
#include "net/socket.h"
#include "common/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

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

    // 设置地址复用
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);
    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to bind listen socket");
    }

    // 监听
    if (::listen(listenFd_, 128) < 0) {
        close(listenFd_);
        throw std::runtime_error("failed to listen");
    }

    // 设置非阻塞
    int flags = fcntl(listenFd_, F_GETFL, 0);
    fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    // 准备第一个 accept
    acceptNewConnection();

    running_ = true;
    LOG_INFO("EventLoop", "Server listening on port ", listenPort);

    while (running_) {
        LOG_DEBUG("EventLoop", "=== loop iteration, connections=", connections_.size(), " ===");
        for (auto& [fd, conn] : connections_) {
            conn->prepareIO(uring_);
        }

        uring_.submitAndWait(1);

        uring_.processCompletions([this](const net::UringRequest& req,
                                          int result, uint32_t flags) {
            handleCqe(req, result, flags);
        });

        cleanupClosedConnections();
    }

    // 清理
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
    // 准备 accept（使用固定缓冲区）
    static sockaddr_in clientAddr;
    static socklen_t addrLen = sizeof(clientAddr);

    uring_.prepareAccept(listenFd_,
                         reinterpret_cast<sockaddr*>(&clientAddr),
                         &addrLen);
}

void EventLoop::handleCqe(const net::UringRequest& req, int result, uint32_t flags) {
    LOG_DEBUG("EventLoop", "CQE type=", req.type, " fd=", req.fd, " result=", result);
    switch (static_cast<net::UringEventType>(req.type)) {
        case net::UringEventType::ACCEPT: {
            if (result < 0) {
                LOG_ERROR("EventLoop", "Accept failed: ", result);
                acceptNewConnection();
                return;
            }

            int clientFd = result;
            LOG_INFO("EventLoop", "New connection: fd=", clientFd);

            auto conn = std::make_unique<VlessConnection>(clientFd, uring_);
            connections_[clientFd] = std::move(conn);

            acceptNewConnection();
            break;
        }

        case net::UringEventType::READ:
        case net::UringEventType::WRITE: {
            auto* conn = findConnection(req.fd);
            if (conn) {
                LOG_DEBUG("EventLoop", "Dispatching to connection for fd=", req.fd, " type=", (int)req.type);
                conn->onIOComplete(req.fd, result, static_cast<net::UringEventType>(req.type));
            } else {
                LOG_DEBUG("EventLoop", "Orphan CQE: no connection for fd=", req.fd, " type=", (int)req.type);
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

    LOG_DEBUG("EventLoop", "findConnection fd=", fd, " not found");
    return nullptr;
}

} // namespace server
} // namespace vmess
