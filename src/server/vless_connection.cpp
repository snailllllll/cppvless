#include "server/vless_connection.h"
#include "proxy/vless/protocol.h"
#include "proxy/vless/vision.h"
#include "coro/uring_awaitable.h"
#include "coro/async_stream.h"
#include "common/log.h"

#include <unistd.h>
#include <sys/socket.h>

#include <cstring>

namespace vmess {
namespace server {

VlessConnection::VlessConnection(int clientFd, net::IoUring& uring,
                                 const proxy::vless::Validator& validator)
    : clientFd_(clientFd), uring_(uring), validator_(validator), stream_(clientFd, uring) {}

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
                 req.addressString(), ":", req.port,
                 " flow=\"", req.flow, "\"",
                 " encryption=\"", req.encryption, "\"",
                 " cmd=", static_cast<int>(req.command));
        command_ = req.command;

        if (req.command == proxy::vless::Command::Mux ||
            req.command == proxy::vless::Command::Rvs) {
            LOG_WARN("VlessConnection", "fd=", clientFd_,
                     " command not implemented yet, cmd=", static_cast<int>(req.command));
            closed_ = true;
            co_return;
        }

        if (req.command == proxy::vless::Command::UDP) {
            // 先做 UDP 基础兼容，不与 Vision/Encryption 叠加，避免错语义。
            if (!req.flow.empty() || !req.encryption.empty()) {
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " UDP currently requires empty flow/encryption");
                closed_ = true;
                co_return;
            }
        } else {
            // TCP: 协议协商 Vision / Encryption
            if (!co_await setupVision(req) || !co_await setupEncryption(req)) {
                closed_ = true;
                co_return;
            }
        }

        // 对齐 Xray inbound：先写 VLESS response header，再出站建连。
        // 否则 DNS/connect 变慢时，客户端会在等响应阶段 RST（ECONNRESET）。
        if (!co_await sendResponseAndKey(req.version)) {
            closed_ = true;
            co_return;
        }

        if (!co_await connectTarget(req)) {
            closed_ = true;
            co_return;
        }

        // 启动 target → client 协程
        startTargetTask(targetFd_);

        bool normalEnd = true;
        if (req.command == proxy::vless::Command::UDP) {
            // UDP: 握手剩余字节属于首批 length-packet 数据，由 UDP 中继统一处理。
            normalEnd = co_await relayUdpClientToTarget();
        } else {
            // TCP: 先转发握手阶段剩余数据，再进入流式中继
            if (!co_await forwardHandshakeRemaining()) {
                closed_ = true;
                co_return;
            }
            normalEnd = co_await relayClientToTarget();
        }

        clientReadDone_ = true;
        LOG_INFO("VlessConnection", "fd=", clientFd_, " client→target ",
                 (normalEnd ? "EOF (half-close)" : "interrupted"),
                 " clientReadDone=", clientReadDone_, " targetReadDone=", targetReadDone_);

    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " clientTask exception: ", e.what());
    }

    clientReadDone_ = true;
    finishClientTask();

    LOG_INFO("VlessConnection", "fd=", clientFd_, " clientTask END");
}

// ── 数据转发 ──

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
    coro::AsyncStream clientStream(clientFd_, uring_);
    coro::AsyncStream targetStream(targetFd_, uring_);
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

// ── 清理 ──

void VlessConnection::finishClientTask() {
    if (!targetReadDone_ && targetFd_ >= 0) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_,
                  " interrupting target (shutdown SHUT_RDWR), targetFd=", targetFd_);
        ::shutdown(targetFd_, SHUT_RDWR);
    }

    if (clientReadDone_ && targetReadDone_) {
        closed_ = true;
    }
}

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
            // Encryption 模式：读取 target 数据 → 加密 → 写入 client
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

                // 加密 server → client 数据
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
            // Vision 模式：读取 target 数据 → pad → 写入 client
            normalEnd = true;
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

                // Vision padding
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

                // 一旦 VisionWriter 切换到 direct copy，后续用 copyStream
                if (visionWriter_->directCopy()) {
                    LOG_DEBUG("VisionWriter", "fd=", clientFd_, " switched to direct copy");
                    normalEnd = co_await coro::copyStream(clientStream, targetStream, closed_);
                    break;
                }
            }
        } else {
            // 普通 VLESS：直接 copyStream
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

    // 参考 Xray: 一方完成后，如果另一方还在等，主动中断它
    if (!clientReadDone_ && clientFd_ >= 0) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " interrupting client (shutdown SHUT_RDWR)");
        ::shutdown(clientFd_, SHUT_RDWR);
    }

    // 如果两个方向都完成了，标记连接完全关闭
    if (clientReadDone_ && targetReadDone_) {
        closed_ = true;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " targetTask END");
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
