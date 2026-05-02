#include "server/vless_connection.h"
#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"
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
    : clientFd_(clientFd), uring_(uring), stream_(clientFd) {}

VlessConnection::~VlessConnection() {
    if (state_ != State::CLOSED) {
        close();
    }
}

// ── Connection 接口 ──

void VlessConnection::prepareIO(net::IoUring& uring) {
    switch (state_) {
        case State::HANDSHAKE:
            // HANDSHAKE: 每次都检查是否需要读取
            prepareHandshakeIO(uring);
            break;
        case State::CONNECTING:
            // CONNECTING: 只准备一次 connect 操作
            if (needsPrepare_) {
                prepareConnectingIO(uring);
                needsPrepare_ = false;
            }
            break;
        case State::SENDING_RESPONSE:
            // SENDING_RESPONSE: 只准备一次 send 操作
            if (needsPrepare_) {
                prepareSendingResponseIO(uring);
                needsPrepare_ = false;
            }
            break;
        case State::RELAY:
            // RELAY: 每次都准备 recv
            prepareRelayIO(uring);
            break;
        case State::CLOSED:
            break;
    }
}

void VlessConnection::onIOComplete(int fd, int result, net::UringEventType type) {
    switch (state_) {
        case State::HANDSHAKE:
            onHandshakeIOComplete(fd, result);
            break;
        case State::CONNECTING:
            onConnectingIOComplete(fd, result);
            break;
        case State::SENDING_RESPONSE:
            onSendingResponseIOComplete(fd, result);
            break;
        case State::RELAY:
            if (type == net::UringEventType::READ) {
                onRelayIOComplete(fd, result);
            } else if (type == net::UringEventType::WRITE) {
                LOG_DEBUG("VlessConnection", "fd=", clientFd_, " send complete fd=", fd, " result=", result);
            }
            break;
        case State::CLOSED:
            break;
    }
}

bool VlessConnection::isClosed() const {
    return state_ == State::CLOSED;
}

int VlessConnection::primaryFd() const {
    return clientFd_;
}

bool VlessConnection::hasFd(int fd) const {
    return fd == clientFd_ || fd == targetFd_ || fd == connectingFd_;
}

// ── 握手阶段 ──

void VlessConnection::prepareHandshakeIO(net::IoUring& uring) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " prepareHandshakeIO, started=", handshakeStarted_, ", needsRead=", stream_.needsRead());
    if (!handshakeStarted_) {
        handshakeStarted_ = true;
        handshakeTask_ = processHandshake();
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " handshake coroutine started");
        if (!handshakeTask_.done()) {
            handshakeTask_.h.resume();
            LOG_DEBUG("VlessConnection", "fd=", clientFd_, " resumed initial suspend");
        }
    }

    if (stream_.needsRead()) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " preparing RECV");
        uring.prepareRecv(clientFd_, stream_.recvBuffer(),
                          coro::UringBufferedStream::recvBufferSize());
        stream_.clearNeedRead();
    }
}

void VlessConnection::onHandshakeIOComplete(int fd, int result) {
    (void)fd;  // 握手阶段只有 clientFd_ 会完成，无需检查 fd
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " onHandshakeIOComplete, result=", result);
    if (result <= 0) {
        LOG_INFO("VlessConnection", "fd=", clientFd_, " handshake closed/errored, result=", result);
        close();
        return;
    }

    stream_.feed(stream_.recvBuffer(), result);
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " after feed, task.done=", handshakeTask_.done());

    if (handshakeTask_.done()) {
        try {
            pendingRequest_ = handshakeTask_.result();
        } catch (const std::exception& e) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " handshake exception: ", e.what());
            close();
            return;
        }

        // 创建 target socket 并开始异步连接
        startConnecting();
    }
}

void VlessConnection::startConnecting() {
    // 创建 socket
    connectingFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (connectingFd_ < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " socket() failed");
        close();
        return;
    }

    // 设置非阻塞（io_uring 需要）
    int flags = fcntl(connectingFd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(connectingFd_, F_SETFL, flags | O_NONBLOCK);
    }

    // 解析目标地址
    std::memset(&targetAddr_, 0, sizeof(targetAddr_));
    targetAddr_.sin_family = AF_INET;
    targetAddr_.sin_port = htons(pendingRequest_.port);

    if (pendingRequest_.isIPv4()) {
        auto& ip = std::get<std::array<uint8_t, 4>>(pendingRequest_.address);
        std::memcpy(&targetAddr_.sin_addr, ip.data(), 4);
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " target IPv4: ", inet_ntoa(targetAddr_.sin_addr));
        state_ = State::CONNECTING;
        needsPrepare_ = true;
    } else if (pendingRequest_.isDomain()) {
        auto& domain = std::get<std::string>(pendingRequest_.address);
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " resolving domain: ", domain);
        // 注意：getaddrinfo 是同步的，会阻塞事件循环
        // 在生产环境中应使用异步 DNS 解析
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) != 0 || !res) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " getaddrinfo failed for: ", domain);
            ::close(connectingFd_);
            connectingFd_ = -1;
            close();
            return;
        }
        targetAddr_.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " domain resolved to: ", inet_ntoa(targetAddr_.sin_addr));
        state_ = State::CONNECTING;
        needsPrepare_ = true;
    } else {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " IPv6 not supported yet");
        ::close(connectingFd_);
        connectingFd_ = -1;
        close();
        return;
    }
}

coro::Task<proxy::vless::Request> VlessConnection::processHandshake() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " processHandshake START");
    auto req = co_await proxy::vless::Decoder::decode(stream_);
    LOG_INFO("VlessConnection", "fd=", clientFd_, " Handshake target=", req.addressString(), ":", req.port);
    co_return req;
}

