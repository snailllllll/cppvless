#include "server/vless_connection.h"
#include "coro/async_stream.h"
#include "common/log.h"

#include <sys/socket.h>
#include <exception>

namespace vmess {
namespace server {

coro::Task<void> VlessConnection::targetTask(int targetFd) {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " targetTask START, targetFd=", targetFd);

    try {
        coro::AsyncStream targetStream(targetFd, uring_);
        coro::AsyncStream clientStream(clientFd_, uring_);

        bool normalEnd = true;
        if (command_ == proxy::vless::Command::UDP) {
            co_await relayUdpTargetToClient();
            normalEnd = true;
        } else if (useEncryption_) {
            while (!closed_) {
                auto rr = co_await targetStream.read();
                if (rr.eof()) {
                    co_await clientStream.shutdownWrite();
                    break;
                }
                if (rr.error()) {
                    LOG_WARN("VlessConnection", "fd=", clientFd_,
                             " target read error in encryption relay: ", rr.result);
                    co_await clientStream.shutdownWrite();
                    normalEnd = false;
                    break;
                }

                std::vector<uint8_t> ciphertext;
                size_t encryptedLen = encryptionSession_->encryptServer(rr.data, ciphertext);
                if (encryptedLen == 0) {
                    LOG_ERROR("VlessConnection", "fd=", clientFd_,
                              " encryption failed");
                    normalEnd = false;
                    break;
                }

                int written = co_await clientStream.writeFull(ciphertext);
                if (written <= 0) {
                    normalEnd = false;
                    break;
                }
            }
        } else if (useVision_) {
            while (!closed_) {
                auto rr = co_await targetStream.read();
                if (rr.eof()) {
                    co_await clientStream.shutdownWrite();
                    break;
                }
                if (rr.error()) {
                    LOG_WARN("VlessConnection", "fd=", clientFd_,
                             " target read error in vision relay: ", rr.result);
                    co_await clientStream.shutdownWrite();
                    normalEnd = false;
                    break;
                }

                auto data = rr.data;
                if (!visionWriter_->directCopy()) {
                    data = visionWriter_->process(data);
                }

                if (!data.empty()) {
                    int written = co_await clientStream.writeFull(data);
                    if (written <= 0) {
                        normalEnd = false;
                        break;
                    }
                }

                if (visionWriter_->directCopy()) {
                    LOG_DEBUG("VisionWriter", "fd=", clientFd_, " switched to direct copy");
                    normalEnd = co_await coro::copyStream(clientStream, targetStream, closed_);
                    break;
                }
            }
        } else {
            normalEnd = co_await coro::copyStream(clientStream, targetStream, closed_);
        }

        targetReadDone_ = true;
        LOG_INFO("VlessConnection", "fd=", clientFd_, " target→client ",
                 (normalEnd ? "EOF (half-close)" : "interrupted"),
                 " clientReadDone=", clientReadDone_, " targetReadDone=", targetReadDone_);

    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " targetTask exception: ", e.what());
    }

    targetReadDone_ = true;

    if (!clientReadDone_ && clientFd_ >= 0) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " interrupting client (shutdown SHUT_RDWR)");
        ::shutdown(clientFd_, SHUT_RDWR);
    }

    if (clientReadDone_ && targetReadDone_) {
        closed_ = true;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " targetTask END");
}

} // namespace server
} // namespace vmess
