#include "server/vless_connection.h"
#include "proxy/vless/protocol.h"
#include "coro/async_stream.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <unistd.h>
#include <sys/socket.h>

#include <exception>

namespace vmess {
namespace server {

VlessConnection::VlessConnection(int clientFd, net::IoUring& uring,
                                 const proxy::vless::Validator& validator,
                                 SSL_CTX* tlsCtx)
    : clientFd_(clientFd), uring_(uring), validator_(validator), tlsCtx_(tlsCtx),
      rawStream_(std::make_unique<coro::AsyncStream>(clientFd, uring)),
      clientStream_(tlsCtx_
                        ? static_cast<net::Stream*>(
                              new net::TlsStream(*rawStream_, tlsCtx_, true))
                        : static_cast<net::Stream*>(rawStream_.get())),
      stream_(*clientStream_) {}

VlessConnection::~VlessConnection() {
    // 明文模式（tlsCtx_ == nullptr）下 clientStream_ 仅是 rawStream_ 的别名
    // （构造时赋值为 rawStream_.get()，不拥有所有权）。成员按声明逆序析构，
    // clientStream_ 先于 rawStream_ 析构，若不先 release 会 double free：
    //   clientStream_ → delete(AsyncStream) → rawStream_ → delete(AsyncStream) 两次
    if (tlsCtx_ == nullptr) {
        clientStream_.release();
    }

    // 无论 closed_ 标记如何，析构时都必须释放 fd。
    // 半关闭路径会先置 closed_=true，若此处跳过 doClose 会泄漏 fd，
    // 最终触发 EMFILE，表现为“连一会儿就不正常”。
    doClose();
}

void VlessConnection::start() {
    if (closed_) return;
    clientTask_ = clientTask();
    if (!clientTask_.done()) {
        clientTask_.h.resume();
    }
}

/**
 * 会话状态机主控：
 *   Handshake -> Dispatch(TCP|UDP) -> Relay -> Cleanup
 *
 * 失败/未实现命令不在此处直接 closed_=true（若 targetTask 已启动会竞态析构）。
 * 统一走 finishClientTask()，按是否已启动对端协程决定立即关闭或半关闭唤醒。
 */
coro::Task<void> VlessConnection::clientTask() {
    LOG_DEBUG("VlessConnection", "fd=", clientFd_, " clientTask START");

    try {
        // Phase 0: TLS 握手（配置了 --tls-port 时，TLS 层先于 VLESS 协议）
        if (tlsCtx_) {
            auto* tlsStream = static_cast<net::TlsStream*>(clientStream_.get());
            if (!co_await tlsStream->handshake()) {
                LOG_ERROR("VlessConnection", "fd=", clientFd_, " TLS handshake failed");
                finishClientTask();
                co_return;
            }
            LOG_INFO("VlessConnection", "fd=", clientFd_, " TLS handshake OK");
        }

        // Phase 1: Handshake
        auto req = co_await processHandshake();
        command_ = req.command;
        LOG_INFO("VlessConnection", "fd=", clientFd_, " Handshake target=",
                 req.addressString(), ":", req.port,
                 " flow=\"", req.flow, "\"",
                 " encryption=\"", req.encryption, "\"",
                 " cmd=", static_cast<int>(req.command));

        // Phase 2: Dispatch
        bool normalEnd = false;
        switch (req.command) {
            case proxy::vless::Command::TCP:
                normalEnd = co_await runTcpSession(req);
                break;
            case proxy::vless::Command::UDP:
                normalEnd = co_await runUdpSession(req);
                break;
            case proxy::vless::Command::Mux:
            case proxy::vless::Command::Rvs:
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " command not implemented yet, cmd=", static_cast<int>(req.command));
                break;
            default:
                LOG_WARN("VlessConnection", "fd=", clientFd_,
                         " unknown command=", static_cast<int>(req.command));
                break;
        }

        // Phase 3: Uplink done
        LOG_INFO("VlessConnection", "fd=", clientFd_, " client→target ",
                 (normalEnd ? "EOF (half-close)" : "interrupted"),
                 " clientReadDone=", true, " targetReadDone=", targetReadDone_);

    } catch (const std::exception& e) {
        LOG_ERROR("VlessConnection", "fd=", clientFd_, " clientTask exception: ", e.what());
    }

    // Phase 4: Cleanup
    finishClientTask();

    LOG_INFO("VlessConnection", "fd=", clientFd_, " clientTask END");
}

/**
 * TCP 会话状态机：
 *   Negotiate(Vision/Encryption) -> Respond -> Connect -> Relay
 *
 * 响应先于出站建连，对齐 Xray inbound；避免慢 DNS/connect 导致客户端 RST。
 */
coro::Task<bool> VlessConnection::runTcpSession(const proxy::vless::Request& req) {
    if (!co_await setupVision(req) || !co_await setupEncryption(req)) {
        co_return false;
    }

    if (!co_await sendResponseAndKey(req.version)) {
        co_return false;
    }

    if (!co_await connectTarget(req)) {
        co_return false;
    }

    startTargetTask(targetFd_);

    if (!co_await forwardHandshakeRemaining()) {
        co_return false;
    }

    co_return co_await relayClientToTarget();
}

/**
 * UDP 会话状态机：
 *   Validate -> Respond -> Connect -> LengthPacketRelay
 *
 * 当前约束：不叠加 Vision / Encryption。
 */
coro::Task<bool> VlessConnection::runUdpSession(const proxy::vless::Request& req) {
    if (!req.flow.empty() || !req.encryption.empty()) {
        LOG_WARN("VlessConnection", "fd=", clientFd_,
                 " UDP currently requires empty flow/encryption");
        co_return false;
    }

    if (!co_await sendResponseAndKey(req.version)) {
        co_return false;
    }

    if (!co_await connectTarget(req)) {
        co_return false;
    }

    startTargetTask(targetFd_);
    co_return co_await relayUdpClientToTarget();
}

void VlessConnection::finishClientTask() {
    clientReadDone_ = true;

    // 对端协程从未启动：本连接可以立刻回收。
    if (!targetTaskStarted_) {
        closed_ = true;
        return;
    }

    // 对端仍在跑：shutdown 唤醒它，等两边都结束后再 closed_。
    if (!targetReadDone_ && targetFd_ >= 0) {
        LOG_DEBUG("VlessConnection", "fd=", clientFd_,
                  " interrupting target (shutdown SHUT_RDWR), targetFd=", targetFd_);
        ::shutdown(targetFd_, SHUT_RDWR);
        return;
    }

    closed_ = true;
}

void VlessConnection::startTargetTask(int targetFd) {
    targetTaskStarted_ = true;
    targetTask_ = targetTask(targetFd);
    if (!targetTask_.done()) {
        targetTask_.h.resume();
    }
}

void VlessConnection::doClose() {
    closed_ = true;

    auto& pending = coro::PendingUringOps::instance();
    if (clientFd_ >= 0) {
        pending.cancelFd(clientFd_);
    }
    if (targetFd_ >= 0) {
        pending.cancelFd(targetFd_);
    }

    if (targetFd_ >= 0) {
        ::close(targetFd_);
        targetFd_ = -1;
    }
    if (clientFd_ >= 0) {
        LOG_INFO("VlessConnection", "fd=", clientFd_, " closing connection");
        ::close(clientFd_);
        clientFd_ = -1;
    }
}

} // namespace server
} // namespace vmess
