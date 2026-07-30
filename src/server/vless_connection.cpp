#include "server/vless_connection.h"
#include "proxy/vless/protocol.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <unistd.h>
#include <sys/socket.h>

#include <exception>

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
