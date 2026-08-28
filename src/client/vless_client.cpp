#include "client/vless_client.h"
#include "proxy/vless/encoder.h"
#include "coro/async_stream.h"
#include "coro/buffered_stream.h"
#include "coro/uring_awaitable.h"
#include "net/tls_stream.h"
#include "common/log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <cstring>

namespace vless {
namespace client {

VlessClientConfig VlessClientConfig::fromString(const std::string& s) {
    VlessClientConfig cfg;
    auto pos = s.rfind(':');
    if (pos == std::string::npos) {
        cfg.remoteHost = s;
    } else {
        cfg.remoteHost = s.substr(0, pos);
        try {
            cfg.remotePort = static_cast<uint16_t>(std::stoi(s.substr(pos + 1)));
        } catch (...) {
            // 端口解析失败时保持默认
        }
    }
    return cfg;
}

proxy::vless::Request toVlessRequest(const proxy::socks5::Request& req,
                                     proxy::vless::Command cmd) {
    proxy::vless::Request vreq;
    vreq.command = cmd;
    vreq.port = req.port;

    // SOCKS5 Address 与 VLESS Request 的地址 variant 类型完全一致，直接搬运
    if (req.address.isIPv4()) {
        vreq.address = std::get<std::array<uint8_t, 4>>(req.address.value);
    } else if (req.address.isIPv6()) {
        vreq.address = std::get<std::array<uint8_t, 16>>(req.address.value);
    } else {
        vreq.address = std::get<std::string>(req.address.value);
    }
    return vreq;
}

namespace {

/**
 * @brief 解析远端服务器地址，返回第一个可用 sockaddr
 * @return 0 成功，-1 失败
 */
int resolveRemote(const std::string& host, uint16_t port,
                  struct sockaddr_storage& out, socklen_t& outLen) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);

    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        LOG_ERROR("VlessClient", "getaddrinfo failed for remote: ", host);
        return -1;
    }

    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    outLen = static_cast<socklen_t>(res->ai_addrlen);
    freeaddrinfo(res);
    return 0;
}

int makeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

} // namespace

coro::Task<VlessClientHandshakeResult> vlessConnectAndHandshake(
    net::IoUring& uring,
    const VlessClientConfig& cfg,
    const proxy::vless::Request& vlessReq) {

    VlessClientHandshakeResult result;

    // 1. 解析远端服务器地址
    struct sockaddr_storage remoteAddr {};
    socklen_t remoteAddrLen = 0;
    if (resolveRemote(cfg.remoteHost, cfg.remotePort, remoteAddr, remoteAddrLen) < 0) {
        co_return result;
    }

    // 2. 创建 socket 并异步连接
    int fd = socket(remoteAddr.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("VlessClient", "socket() failed for remote: ", cfg.remoteHost);
        co_return result;
    }
    makeNonBlocking(fd);

    int connectResult = co_await coro::AsyncConnect(
        fd, uring,
        reinterpret_cast<const struct sockaddr*>(&remoteAddr),
        remoteAddrLen);
    if (connectResult < 0) {
        LOG_ERROR("VlessClient", "connect failed to remote ",
                  cfg.remoteHost, ":", cfg.remotePort,
                  " err=", connectResult, " (", strerror(-connectResult), ")");
        ::close(fd);
        co_return result;
    }

    LOG_INFO("VlessClient", "connected to remote ", cfg.remoteHost, ":",
             cfg.remotePort, " fd=", fd);

    // 3. 构造数据流：明文 AsyncStream；启用 TLS 时用 TlsStream 包裹（isServer=false）
    auto rawStream = std::make_shared<coro::AsyncStream>(fd, uring);
    std::shared_ptr<net::Stream> stream = rawStream;

    if (cfg.tlsEnabled) {
        SSL_CTX* ctx = static_cast<SSL_CTX*>(cfg.tlsCtx.get());
        if (!ctx) {
            LOG_ERROR("VlessClient", "tls enabled but tlsCtx is null");
            ::close(fd);
            co_return result;
        }
        auto tlsStream = std::make_shared<net::TlsStream>(*rawStream, ctx, false);
        if (!co_await tlsStream->handshake()) {
            LOG_ERROR("VlessClient", "TLS handshake failed to remote ",
                      cfg.remoteHost, ":", cfg.remotePort);
            ::close(fd);
            co_return result;
        }
        LOG_INFO("VlessClient", "TLS handshake OK to ", cfg.remoteHost, ":",
                 cfg.remotePort, " fd=", fd);
        stream = tlsStream;
    }

    // 4. 发送 VLESS 请求头（补上用户 UUID）
    proxy::vless::Request outReq = vlessReq;
    outReq.uuid = cfg.uuid;
    auto requestBytes = proxy::vless::Encoder::encodeRequest(outReq);
    int sent = co_await stream->writeFull(requestBytes);
    if (sent <= 0) {
        LOG_ERROR("VlessClient", "failed to send vless request header, sent=", sent);
        ::close(fd);
        co_return result;
    }

    // 5. 读取并校验响应头（缓冲流基于 stream 取数）
    coro::UringBufferedStream buffered(*stream);
    if (!co_await proxy::vless::Encoder::decodeResponse(buffered)) {
        LOG_ERROR("VlessClient", "invalid vless response header from remote ",
                  cfg.remoteHost);
        ::close(fd);
        co_return result;
    }

    // 6. 收集握手缓冲中多读出的数据（对端可能已开始转发目标数据）
    result.remaining = buffered.drainRemaining();
    result.remoteFd = fd;
    result.stream = std::move(stream);
    result.raw = std::move(rawStream);   // TLS 时保活底层明文流
    co_return result;
}

} // namespace client
} // namespace vless
