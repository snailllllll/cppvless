#include "client/socks5_connection.h"
#include "proxy/socks5/socks5.h"
#include "proxy/vless/protocol.h"
#include "coro/async_stream.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <exception>

namespace vmess {
namespace client {

Socks5Connection::Socks5Connection(int appFd, net::IoUring& uring,
                                   const VlessClientConfig& cfg)
    : appFd_(appFd), uring_(uring), cfg_(cfg),
      appStream_(std::make_unique<coro::AsyncStream>(appFd, uring)),
      stream_(*appStream_) {}

Socks5Connection::~Socks5Connection() {
    doClose();
}

void Socks5Connection::start() {
    if (closed_) return;
    clientTask_ = clientTask();
    if (!clientTask_.done()) {
        clientTask_.h.resume();
    }
}

bool Socks5Connection::hasFd(int fd) const {
    if (fd == appFd_ || fd == remoteFd_) return true;
    if (udpRelay_ && udpRelay_->hasFd(fd)) return true;
    return false;
}

/**
 * 会话状态机主控：
 *   Greeting -> Request -> Dispatch(CONNECT | UDP ASSOCIATE) -> Relay -> Cleanup
 */
coro::Task<void> Socks5Connection::clientTask() {
    LOG_INFO("Socks5Connection", "fd=", appFd_, " clientTask START");

    try {
        // Phase 1: SOCKS5 Greeting（仅支持无认证）
        bool noAuth = co_await proxy::socks5::Parser::readGreeting(stream_);
        if (!noAuth) {
            LOG_WARN("Socks5Connection", "fd=", appFd_,
                     " no acceptable auth method (only no-auth supported)");
            net::Stream& appStream = *appStream_;
            std::vector<uint8_t> refuse = {0x05, 0xFF};
            co_await appStream.writeFull(refuse);
            finishClientTask();
            co_return;
        }

        net::Stream& appStream = *appStream_;
        int g = co_await appStream.writeFull(proxy::socks5::Parser::encodeGreetingResponse());
        if (g <= 0) {
            finishClientTask();
            co_return;
        }

        // Phase 2: 读取 SOCKS5 请求
        auto req = co_await proxy::socks5::Parser::readRequest(stream_);
        LOG_INFO("Socks5Connection", "fd=", appFd_,
                 " request cmd=", static_cast<int>(req.cmd),
                 " target=", req.address.toString(), ":", req.port);

        // Phase 3: Dispatch
        switch (req.cmd) {
            case proxy::socks5::Command::Connect:
                co_await runTcpSession(req);
                break;
            case proxy::socks5::Command::UdpAssociate:
                co_await runUdpAssociate(req);
                break;
            default: {
                LOG_WARN("Socks5Connection", "fd=", appFd_,
                         " unsupported command=", static_cast<int>(req.cmd));
                co_await sendSocksReply(proxy::socks5::Reply::CommandNotSupported,
                                        "0.0.0.0", 0);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Socks5Connection", "fd=", appFd_, " clientTask exception: ", e.what());
    }

    finishClientTask();
    LOG_INFO("Socks5Connection", "fd=", appFd_, " clientTask END");
}

/**
 * TCP 会话状态机：
 *   ConnectRemote -> VLESS Handshake -> SOCKS5 Reply -> Relay
 */
coro::Task<void> Socks5Connection::runTcpSession(const proxy::socks5::Request& req) {
    auto vlessReq = toVlessRequest(req, proxy::vless::Command::TCP);

    // 建立到远端 VLESS 服务器的隧道
    auto handshake = co_await vlessConnectAndHandshake(uring_, cfg_, vlessReq);
    if (handshake.remoteFd < 0) {
        LOG_ERROR("Socks5Connection", "fd=", appFd_,
                  " failed to establish vless tunnel to ",
                  cfg_.remoteHost, ":", cfg_.remotePort);
        co_await sendSocksReply(proxy::socks5::Reply::ConnectionRefused, "0.0.0.0", 0);
        co_return;
    }
    remoteFd_ = handshake.remoteFd;

    // 隧道建立成功，告知应用
    if (!co_await sendSocksReply(proxy::socks5::Reply::Success, "0.0.0.0", 0)) {
        co_return;
    }
    LOG_INFO("Socks5Connection", "fd=", appFd_,
             " socks5 connect ok, target=", req.address.toString(), ":", req.port,
             " remoteFd=", remoteFd_);

    // 转发握手阶段多读出的远端数据
    if (!handshake.remaining.empty()) {
        net::Stream& appStream = *appStream_;
        int fwd = co_await appStream.writeFull(handshake.remaining);
        if (fwd <= 0) {
            LOG_WARN("Socks5Connection", "fd=", appFd_,
                     " forward handshake remaining failed: ", fwd);
            co_return;
        }
    }

    // 启动对端协程，双向中继
    startRemoteTask(remoteFd_);

    net::Stream& appStream = *appStream_;
    coro::AsyncStream remoteStream(remoteFd_, uring_);
    co_await coro::copyStream(remoteStream, appStream, closed_);
    LOG_INFO("Socks5Connection", "fd=", appFd_, " app→remote relay finished");
}

/**
 * UDP ASSOCIATE 会话状态机：
 *   BindUdp -> SOCKS5 Reply(BND) -> Socks5UdpRelay -> 等待控制连接关闭
 */
coro::Task<void> Socks5Connection::runUdpAssociate(const proxy::socks5::Request& req) {
    (void)req;

    // 创建并绑定本地 UDP socket
    int udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd < 0) {
        LOG_ERROR("Socks5Connection", "fd=", appFd_, " udp socket() failed");
        co_await sendSocksReply(proxy::socks5::Reply::GeneralFailure, "0.0.0.0", 0);
        co_return;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只监听本地
    bindAddr.sin_port = 0;  // 内核分配端口
    if (bind(udpFd, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
        LOG_ERROR("Socks5Connection", "fd=", appFd_, " udp bind() failed");
        ::close(udpFd);
        co_await sendSocksReply(proxy::socks5::Reply::GeneralFailure, "0.0.0.0", 0);
        co_return;
    }

    sockaddr_in actualAddr{};
    socklen_t actualLen = sizeof(actualAddr);
    getsockname(udpFd, reinterpret_cast<sockaddr*>(&actualAddr), &actualLen);
    uint16_t bndPort = ntohs(actualAddr.sin_port);

    int flags = fcntl(udpFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(udpFd, F_SETFL, flags | O_NONBLOCK);
    }

    // 告知应用 UDP 数据报发往哪里
    if (!co_await sendSocksReply(proxy::socks5::Reply::Success, "127.0.0.1", bndPort)) {
        ::close(udpFd);
        co_return;
    }
    LOG_INFO("Socks5Connection", "fd=", appFd_,
             " udp associate ok, bnd=127.0.0.1:", bndPort);

    // 启动 UDP 中继
    udpRelay_ = std::make_unique<Socks5UdpRelay>(udpFd, uring_, cfg_);
    udpRelay_->start();

    // 等待应用关闭 TCP 控制连接（EOF）以结束关联
    while (!closed_) {
        auto rr = co_await coro::AsyncRecv(appFd_, uring_);
        if (rr.eof() || rr.error()) {
            break;
        }
        // 控制连接上的数据按规范应忽略
    }

    udpRelay_->stop();
    LOG_INFO("Socks5Connection", "fd=", appFd_, " udp associate finished");
}

/**
 * 远端侧协程：remote → app 转发（半关闭传播）
 */
coro::Task<void> Socks5Connection::remoteTask(int remoteFd) {
    LOG_DEBUG("Socks5Connection", "fd=", appFd_, " remoteTask START, remoteFd=", remoteFd);

    try {
        net::Stream& appStream = *appStream_;
        coro::AsyncStream remoteStream(remoteFd, uring_);
        co_await coro::copyStream(appStream, remoteStream, closed_);
    } catch (const std::exception& e) {
        LOG_ERROR("Socks5Connection", "fd=", appFd_, " remoteTask exception: ", e.what());
    }

    remoteReadDone_ = true;
    LOG_INFO("Socks5Connection", "fd=", appFd_, " remote→app relay finished");

    if (!appReadDone_ && appFd_ >= 0) {
        LOG_DEBUG("Socks5Connection", "fd=", appFd_, " interrupting app (shutdown SHUT_RDWR)");
        ::shutdown(appFd_, SHUT_RDWR);
    }

    if (appReadDone_ && remoteReadDone_) {
        closed_ = true;
    }

    LOG_DEBUG("Socks5Connection", "fd=", appFd_, " remoteTask END");
}

void Socks5Connection::startRemoteTask(int remoteFd) {
    remoteTaskStarted_ = true;
    remoteTask_ = remoteTask(remoteFd);
    if (!remoteTask_.done()) {
        remoteTask_.h.resume();
    }
}

coro::Task<bool> Socks5Connection::sendSocksReply(proxy::socks5::Reply reply,
                                                  const std::string& bindIp,
                                                  uint16_t bindPort) {
    auto bytes = proxy::socks5::Parser::encodeReply(reply, bindIp, bindPort);
    net::Stream& appStream = *appStream_;
    int written = co_await appStream.writeFull(bytes);
    co_return written > 0;
}

void Socks5Connection::finishClientTask() {
    appReadDone_ = true;

    // 对端协程从未启动：本连接可以立刻回收
    if (!remoteTaskStarted_) {
        closed_ = true;
        return;
    }

    // 对端仍在跑：shutdown 唤醒它，等两边都结束后再 closed_
    if (!remoteReadDone_ && remoteFd_ >= 0) {
        LOG_DEBUG("Socks5Connection", "fd=", appFd_,
                  " interrupting remote (shutdown SHUT_RDWR), remoteFd=", remoteFd_);
        ::shutdown(remoteFd_, SHUT_RDWR);
        return;
    }

    closed_ = true;
}

void Socks5Connection::doClose() {
    closed_ = true;

    auto& pending = coro::PendingUringOps::instance();
    if (appFd_ >= 0) {
        pending.cancelFd(appFd_);
    }
    if (remoteFd_ >= 0) {
        pending.cancelFd(remoteFd_);
    }

    // UDP 中继随连接销毁而停止
    if (udpRelay_) {
        udpRelay_->stop();
        udpRelay_.reset();
    }

    if (remoteFd_ >= 0) {
        ::close(remoteFd_);
        remoteFd_ = -1;
    }
    if (appFd_ >= 0) {
        LOG_INFO("Socks5Connection", "fd=", appFd_, " closing connection");
        ::close(appFd_);
        appFd_ = -1;
    }
}

} // namespace client
} // namespace vmess
