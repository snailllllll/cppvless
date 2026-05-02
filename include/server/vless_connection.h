#ifndef VMESS_SERVER_VLESS_CONNECTION_H
#define VMESS_SERVER_VLESS_CONNECTION_H

#include "server/connection.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "proxy/vless/protocol.h"
#include "net/io_uring.h"

#include <array>
#include <cstdint>

namespace vmess {
namespace server {

/**
 * @brief VLESS 协议连接（统一协程版本）
 * 
 * 核心设计：两个协程分别处理两个方向的 I/O
 *   - clientTask_: 握手 + client → target 转发
 *   - targetTask_: target → client 转发
 * 
 * 主循环通过 CoroutineRegistry resume 对应协程，
 * 不再需要 State 枚举和 prepareIO/onIOComplete 回调对。
 */
class VlessConnection : public Connection {
public:
    VlessConnection(int clientFd, net::IoUring& uring);
    ~VlessConnection() override;

    // ── Connection 接口 ──

    void prepareIO(net::IoUring& uring) override;
    void onIOComplete(int fd, int result, net::UringEventType type) override;
    bool isClosed() const override;
    int primaryFd() const override;
    bool hasFd(int fd) const override;
    bool needsPrepare() const override { return needsPrepare_; }
    void clearNeedsPrepare() override { needsPrepare_ = false; }

private:
    // ── 协程 ──

    /// 客户端侧协程：握手 + client → target 转发
    coro::Task<void> clientTask();

    /// 远端侧协程：target → client 转发
    coro::Task<void> targetTask(int targetFd);

    // ── 握手子流程 ──
    coro::Task<proxy::vless::Request> processHandshake();

    /// 解析目标地址并创建连接 socket（返回 fd，-1 表示失败）
    int createTargetSocket(const proxy::vless::Request& req);

    /// 通知 targetTask 启动
    void startTargetTask(int targetFd);

    // ── 通用 ──
    void close();
    void doClose();

    // ── 成员 ──

    int clientFd_;
    int targetFd_ = -1;
    net::IoUring& uring_;
    coro::UringBufferedStream stream_;     // 仅握手阶段使用

    coro::Task<void> clientTask_;
    coro::Task<void> targetTask_;

    bool started_ = false;
    bool closed_ = false;
    bool needsPrepare_ = false;

    // 握手阶段目标地址（createTargetSocket 填充，connect 使用）
    struct sockaddr_in targetAddr_{};

    // 握手阶段缓冲区中剩余数据
    std::vector<uint8_t> handshakeRemaining_;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_VLESS_CONNECTION_H
