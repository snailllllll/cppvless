#include "server/vless_connection.h"
#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#include <stdexcept>
#include <cstring>

namespace vmess {
namespace server {

VlessConnection::VlessConnection(int clientFd, net::IoUring& uring)
    : clientFd_(clientFd), uring_(uring), stream_(clientFd, uring) {}

VlessConnection::~VlessConnection() {
    // 先清除 CoroutineRegistry 中所有相关的注册
    // 必须在 ~Task() 之前做，否则协程帧被销毁后 registry 中的 handle 变成悬空指针
    auto& registry = coro::CoroutineRegistry::instance();
    if (clientFd_ >= 0) {
        registry.eraseAll(clientFd_);
    }
    if (targetFd_ >= 0) {
        registry.eraseAll(targetFd_);
    }

    if (!closed_) {
        doClose();
    }
}

void VlessConnection::start() {
    if (closed_) return;
    clientTask_ = clientTask();
    // Task 的 initial_suspend 是 suspend_always，需要手动 resume 启动
    if (!clientTask_.done()) {
        clientTask_.h.resume();
    }
}

// ── 协程 ──

coro::Task<void> VlessConnection::clientTask() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " clientTask START");

    try {
        // 1. 握手（纯协程：stream_.read 内部 co_await AsyncRecv）
        auto req = co_await processHandshake();
        LOG_INFO("VlessConnection", "fd=", clientFd_, " Handshake target=",
                 req.addressString(), ":", req.port);

        // 2. 创建 target socket 并异步连接
        int targetFd = createTargetSocket(req);
        if (targetFd < 0) {
            closed_ = true;
            co_return;
        }

        int connectResult = co_await coro::AsyncConnect(
            targetFd, uring_,
            reinterpret_cast<const struct sockaddr*>(&targetAddr_),
            sizeof(targetAddr_));

        if (connectResult < 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " connect failed: ", connectResult,
                      " (", strerror(-connectResult), ")");
            ::close(targetFd);
            closed_ = true;
            co_return;
        }

        LOG_INFO("VlessConnection", "fd=", clientFd_, " Connected to target, fd=", targetFd);
        targetFd_ = targetFd;

        // 3. 发送 VLESS 响应头
        auto response = proxy::vless::Decoder::encodeResponse(req.version);
        int sendResult = co_await coro::AsyncSend(clientFd_, uring_,
                                                   response.data(), response.size());
        if (sendResult < 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " send response failed: ", sendResult);
            closed_ = true;
            co_return;
        }

        LOG_INFO("VlessConnection", "fd=", clientFd_, " VLESS response sent, starting RELAY");

        // 4. 启动 target → client 协程
        startTargetTask(targetFd);

        // 5. 转发握手阶段剩余数据
        if (!handshakeRemaining_.empty()) {
            int fwdResult = co_await coro::AsyncSend(targetFd_, uring_,
                                                      handshakeRemaining_.data(),
                                                      handshakeRemaining_.size());
            if (fwdResult <= 0) {
                LOG_WARN("VlessConnection", "fd=", clientFd_, " forward handshake remaining failed");
                closed_ = true;
                co_return;
            }
            LOG_DEBUG("VlessConnection", "fd=", clientFd_, " Forwarded handshake remaining: ",
                      handshakeRemaining_.size(), " bytes");
            handshakeRemaining_.clear();
        }

        // 6. client → target 转发循环
        while (!closed_) {
            auto data = co_await coro::AsyncRecv(clientFd_, uring_);
            if (data.empty()) {
                LOG_INFO("VlessConnection", "fd=", clientFd_, " client read EOF");
                break;
            }
            int sent = co_await coro::AsyncSend(targetFd_, uring_,
                                                 data.data(), data.size());
            if (sent <= 0) {
                LOG_INFO("VlessConnection", "fd=", clientFd_, " send to target failed: ", sent);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " clientTask exception: ", e.what());
    }

    // 通知另一个协程退出
    closed_ = true;
    LOG_INFO("VlessConnection", "fd=", clientFd_, " clientTask END");
}

coro::Task<void> VlessConnection::targetTask(int targetFd) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " targetTask START, targetFd=", targetFd);

    try {
        while (!closed_) {
            auto data = co_await coro::AsyncRecv(targetFd, uring_);
            if (data.empty()) {
                LOG_INFO("VlessConnection", "fd=", clientFd_, " target read EOF, targetFd=", targetFd);
                break;
            }
            int sent = co_await coro::AsyncSend(clientFd_, uring_,
                                                 data.data(), data.size());
            if (sent <= 0) {
                LOG_INFO("VlessConnection", "fd=", clientFd_, " send to client failed: ", sent);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " targetTask exception: ", e.what());
    }

    // 通知另一个协程退出
    closed_ = true;
    LOG_INFO("VlessConnection", "fd=", clientFd_, " targetTask END");
}

// ── 握手子流程 ──

coro::Task<proxy::vless::Request> VlessConnection::processHandshake() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " processHandshake START");
    auto req = co_await proxy::vless::Decoder::decode(stream_);

    auto remaining = stream_.drainRemaining();
    if (!remaining.empty()) {
        handshakeRemaining_ = std::move(remaining);
    }

    co_return req;
}

int VlessConnection::createTargetSocket(const proxy::vless::Request& req) {
    int targetFd = socket(AF_INET, SOCK_STREAM, 0);
    if (targetFd < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " socket() failed");
        return -1;
    }

    int flags = fcntl(targetFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(targetFd, F_SETFL, flags | O_NONBLOCK);
    }

    memset(&targetAddr_, 0, sizeof(targetAddr_));
    targetAddr_.sin_family = AF_INET;
    targetAddr_.sin_port = htons(req.port);

    if (req.isIPv4()) {
        auto& ip = std::get<std::array<uint8_t, 4>>(req.address);
        memcpy(&targetAddr_.sin_addr, ip.data(), 4);
    } else if (req.isDomain()) {
        auto& domain = std::get<std::string>(req.address);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) != 0 || !res) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " getaddrinfo failed for: ", domain);
            ::close(targetFd);
            return -1;
        }
        targetAddr_.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    } else {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " IPv6 not supported yet");
        ::close(targetFd);
        return -1;
    }

    return targetFd;
}

void VlessConnection::startTargetTask(int targetFd) {
    targetTask_ = targetTask(targetFd);

    if (!targetTask_.done()) {
        targetTask_.h.resume();
    }
}

// ── 通用 ──

void VlessConnection::doClose() {
    closed_ = true;

    LOG_INFO("VlessConnection", "fd=", clientFd_, " closing connection");

    // 清除 CoroutineRegistry 中与这两个 fd 相关的所有注册
    auto& registry = coro::CoroutineRegistry::instance();
    if (clientFd_ >= 0) {
        registry.eraseAll(clientFd_);
    }
    if (targetFd_ >= 0) {
        registry.eraseAll(targetFd_);
    }

    if (targetFd_ >= 0) {
        ::close(targetFd_);
        targetFd_ = -1;
    }
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
}

} // namespace server
} // namespace vmess
