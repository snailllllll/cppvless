#include "server/vless_connection.h"
#include "coro/uring_awaitable.h"
#include "coro/async_stream.h"
#include "proxy/vless/decoder.h"
#include "common/log.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#include <cstring>
#include <string>

namespace vless {
namespace server {

coro::Task<bool> VlessConnection::connectTarget(const proxy::vless::Request& req) {
    int targetFd = createTargetSocket(req);
    if (targetFd < 0) {
        co_return false;
    }

    int connectResult = co_await coro::AsyncConnect(
        targetFd, uring_,
        reinterpret_cast<const struct sockaddr*>(&targetAddr_),
        targetAddrLen_);

    if (connectResult < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " connect failed: ", connectResult,
                  " (", strerror(-connectResult), ")");
        ::close(targetFd);
        co_return false;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " Connected to target, fd=", targetFd);
    targetFd_ = targetFd;
    co_return true;
}

coro::Task<bool> VlessConnection::sendResponseAndKey(uint8_t version) {
    auto response = proxy::vless::Decoder::encodeResponse(version);
    int sendResult = co_await clientStream_->writeFull(response.data(), response.size());
    if (sendResult < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " send response failed: ", sendResult,
                  " (", strerror(-sendResult), ")");
        co_return false;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " VLESS response sent, starting RELAY",
             (useEncryption_ ? " (Encryption)" : (useVision_ ? " (Vision)" : "")));

    if (useEncryption_) {
        const auto& serverPubKey = encryptionSession_->publicKey();
        int sendKeyResult = co_await clientStream_->writeFull(
            std::vector<uint8_t>(serverPubKey.begin(), serverPubKey.end()));
        if (sendKeyResult <= 0) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " failed to send server public key");
            co_return false;
        }
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " sent server public key");

        if (!encryptionSession_->computeSharedSecret(clientPublicKey_.data())) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " failed to compute shared secret");
            co_return false;
        }
        LOG_INFO("VlessConnection", "fd=", clientFd_, " Encryption session established");
    }

    co_return true;
}

int VlessConnection::createTargetSocket(const proxy::vless::Request& req) {
    std::memset(&targetAddr_, 0, sizeof(targetAddr_));
    targetAddrLen_ = 0;

    int family = AF_UNSPEC;
    const bool udpMode = (req.command == proxy::vless::Command::UDP);
    const int socketType = udpMode ? SOCK_DGRAM : SOCK_STREAM;
    int targetFd = -1;

    if (req.isIPv4()) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&targetAddr_);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(req.port);
        auto& ip = std::get<std::array<uint8_t, 4>>(req.address);
        std::memcpy(&v4->sin_addr, ip.data(), 4);
        family = AF_INET;
        targetAddrLen_ = sizeof(sockaddr_in);
    } else if (req.isIPv6()) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&targetAddr_);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(req.port);
        auto& ip = std::get<std::array<uint8_t, 16>>(req.address);
        std::memcpy(&v6->sin6_addr, ip.data(), 16);
        family = AF_INET6;
        targetAddrLen_ = sizeof(sockaddr_in6);
    } else if (req.isDomain()) {
        auto& domain = std::get<std::string>(req.address);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socketType;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(req.port);
        if (getaddrinfo(domain.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " getaddrinfo failed for: ", domain);
            return -1;
        }

        const addrinfo* cur = res;
        while (cur && cur->ai_family != AF_INET && cur->ai_family != AF_INET6) {
            cur = cur->ai_next;
        }
        if (!cur) {
            freeaddrinfo(res);
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " no supported ip for domain: ", domain);
            return -1;
        }

        std::memcpy(&targetAddr_, cur->ai_addr, cur->ai_addrlen);
        targetAddrLen_ = static_cast<socklen_t>(cur->ai_addrlen);
        family = cur->ai_family;
        freeaddrinfo(res);
    } else {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " unsupported request address type");
        return -1;
    }

    targetFd = socket(family, socketType, 0);
    if (targetFd < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " socket() failed");
        return -1;
    }

    int flags = fcntl(targetFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(targetFd, F_SETFL, flags | O_NONBLOCK);
    }

    return targetFd;
}

} // namespace server
} // namespace vless
