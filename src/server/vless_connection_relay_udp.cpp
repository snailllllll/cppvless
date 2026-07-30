#include "server/vless_connection.h"
#include "coro/async_stream.h"
#include "common/log.h"

namespace vmess {
namespace server {

coro::Task<bool> VlessConnection::relayUdpClientToTarget() {
    coro::AsyncStream clientStream(clientFd_, uring_);
    coro::AsyncStream targetStream(targetFd_, uring_);
    bool normalEnd = true;

    std::vector<uint8_t> pending = std::move(handshakeRemaining_);
    handshakeRemaining_.clear();

    while (!closed_) {
        size_t offset = 0;
        while (pending.size() - offset >= 2) {
            uint16_t pktLen = (static_cast<uint16_t>(pending[offset]) << 8) |
                              static_cast<uint16_t>(pending[offset + 1]);
            if (pending.size() - offset < static_cast<size_t>(2 + pktLen)) {
                break;
            }
            if (pktLen > 0) {
                int written = co_await targetStream.writeFull(
                    pending.data() + offset + 2, pktLen);
                if (written <= 0) {
                    normalEnd = false;
                    co_return normalEnd;
                }
            }
            offset += 2 + pktLen;
        }

        if (offset > 0) {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
            continue;
        }

        auto rr = co_await clientStream.read();
        if (rr.eof()) {
            break;
        }
        if (rr.error()) {
            LOG_WARN("VlessConnection", "fd=", clientFd_,
                     " client read error in udp relay: ", rr.result);
            normalEnd = false;
            break;
        }

        pending.insert(pending.end(), rr.data.begin(), rr.data.end());
    }

    co_return normalEnd;
}

coro::Task<void> VlessConnection::relayUdpTargetToClient() {
    coro::AsyncStream targetStream(targetFd_, uring_);
    coro::AsyncStream clientStream(clientFd_, uring_);

    while (!closed_) {
        auto rr = co_await targetStream.read(65536);
        if (rr.eof()) {
            co_return;
        }
        if (rr.error()) {
            LOG_WARN("VlessConnection", "fd=", clientFd_,
                     " target read error in udp relay: ", rr.result);
            co_return;
        }

        if (rr.data.size() > 0xFFFF) {
            LOG_WARN("VlessConnection", "fd=", clientFd_,
                     " udp datagram too large for vless length packet: ", rr.data.size());
            continue;
        }

        std::vector<uint8_t> framed;
        framed.reserve(2 + rr.data.size());
        uint16_t len = static_cast<uint16_t>(rr.data.size());
        framed.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        framed.push_back(static_cast<uint8_t>(len & 0xFF));
        framed.insert(framed.end(), rr.data.begin(), rr.data.end());

        int written = co_await clientStream.writeFull(framed);
        if (written <= 0) {
            co_return;
        }
    }
}

} // namespace server
} // namespace vmess
