#ifndef VMESS_CLIENT_SOCKS5_UDP_RELAY_H
#define VMESS_CLIENT_SOCKS5_UDP_RELAY_H

#include "client/vless_client.h"
#include "coro/task.h"
#include "net/io_uring.h"
#include "proxy/socks5/socks5.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace vmess {
namespace client {

/**
 * @brief SOCKS5 UDP ASSOCIATE 中继
 *
 * 职责：
 *   - 监听本地 UDP socket（udpFd），接收应用发来的 SOCKS5 UDP 数据报
 *   - 每个目标地址维护一个到远端 VLESS 服务器的独立 UDP 会话（TCP 连接 + 长度帧）
 *   - 回程数据帧转回 SOCKS5 UDP 数据报发送给应用
 *
 * 并发模型（单线程事件循环，无数据竞争）：
 *   - recvLoop_：独占 (udpFd, READ)，解析数据报并路由到会话
 *   - 每个会话 outTask_：独占 (remoteFd, READ)，读长度帧 → 入队
 *   - sendLoop_：独占 (udpFd, WRITE) 与 (eventFd, READ)，等待 eventfd 通知后批量 sendto
 */
class Socks5UdpRelay {
public:
    Socks5UdpRelay(int udpFd, net::IoUring& uring, const VlessClientConfig& cfg);
    ~Socks5UdpRelay();

    Socks5UdpRelay(const Socks5UdpRelay&) = delete;
    Socks5UdpRelay& operator=(const Socks5UdpRelay&) = delete;

    /// 启动接收循环与会话管理
    void start();
    /// 停止所有会话并关闭资源（幂等）
    void stop();

    bool isClosed() const { return closed_; }

    /// 是否持有该 fd（用于 EventLoop 的 fd 追踪）
    bool hasFd(int fd) const;

private:
    /// 单个目标地址的 VLESS UDP 会话
    struct Session {
        int remoteFd = -1;
        std::shared_ptr<net::Stream> stream;  // 与远端的数据流（TLS 或明文）
        proxy::socks5::Address dest;
        uint16_t port = 0;
        struct sockaddr_storage appSrc {};  // 应用 UDP 源地址（回程目标）
        socklen_t appSrcLen = 0;
        bool alive = true;
        coro::Task<void> outTask_;
    };

    /// 会话键：类型 + 地址 + 端口
    static std::string makeKey(const proxy::socks5::Address& addr, uint16_t port);

    /// 获取或创建目标会话（异步握手，失败返回 nullptr）
    coro::Task<std::shared_ptr<Session>> getOrCreateSession(
        const proxy::socks5::Address& addr, uint16_t port);

    /// 应用 → 远端：接收 udpFd 数据报，路由到会话
    coro::Task<void> recvLoop();

    /// 远端 → 应用：等待 eventfd 通知，批量发送回程数据报
    coro::Task<void> sendLoop();

    /// 会话回程任务：读长度帧 → 入队回程数据报
    /// @param session 裸指针：会话由 map 持有，必须存活至协程结束
    coro::Task<void> sessionOutTask(Session* session);

    /// 入队回程数据报并唤醒 sendLoop
    void enqueueDatagram(const proxy::socks5::Address& dest, uint16_t port,
                         const std::vector<uint8_t>& payload,
                         const struct sockaddr* appSrc, socklen_t appSrcLen);

    int udpFd_;
    int eventFd_ = -1;
    net::IoUring& uring_;
    VlessClientConfig cfg_;

    bool closed_ = false;
    coro::Task<void> recvLoop_;
    coro::Task<void> sendLoop_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;

    // 回程数据报队列：{data, 应用源地址}
    struct PendingDatagram {
        std::vector<uint8_t> data;
        struct sockaddr_storage src {};
        socklen_t srcLen = 0;
    };
    std::deque<PendingDatagram> sendQueue_;
};

} // namespace client
} // namespace vmess

#endif // VMESS_CLIENT_SOCKS5_UDP_RELAY_H
