#ifndef VMESS_SERVER_VLESS_CONNECTION_H
#define VMESS_SERVER_VLESS_CONNECTION_H

#include "server/connection.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"
#include "proxy/vless/protocol.h"
#include "net/io_uring.h"

#include <array>
#include <vector>
#include <cstdint>

namespace vmess {
namespace server {

/**
 * @brief VLESS 协议连接
 *
 * 生命周期：
 *   HANDSHAKE -> CONNECTING -> SENDING_RESPONSE -> RELAY -> CLOSED
 *
 * 握手阶段由协程驱动，连接和响应发送使用 io_uring 异步操作
 */
class VlessConnection : public Connection {
public:
    enum class State {
        HANDSHAKE,         // 协程解析 VLESS 请求头
        CONNECTING,         // 异步连接目标服务器
        SENDING_RESPONSE,   // 异步发送 VLESS 响应头
        RELAY,              // 纯 io_uring 双向转发
        CLOSED              // 连接已关闭
    };

    VlessConnection(int clientFd, net::IoUring& uring);
    ~VlessConnection() override;

    // Connection 接口
    void prepareIO(net::IoUring& uring) override;
    void onIOComplete(int fd, int result, net::UringEventType type) override;
    bool isClosed() const override;
    int primaryFd() const override;
    bool hasFd(int fd) const override;

    // 获取当前状态（调试用）
    State state() const { return state_; }

    // Connection 接口：通知 EventLoop 需要重新 prepareIO()
    bool needsPrepare() const override { return needsPrepare_; }
    void clearNeedsPrepare() override { needsPrepare_ = false; }

private:
    // 握手阶段
    void prepareHandshakeIO(net::IoUring& uring);
    void onHandshakeIOComplete(int fd, int result);
    coro::Task<proxy::vless::Request> processHandshake();
    void startConnecting();  // 开始异步连接目标服务器

    // 连接目标阶段（异步）
    void prepareConnectingIO(net::IoUring& uring);
    void onConnectingIOComplete(int fd, int result);

    // 发送响应阶段（异步）
    void prepareSendingResponseIO(net::IoUring& uring);
    void onSendingResponseIOComplete(int fd, int result);

    // 转发阶段
    void prepareRelayIO(net::IoUring& uring);
    void onRelayIOComplete(int fd, int result);

    // 通用
    void close();
    void enterRelayState(int targetFd);

    int clientFd_;
    int targetFd_ = -1;
    net::IoUring& uring_;
    coro::UringBufferedStream stream_;
    coro::Task<proxy::vless::Request> handshakeTask_;  // 返回 Request 而不是 targetFd
    bool handshakeStarted_ = false;
    State state_ = State::HANDSHAKE;

    // 异步连接阶段使用的临时数据
    proxy::vless::Request pendingRequest_;  // 握手解析出的请求
    int connectingFd_ = -1;                // 正在连接的 socket fd
    struct sockaddr_in targetAddr_{};       // 目标地址

    // 发送响应阶段使用的缓冲区
    std::array<uint8_t, 2> responseBuf_;  // VLESS 响应头缓冲区（version + addons_len）
    size_t responseLen_ = 0;              // 响应长度

    // 转发缓冲区
    alignas(64) std::array<uint8_t, 4096> clientRecvBuf_;
    alignas(64) std::array<uint8_t, 4096> targetRecvBuf_;

    // 待发送队列（本轮 recv 到的数据，下轮 prepare 时发）
    struct PendingSend {
        int fd;
        uint8_t* buf;
        size_t len;
    };
    std::vector<PendingSend> pendingSends_;

    // 防止重复提交 RECV SQE
    bool clientRecvInflight_ = false;
    bool targetRecvInflight_ = false;

    // 半关闭状态：一个方向已关闭，另一个方向继续转发
    bool clientClosed_ = false;
    bool targetClosed_ = false;

    // 握手阶段 buffer 中剩余的客户端数据（请求头之后的 payload）
    std::vector<uint8_t> handshakeRemaining_;

    // 通知 EventLoop 需要重新调用 prepareIO()（状态刚变化）
    bool needsPrepare_ = false;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_VLESS_CONNECTION_H
