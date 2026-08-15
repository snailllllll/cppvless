#include "server/vless_connection.h"
#include "coro/async_stream.h"
#include "common/log.h"

namespace vmess {
namespace server {

coro::Task<bool> VlessConnection::forwardHandshakeRemaining() {
    if (handshakeRemaining_.empty()) {
        co_return true;
    }

    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " Forwarding handshake remaining: ",
              handshakeRemaining_.size(), " bytes");

    coro::AsyncStream targetStream(targetFd_, uring_);

    if (useEncryption_) {
        std::vector<uint8_t> plaintext;
        if (!encryptionSession_->decryptClient(handshakeRemaining_.data(),
                                                handshakeRemaining_.size(), plaintext)) {
            LOG_ERROR("VlessConnection", "fd=", clientFd_, " decrypt handshake remaining failed");
            co_return false;
        }
        if (!plaintext.empty()) {
            int fwdResult = co_await targetStream.writeFull(plaintext);
            if (fwdResult <= 0) {
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " forward encrypted handshake remaining failed: ", fwdResult);
                co_return false;
            }
        }
    } else if (useVision_) {
        auto unpadded = visionReader_->process(handshakeRemaining_);
        if (!unpadded.empty()) {
            int fwdResult = co_await targetStream.writeFull(unpadded);
            if (fwdResult <= 0) {
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " forward vision handshake remaining failed: ", fwdResult);
                co_return false;
            }
        }
    } else {
        int fwdResult = co_await targetStream.writeFull(handshakeRemaining_);
        if (fwdResult <= 0) {
            LOG_WARN("VlessConnection", "fd=", clientFd_,
                     " forward handshake remaining failed: ", fwdResult);
            co_return false;
        }
    }

    handshakeRemaining_.clear();
    co_return true;
}

coro::Task<bool> VlessConnection::relayClientToTarget() {
    coro::AsyncStream targetStream(targetFd_, uring_);
    net::Stream& clientStream = *clientStream_;
    bool normalEnd = true;

    if (useEncryption_) {
        while (!closed_) {
            auto rr = co_await clientStream.read();
            if (rr.eof()) {
                co_await targetStream.shutdownWrite();
                break;
            }
            if (rr.error()) {
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " client read error in encryption relay: ", rr.result);
                co_await targetStream.shutdownWrite();
                normalEnd = false;
                break;
            }

            std::vector<uint8_t> plaintext;
            if (!encryptionSession_->decryptClient(rr.data.data(), rr.data.size(), plaintext)) {
                LOG_ERROR("VlessConnection", "fd=", clientFd_, " decryption failed");
                normalEnd = false;
                break;
            }

            if (!plaintext.empty()) {
                int written = co_await targetStream.writeFull(plaintext);
                if (written <= 0) {
                    normalEnd = false;
                    break;
                }
            }
        }
    } else if (useVision_) {
        while (!closed_) {
            auto rr = co_await clientStream.read();
            if (rr.eof()) {
                co_await targetStream.shutdownWrite();
                break;
            }
            if (rr.error()) {
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " client read error in vision relay: ", rr.result);
                co_await targetStream.shutdownWrite();
                normalEnd = false;
                break;
            }

            auto data = rr.data;
            if (!visionReader_->directCopy()) {
                data = visionReader_->process(data);
            }

            if (!data.empty()) {
                int written = co_await targetStream.writeFull(data);
                if (written <= 0) {
                    normalEnd = false;
                    break;
                }
            }

            if (visionReader_->directCopy()) {
                LOG_DEBUG("VisionReader", "fd=", clientFd_, " switched to direct copy");
                normalEnd = co_await coro::copyStream(targetStream, clientStream, closed_);
                break;
            }
        }
    } else {
        normalEnd = co_await coro::copyStream(targetStream, clientStream, closed_);
    }

    co_return normalEnd;
}

} // namespace server
} // namespace vmess
