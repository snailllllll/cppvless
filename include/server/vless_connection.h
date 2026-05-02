#ifndef VMESS_SERVER_VLESS_CONNECTION_H
#define VMESS_SERVER_VLESS_CONNECTION_H

#include "coro/buffered_stream.h"
#include "coro/async_stream.h"
#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "proxy/vless/protocol.h"
#include "net/io_uring.h"

#include <array>
#include <cstdint>

namespace vmess {
namespace server {

/**
 * @brief VLESS 协议连接（Go-like 协程版本）
 * 
 * 核心设计：两个协程分别处理两个方向的 I/O
 *   - clientTask_: 握手 + client → target 转发
 *   - targetTask_: target → client 转发
 * 
 * 对标 Go Xray 的 task.Run 模式：
 *   task.OnSuccess(postRequest, task.Close(serverWriter))
 *   task.OnSuccess(getResponse, task.Close(writer))
 * 
 * 半关闭状态模型（参考 Go buf.Copy + task.Close）：
 *   - clientReadDone_: client→target 方向 EOF → shutdown target 写端
 *   - targetReadDone_: target→client 方向 EOF → shutdown client 写端
 *   - 两者都 true → 连接完全关闭
 */
class VlessConnection {
public:
    VlessConnection(int clientFd, net::IoUring& uring);
    ~VlessConnection();

    /// 启动 clientTask 协程（由 EventLoop 在新连接时调用）
    void start();

    bool isClosed() const { return closed_; }
    int primaryFd() const { return clientFd_; }
    bool hasFd(int fd) const { return fd == clientFd_ || fd == targetFd_; }

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

    /// 完全关闭连接（释放所有资源）
    void doClose();

    // ── 成员 ──

    int clientFd_;
    int targetFd_ = -1;
    net::IoUring& uring_;
    coro::UringBufferedStream stream_;     // 握手阶段使用的缓冲流

    coro::Task<void> clientTask_;
    coro::Task<void> targetTask_;

    // ── 半关闭状态 ──
    bool closed_ = false;            // 连接完全关闭
    bool clientReadDone_ = false;    // client → target 方向读端 EOF
    bool targetReadDone_ = false;    // target → client 方向读端 EOF

    // 握手阶段目标地址（createTargetSocket 填充，connect 使用）
    struct sockaddr_in targetAddr_{};

    // 握手阶段缓冲区中剩余数据
    std::vector<uint8_t> handshakeRemaining_;
};

} // namespace server
} // namespace vmess

#endif // VMESS_SERVER_VLESS_CONNECTION_H