// ── 转发阶段 ──

void VlessConnection::prepareRelayIO(net::IoUring& uring) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " prepareRelayIO, pendingSends=", pendingSends_.size());

    for (auto& ps : pendingSends_) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " prepareSend fd=", ps.fd, " len=", ps.len);
        uring.prepareSend(ps.fd, ps.buf, ps.len);
    }
    pendingSends_.clear();

    if (!clientClosed_ && !clientRecvInflight_) {
        uring.prepareRecv(clientFd_, clientRecvBuf_.data(), clientRecvBuf_.size());
        clientRecvInflight_ = true;
    }
    if (!targetClosed_ && !targetRecvInflight_) {
        uring.prepareRecv(targetFd_, targetRecvBuf_.data(), targetRecvBuf_.size());
        targetRecvInflight_ = true;
    }
}

void VlessConnection::onRelayIOComplete(int fd, int result) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " onRelayIOComplete fd=", fd, " result=", result);

    if (fd == clientFd_) {
        clientRecvInflight_ = false;
    } else {
        targetRecvInflight_ = false;
    }

    if (result <= 0) {
        LOG_INFO("VlessConnection", "fd=", clientFd_, " fd=", fd, " closed/errored");
        if (fd == clientFd_) {
            clientClosed_ = true;
        } else {
            targetClosed_ = true;
        }
        if (pendingSends_.empty() && (clientClosed_ || targetClosed_)) {
            LOG_INFO("VlessConnection", "fd=", clientFd_, " both sides done, closing");
            close();
        }
        return;
    }

    if (fd == clientFd_) {
        pendingSends_.push_back({targetFd_, clientRecvBuf_.data(), static_cast<size_t>(result)});
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " queued send to targetFd=", targetFd_, " len=", result);
    } else if (fd == targetFd_) {
        pendingSends_.push_back({clientFd_, targetRecvBuf_.data(), static_cast<size_t>(result)});
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " queued send to clientFd=", clientFd_, " len=", result);
    }
}

// ── 通用 ──

void VlessConnection::close() {
    if (state_ == State::CLOSED) return;

    state_ = State::CLOSED;

    if (targetFd_ >= 0) {
        ::close(targetFd_);
        targetFd_ = -1;
    }
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
}

void VlessConnection::enterRelayState(int targetFd) {
    targetFd_ = targetFd;
    state_ = State::RELAY;
    LOG_INFO("VlessConnection", "fd=", clientFd_, " Entering RELAY state, targetFd=", targetFd);

    auto remaining = stream_.drainRemaining();
    if (!remaining.empty()) {
        handshakeRemaining_.assign(remaining.begin(), remaining.end());
        pendingSends_.push_back({targetFd_, handshakeRemaining_.data(), handshakeRemaining_.size()});
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " Handshake remaining data queued: ", handshakeRemaining_.size(),
                  " bytes -> targetFd=", targetFd);
    }

    // 通知 EventLoop 需要重新调用 prepareIO() 来提交 RELAY 阶段的 I/O
    needsPrepare_ = true;
}

// ── 异步连接阶段 ──

void VlessConnection::prepareConnectingIO(net::IoUring& uring) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " prepareConnectingIO, connectingFd=", connectingFd_);
    if (connectingFd_ >= 0) {
        uring.prepareConnect(connectingFd_, reinterpret_cast<struct sockaddr*>(&targetAddr_), sizeof(targetAddr_));
    }
}

void VlessConnection::onConnectingIOComplete(int fd, int result) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " onConnectingIOComplete, fd=", fd, " result=", result);

    if (fd != connectingFd_) {
        LOG_WARN("VlessConnection", "fd=", clientFd_, " unexpected fd in onConnectingIOComplete: ", fd);
        return;
    }

    if (result < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " connect() failed: ", result, " (", strerror(-result), ")");
        ::close(connectingFd_);
        connectingFd_ = -1;
        close();
        return;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " Connected to target, fd=", connectingFd_);

    // 连接成功，现在发送 VLESS 响应
    targetFd_ = connectingFd_;
    connectingFd_ = -1;
    state_ = State::SENDING_RESPONSE;
    needsPrepare_ = true;
}

// ── 异步发送响应阶段 ──

void VlessConnection::prepareSendingResponseIO(net::IoUring& uring) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " prepareSendingResponseIO");

    auto response = proxy::vless::Decoder::encodeResponse(pendingRequest_.version);
    std::memcpy(responseBuf_.data(), response.data(), response.size());
    responseLen_ = response.size();

    uring.prepareSend(clientFd_, responseBuf_.data(), responseLen_);
}

void VlessConnection::onSendingResponseIOComplete(int fd, int result) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " onSendingResponseIOComplete, fd=", fd, " result=", result);

    if (result < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " send response failed: ", result);
        close();
        return;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " VLESS response sent successfully, starting RELAY");

    // 进入转发状态
    state_ = State::RELAY;
    needsPrepare_ = true;

    // 处理握手阶段剩余的客户端数据
    auto remaining = stream_.drainRemaining();
    if (!remaining.empty()) {
        handshakeRemaining_.assign(remaining.begin(), remaining.end());
        pendingSends_.push_back({targetFd_, handshakeRemaining_.data(), handshakeRemaining_.size()});
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " Handshake remaining data queued: ", handshakeRemaining_.size(),
                  " bytes -> targetFd=", targetFd_);
    }
}

} // namespace server
} // namespace vmess
