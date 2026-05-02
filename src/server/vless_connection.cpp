#include "server/vless_connection.h"
#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"
#include "common/log.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

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
            prepareHandshakeIO(uring);
            break;
        case State::RELAY:
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
    return fd == clientFd_ || fd == targetFd_;
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
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " onHandshakeIOComplete, result=", result);
    if (result <= 0) {
        LOG_INFO("VlessConnection", "fd=", clientFd_, " handshake closed/errored, result=", result);
        close();
        return;
    }

    stream_.feed(stream_.recvBuffer(), result);
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " after feed, task.done=", handshakeTask_.done());

    if (handshakeTask_.done()) {
        int targetFd = -1;
        try {
            targetFd = handshakeTask_.result();
        } catch (const std::exception& e) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " handshake exception: ", e.what());
            close();
            return;
        }

        if (targetFd < 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " handshake returned invalid fd");
            close();
            return;
        }

        enterRelayState(targetFd);
    }
}

coro::Task<int> VlessConnection::processHandshake() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " processHandshake START");
    try {
        auto req = co_await proxy::vless::Decoder::decode(stream_);

        LOG_INFO("VlessConnection", "fd=", clientFd_, " Handshake target=", req.addressString(), ":", req.port);

        int targetFd = socket(AF_INET, SOCK_STREAM, 0);
        if (targetFd < 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " socket() failed");
            co_return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(req.port);

        if (req.isIPv4()) {
            auto& ip = std::get<std::array<uint8_t, 4>>(req.address);
            std::memcpy(&addr.sin_addr, ip.data(), 4);
        } else if (req.isDomain()) {
            auto& domain = std::get<std::string>(req.address);
            LOG_DEBUG("VlessConnection", "fd=", clientFd_, " resolving domain: ", domain);
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) != 0 || !res) {
                LOG_ERROR("VlessConnection", "fd=", clientFd_, " getaddrinfo failed for: ", domain);
                ::close(targetFd);
                co_return -1;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
            LOG_DEBUG("VlessConnection", "fd=", clientFd_, " domain resolved to: ", inet_ntoa(addr.sin_addr));
        } else {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " IPv6 not supported yet");
            ::close(targetFd);
            co_return -1;
        }

        if (::connect(targetFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " connect() failed: ", errno);
            ::close(targetFd);
            co_return -1;
        }

        LOG_INFO("VlessConnection", "fd=", clientFd_, " Connected to target, fd=", targetFd);

        auto response = proxy::vless::Decoder::encodeResponse(req.version);
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " sending VLESS response, len=", response.size());
        ssize_t sent = send(clientFd_, response.data(), response.size(), 0);
        if (sent != static_cast<ssize_t>(response.size())) {
            LOG_WARN("VlessConnection", "fd=", clientFd_, " send response failed: ", sent, " errno=", errno);
            ::close(targetFd);
            co_return -1;
        }
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " VLESS response sent successfully");

        co_return targetFd;
    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " Handshake failed: ", e.what());
        co_return -1;
    }
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
}

} // namespace server
} // namespace vmess
