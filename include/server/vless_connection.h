#ifndef VLESS_SERVER_VLESS_CONNECTION_H
#define VLESS_SERVER_VLESS_CONNECTION_H

#include "coro/async_stream.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"
#include "proxy/vless/protocol.h"
#include "proxy/vless/vision.h"
#include "proxy/vless/encryption.h"
#include "proxy/vless/validator.h"
#include "net/io_uring.h"
#include "net/stream.h"
#include "net/tls.h"
#include "net/tls_stream.h"
#include "server/connection.h"

#include <cstdint>
#include <memory>
#include <sys/socket.h>

namespace vless {
namespace server {

/**
 * @brief VLESS 协议连接（协程状态机）
 *
 * clientTask_ 是会话状态机主控：
 *   Handshake -> Dispatch(TCP|UDP) -> Relay -> Cleanup
 *
 * 双向 I/O 由两个协程协作：
 *   - clientTask_: 握手/建链 + client → target
 *   - targetTask_: target → client
 *
 * 半关闭状态模型（参考 Go buf.Copy + task.Close）：
 *   - clientReadDone_: client→target 方向 EOF → shutdown target 写端
 *   - targetReadDone_: target→client 方向 EOF → shutdown client 写端
 *   - 两者都 true → 连接完全关闭
 */
class VlessConnection : public EventLoopConnection {
public:
    VlessConnection(int clientFd, net::IoUring& uring, const proxy::vless::Validator& validator,
                    SSL_CTX* tlsCtx = nullptr);
    ~VlessConnection();

    /// 启动 clientTask 协程（由 EventLoop 在新连接时调用）
    void start() override;

    bool isClosed() const override { return closed_; }
    int primaryFd() const override { return clientFd_; }
    bool hasFd(int fd) const override { return fd == clientFd_ || fd == targetFd_; }

private:
    // ── 会话状态机 ──

    /// 主控协程：Handshake → Dispatch → Relay → Cleanup
    coro::Task<void> clientTask();

    /// TCP 会话：协商 → 响应 → 建链 → 双向中继（响应先于建连，对齐 Xray）
    coro::Task<bool> runTcpSession(const proxy::vless::Request& req);

    /// UDP 会话：响应 → 建链 → length-packet 中继（响应先于建连，对齐 Xray）
    coro::Task<bool> runUdpSession(const proxy::vless::Request& req);

    /// 远端侧协程：target → client 转发
    coro::Task<void> targetTask(int targetFd);

    // ── 协议协商 ──

    /// 设置 Vision 模式（如果需要）
    coro::Task<bool> setupVision(const proxy::vless::Request& req);

    /// 设置 Encryption 模式（如果需要）：读取客户端公钥、生成服务端密钥对
    coro::Task<bool> setupEncryption(const proxy::vless::Request& req);

    // ── 连接建立 ──

    /// 创建 target socket 并异步连接到目标服务器
    coro::Task<bool> connectTarget(const proxy::vless::Request& req);

    /// 发送 VLESS 响应头 +（若启用 Encryption）发送服务端公钥并计算共享密钥
    coro::Task<bool> sendResponseAndKey(uint8_t version);

    // ── 数据转发 ──

    /// 转发握手阶段缓冲区中的剩余数据到 target
    coro::Task<bool> forwardHandshakeRemaining();

    /// Client → Target 中继（支持三种模式：普通 / Vision / Encryption）
    coro::Task<bool> relayClientToTarget();
    /// UDP: Client(length-packet) → Target(datagram)
    coro::Task<bool> relayUdpClientToTarget();
    /// UDP: Target(datagram) → Client(length-packet)
    coro::Task<void> relayUdpTargetToClient();

    // ── 清理 ──

    /// clientTask 结束后的统一清理逻辑
    void finishClientTask();

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
    const proxy::vless::Validator& validator_;
    SSL_CTX* tlsCtx_ = nullptr;                 // TLS 上下文（配置了 --tls-port 时非空）
    // ── 所有权模型（方案 A：唯一所有权 + 视图指针）──────────────────────
    // rawStream_ 唯一持有底层 AsyncStream；tlsStream_ 唯一持有 TLS 包装
    // （仅 tlsCtx_ 非空时非空）；clientStream_ 是"当前激活流"的视图指针
    // （借用，不拥有）。析构顺序（声明逆序）：stream_ → clientStream_(无操作)
    // → tlsStream_ → rawStream_，TlsStream 内部引用 rawStream_ 始终有效。
    std::unique_ptr<coro::AsyncStream> rawStream_;  // 底层明文流（AsyncStream 实现）
    std::unique_ptr<net::TlsStream> tlsStream_;     // TLS 包装流（仅 TLS 模式拥有）
    net::Stream* clientStream_ = nullptr;           // 当前激活流视图：明文→rawStream_，TLS→tlsStream_
    coro::UringBufferedStream stream_;     // 握手阶段使用的缓冲流（基于 clientStream_）

    coro::Task<void> clientTask_;
    coro::Task<void> targetTask_;

    // ── 半关闭状态 ──
    bool closed_ = false;            // 连接完全关闭（可被 EventLoop 回收）
    bool clientReadDone_ = false;    // client → target 方向读端 EOF
    bool targetReadDone_ = false;    // target → client 方向读端 EOF
    bool targetTaskStarted_ = false; // targetTask 是否已启动（决定能否立刻 closed_）

    // 握手阶段目标地址（createTargetSocket 填充，connect 使用）
    struct sockaddr_storage targetAddr_{};
    socklen_t targetAddrLen_ = 0;

    // 握手阶段缓冲区中剩余数据
    std::vector<uint8_t> handshakeRemaining_;

    // ── Vision (xtls-rprx-vision) 支持 ──
    bool useVision_ = false;
    std::shared_ptr<proxy::vless::VisionContext> visionCtx_;
    std::unique_ptr<proxy::vless::VisionReader> visionReader_;
    std::unique_ptr<proxy::vless::VisionWriter> visionWriter_;

    // ── Encryption 支持 ──
    bool useEncryption_ = false;
    std::unique_ptr<proxy::vless::EncryptionSession> encryptionSession_;
    std::vector<uint8_t> clientPublicKey_;  // 客户端 X25519 公钥（32字节）
    proxy::vless::Command command_ = proxy::vless::Command::TCP;
};

} // namespace server
} // namespace vless

#endif // VLESS_SERVER_VLESS_CONNECTION_H
