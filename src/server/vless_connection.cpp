#include "server/vless_connection.h"
#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"
#include "proxy/vless/vision.h"
#include "coro/uring_awaitable.h"
#include "coro/async_stream.h"
#include "common/log.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

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

// ── 协议协商 ──

coro::Task<bool> VlessConnection::setupVision(const proxy::vless::Request& req) {
    if (req.flow == proxy::vless::VLESS_FLOW_VISION) {
        useVision_ = true;
        visionCtx_ = std::make_shared<proxy::vless::VisionContext>();
        visionReader_ = std::make_unique<proxy::vless::VisionReader>(req.uuid, visionCtx_);
        visionWriter_ = std::make_unique<proxy::vless::VisionWriter>(req.uuid, visionCtx_);
        LOG_INFO("VlessConnection", "fd=", clientFd_, " Vision (xtls-rprx-vision) enabled");
        co_return true;
    }

    if (!req.flow.empty()) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_,
                  " unsupported flow=\"", req.flow, "\", closing connection");
        co_return false;
    }

    co_return true;
}

coro::Task<bool> VlessConnection::setupEncryption(const proxy::vless::Request& req) {
    if (req.encryption.empty()) {
        co_return true;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_,
             " Encryption requested: \"", req.encryption, "\"");

    proxy::vless::EncryptionMethod method;
    if (req.encryption == "aes-256-gcm" || req.encryption == "aes256gcm") {
        method = proxy::vless::EncryptionMethod::X25519_AES256GCM;
    } else if (req.encryption == "chacha20-poly1305" || req.encryption == "chacha20") {
        method = proxy::vless::EncryptionMethod::X25519_Chacha20;
    } else {
        LOG_ERROR("VlessConnection", "fd=", clientFd_,
                  " unsupported encryption=\"", req.encryption, "\"");
        co_return false;
    }

    encryptionSession_ = std::make_unique<proxy::vless::EncryptionSession>(method);
    useEncryption_ = true;

    auto clientPubKeyBytes = co_await stream_.read(32);
    if (clientPubKeyBytes.size() != 32) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " failed to read client public key");
        co_return false;
    }
    clientPublicKey_.assign(clientPubKeyBytes.begin(), clientPubKeyBytes.end());
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " received client public key");

    encryptionSession_->generateKeyPair();
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " generated server key pair");

    co_return true;
}

// ── 连接建立 ──

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
    int sendResult = co_await coro::AsyncSend(clientFd_, uring_,
                                               response.data(), response.size());
    if (sendResult < 0) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " send response failed: ", sendResult,
                  " (", strerror(-sendResult), ")");
        co_return false;
    }

    LOG_INFO("VlessConnection", "fd=", clientFd_, " VLESS response sent, starting RELAY",
             (useEncryption_ ? " (Encryption)" : (useVision_ ? " (Vision)" : "")));

    if (useEncryption_) {
        const auto& serverPubKey = encryptionSession_->publicKey();
        coro::AsyncStream encStream(clientFd_, uring_);
        int sendKeyResult = co_await encStream.writeFull(
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

// ── 握手子流程 ──

coro::Task<proxy::vless::Request> VlessConnection::processHandshake() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " processHandshake START");
    auto req = co_await proxy::vless::Decoder::decode(stream_, validator_);

    auto remaining = stream_.drainRemaining();
    if (!remaining.empty()) {
        handshakeRemaining_ = std::move(remaining);
        LOG_DEBUG("VlessConnection", "fd=", clientFd_, " handshake remaining: ",
                  handshakeRemaining_.size(), " bytes");
    }

    co_return req;
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
