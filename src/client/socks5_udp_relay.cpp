#include "client/socks5_udp_relay.h"
#include "proxy/vless/encoder.h"
#include "coro/async_stream.h"
#include "coro/buffered_stream.h"
#include "coro/uring_awaitable.h"
#include "common/log.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>

namespace vmess {
namespace client {

Socks5UdpRelay::Socks5UdpRelay(int udpFd, net::IoUring& uring,
                               const VlessClientConfig& cfg)
    : udpFd_(udpFd), uring_(uring), cfg_(cfg) {}

Socks5UdpRelay::~Socks5UdpRelay() {
    stop();
}

void Socks5UdpRelay::start() {
    eventFd_ = eventfd(0, EFD_NONBLOCK);
    if (eventFd_ < 0) {
        LOG_ERROR("Socks5UdpRelay", "eventfd() failed");
        closed_ = true;
        return;
    }

    recvLoop_ = recvLoop();
    if (!recvLoop_.done()) {
        recvLoop_.h.resume();
    }
    sendLoop_ = sendLoop();
    if (!sendLoop_.done()) {
        sendLoop_.h.resume();
    }
    LOG_INFO("Socks5UdpRelay", "udp relay started, udpFd=", udpFd_);
}

void Socks5UdpRelay::stop() {
    if (closed_) return;
    closed_ = true;

    auto& pending = coro::PendingUringOps::instance();

    // 先取消所有挂起操作，再关闭 fd，避免 CQE 访问已释放的协程帧
    for (auto& [key, session] : sessions_) {
        (void)key;
        if (session && session->remoteFd >= 0) {
            pending.cancelFd(session->remoteFd);
            ::close(session->remoteFd);
            session->remoteFd = -1;
            session->alive = false;
        }
    }

    if (eventFd_ >= 0) {
        pending.cancelFd(eventFd_);
        ::close(eventFd_);
        eventFd_ = -1;
    }
    if (udpFd_ >= 0) {
        pending.cancelFd(udpFd_);
        ::close(udpFd_);
        udpFd_ = -1;
    }

    // 清空 map 会销毁各 session 及其 outTask_ 协程帧（此时已无挂起的 I/O 引用）
    sessions_.clear();
    sendQueue_.clear();

    LOG_INFO("Socks5UdpRelay", "udp relay stopped");
}

bool Socks5UdpRelay::hasFd(int fd) const {
    if (fd == udpFd_ || fd == eventFd_) return true;
    for (const auto& [key, session] : sessions_) {
        (void)key;
        if (session && session->remoteFd == fd) return true;
    }
    return false;
}

std::string Socks5UdpRelay::makeKey(const proxy::socks5::Address& addr, uint16_t port) {
    return std::to_string(static_cast<int>(addr.type())) + "|" +
           addr.toString() + ":" + std::to_string(port);
}

coro::Task<std::shared_ptr<Socks5UdpRelay::Session>>
Socks5UdpRelay::getOrCreateSession(const proxy::socks5::Address& addr, uint16_t port) {
    auto key = makeKey(addr, port);
    auto it = sessions_.find(key);
    if (it != sessions_.end() && it->second->alive) {
        co_return it->second;
    }

    // 构造 VLESS UDP 请求
    proxy::socks5::Request socksReq;
    socksReq.cmd = proxy::socks5::Command::UdpAssociate;
    socksReq.address = addr;
    socksReq.port = port;
    auto vlessReq = toVlessRequest(socksReq, proxy::vless::Command::UDP);

    auto handshake = co_await vlessConnectAndHandshake(uring_, cfg_, vlessReq);
    if (handshake.remoteFd < 0) {
        LOG_WARN("Socks5UdpRelay", "failed to create udp session to ",
                 addr.toString(), ":", port);
        co_return nullptr;
    }

    auto session = std::make_shared<Session>();
    session->remoteFd = handshake.remoteFd;
    session->stream = handshake.stream;
    session->dest = addr;
    session->port = port;

    // 启动会话回程任务（远端 → 应用）
    session->outTask_ = sessionOutTask(session.get());
    if (!session->outTask_.done()) {
        session->outTask_.h.resume();
    }

    // 替换可能存在的失效会话（旧会话 outTask 已结束，销毁安全）
    sessions_[key] = session;
    LOG_INFO("Socks5UdpRelay", "new udp session to ", addr.toString(), ":",
             port, " remoteFd=", session->remoteFd);
    co_return session;
}

coro::Task<void> Socks5UdpRelay::recvLoop() {
    while (!closed_) {
        auto r = co_await coro::AsyncRecvFrom(udpFd_, uring_);
        if (r.rr.error()) {
            if (!closed_) {
                LOG_WARN("Socks5UdpRelay", "udp recvfrom error: ", r.rr.result);
            }
            break;
        }
        if (r.rr.eof() || r.rr.data.empty()) {
            continue;
        }

        // 解析 SOCKS5 UDP 数据报
        proxy::socks5::Address dest;
        uint16_t port = 0;
        std::vector<uint8_t> payload;
        if (!proxy::socks5::Parser::parseUdpDatagram(r.rr.data, dest, port, payload)) {
            LOG_WARN("Socks5UdpRelay", "failed to parse socks5 udp datagram, "
                     "dropping ", r.rr.data.size(), " bytes");
            continue;
        }
        if (payload.empty()) {
            continue;
        }

        // 获取会话（首次到达时异步建连）
        auto session = co_await getOrCreateSession(dest, port);
        if (!session) {
            continue;  // 建连失败，丢弃该包
        }

        // 记录应用源地址（回程发送目标）
        session->appSrc = r.srcAddr;
        session->appSrcLen = r.srcAddrLen;

        // 应用 → 远端：长度帧 + 数据
        std::vector<uint8_t> framed;
        framed.reserve(2 + payload.size());
        uint16_t len = static_cast<uint16_t>(payload.size());
        framed.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        framed.push_back(static_cast<uint8_t>(len & 0xFF));
        framed.insert(framed.end(), payload.begin(), payload.end());

        if (!session->stream) {
            LOG_WARN("Socks5UdpRelay", "udp session stream is null");
            session->alive = false;
            continue;
        }
        int written = co_await session->stream->writeFull(framed);
        if (written <= 0) {
            LOG_WARN("Socks5UdpRelay", "write to udp session failed: ", written);
            // 关闭会话：outTask 会因 recv 错误被唤醒并自行退出
            session->alive = false;
            if (session->remoteFd >= 0) {
                coro::PendingUringOps::instance().cancelFd(session->remoteFd);
                ::close(session->remoteFd);
                session->remoteFd = -1;
            }
            continue;
        }
    }

    // recvLoop 退出（stop 或异常）时整体停止
    stop();
}

coro::Task<void> Socks5UdpRelay::sendLoop() {
    while (!closed_) {
        // 等待 eventfd 通知
        auto rr = co_await coro::AsyncRecv(eventFd_, uring_);
        if (rr.error()) {
            if (!closed_) {
                LOG_WARN("Socks5UdpRelay", "eventfd read error: ", rr.result);
            }
            break;
        }

        // 批量清空回程队列
        while (!sendQueue_.empty() && !closed_) {
            auto item = std::move(sendQueue_.front());
            sendQueue_.pop_front();

            int sent = co_await coro::AsyncSendTo(
                udpFd_, uring_, item.data.data(), item.data.size(),
                reinterpret_cast<const struct sockaddr*>(&item.src),
                item.srcLen);
            if (sent <= 0) {
                LOG_WARN("Socks5UdpRelay", "udp sendto failed: ", sent);
                break;
            }
        }
    }
}

coro::Task<void> Socks5UdpRelay::sessionOutTask(Session* session) {
    while (!closed_ && session->alive && session->remoteFd >= 0 && session->stream) {
        auto rr = co_await session->stream->read();
        if (rr.eof()) {
            break;
        }
        if (rr.error()) {
            if (!closed_) {
                LOG_WARN("Socks5UdpRelay", "udp session read error: ", rr.result);
            }
            break;
        }
        if (rr.data.empty()) {
            continue;
        }

        // 解析长度帧：2B BE + payload（一次 recv 可能含多帧；半帧场景简化丢弃尾部）
        size_t pos = 0;
        while (pos + 2 <= rr.data.size()) {
            uint16_t pktLen = (static_cast<uint16_t>(rr.data[pos]) << 8) |
                              static_cast<uint16_t>(rr.data[pos + 1]);
            if (pos + 2 + pktLen > rr.data.size()) {
                break;  // 半帧，剩余数据简化丢弃
            }
            std::vector<uint8_t> payload(rr.data.begin() +
                                             static_cast<std::ptrdiff_t>(pos + 2),
                                         rr.data.begin() +
                                             static_cast<std::ptrdiff_t>(pos + 2 + pktLen));
            pos += 2 + pktLen;
            enqueueDatagram(session->dest, session->port, payload,
                            reinterpret_cast<const struct sockaddr*>(&session->appSrc),
                            session->appSrcLen);
        }
    }

    // 会话结束：标记失效并关闭 remoteFd（不删除 map 条目，避免销毁正在运行的协程帧）
    session->alive = false;
    if (session->remoteFd >= 0) {
        coro::PendingUringOps::instance().cancelFd(session->remoteFd);
        ::close(session->remoteFd);
        session->remoteFd = -1;
    }
}

void Socks5UdpRelay::enqueueDatagram(const proxy::socks5::Address& dest, uint16_t port,
                                     const std::vector<uint8_t>& payload,
                                     const struct sockaddr* appSrc, socklen_t appSrcLen) {
    if (closed_ || payload.empty()) return;

    PendingDatagram item;
    item.data = proxy::socks5::Parser::encodeUdpDatagram(dest, port, payload);
    if (appSrc && appSrcLen > 0) {
        std::memcpy(&item.src, appSrc, appSrcLen);
        item.srcLen = appSrcLen;
    }
    sendQueue_.push_back(std::move(item));

    // 唤醒 sendLoop
    if (eventFd_ >= 0) {
        uint64_t one = 1;
        ssize_t ret = ::write(eventFd_, &one, sizeof(one));
        (void)ret;  // EAGAIN 时忽略（counter 已非零）
    }
}

} // namespace client
} // namespace vmess
