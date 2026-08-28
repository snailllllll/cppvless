#include "server/vless_connection.h"
#include "proxy/vless/decoder.h"
#include "proxy/vless/vision.h"
#include "common/log.h"

namespace vless {
namespace server {

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

} // namespace server
} // namespace vless
