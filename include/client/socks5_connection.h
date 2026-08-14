#ifndef VMESS_CLIENT_SOCKS5_CONNECTION_H
#define VMESS_CLIENT_SOCKS5_CONNECTION_H

#include "client/vless_client.h"
#include "client/socks5_udp_relay.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"
#include "net/io_uring.h"
#include "server/connection.h"

#include <cstdint>
#include <memory>

namespace vmess {
namespace client {

/**
 * @brief SOCKS5 客户端连接（协程状态机）
 *
 * 一个 SOCKS5 会话 = 一个本地 TCP 连接（appFd）：
 *   Greeting -> Request -> Dispatch(CONNECT | UDP ASSOCIATE) -> Relay -> Cleanup
 *
 * - CONNECT：建立到远端 VLESS 服务器的 TCP 隧道，双向中继（半关闭传播）
 * - UDP ASSOCIATE：创建本地 UDP socket，交给 Socks5UdpRelay 管理
 *   每个目标地址一条独立的 VLESS UDP 会话（TCP 连接 + 长度帧）
 *
 * 半关闭状态模型与服务端 VlessConnection 保持一致：
 *   - appReadDone_: app→remote 方向 EOF → shutdown remote 写端
 *   - remoteReadDone_: remote→app 方向 EOF → shutdown app 写端
 *   - 两者都 true → 连接完全关闭
 */
class Socks5Connection : public server::EventLoopConnection {
public:
    Socks5Connection(int appFd, net::IoUring& uring, const VlessClientConfig& cfg);
    ~Socks5Connection() override;

    Socks5Connection(const Socks5Connection&) = delete;
    Socks5Connection& operator=(const Socks5Connection&) = delete;

    void start() override;
    bool isClosed() const override { return closed_; }
    int primaryFd() const override { return appFd_; }
    bool hasFd(int fd) const override;

private:
    /// 主控协程：Greeting -> Request -> Dispatch -> Relay -> Cleanup
    coro::Task<void> clientTask();

    /// TCP 会话：建 VLESS 隧道 → SOCKS5 成功响应 → 双向中继
    coro::Task<void> runTcpSession(const proxy::socks5::Request& req);

    /// UDP ASSOCIATE 会话：绑定 UDP socket → 响应 → Socks5UdpRelay
    coro::Task<void> runUdpAssociate(const proxy::socks5::Request& req);

    /// 远端侧协程：remote → app 转发
    coro::Task<void> remoteTask(int remoteFd);

    /// 通知 remoteTask 启动
    void startRemoteTask(int remoteFd);

    /// 发送 SOCKS5 响应
    coro::Task<bool> sendSocksReply(proxy::socks5::Reply reply,
                                    const std::string& bindIp,
                                    uint16_t bindPort);

    /// clientTask 结束后的统一清理逻辑
    void finishClientTask();

    /// 完全关闭连接（释放所有资源）
    void doClose();

    int appFd_;
    int remoteFd_ = -1;
    net::IoUring& uring_;
    VlessClientConfig cfg_;
    coro::UringBufferedStream stream_;     // 握手阶段使用的缓冲流

    coro::Task<void> clientTask_;
    coro::Task<void> remoteTask_;

    std::unique_ptr<Socks5UdpRelay> udpRelay_;

    // ── 半关闭状态 ──
    bool closed_ = false;
    bool appReadDone_ = false;
    bool remoteReadDone_ = false;
    bool remoteTaskStarted_ = false;
};

} // namespace client
} // namespace vmess

#endif // VMESS_CLIENT_SOCKS5_CONNECTION_H
