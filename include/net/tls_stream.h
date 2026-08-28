#ifndef VLESS_NET_TLS_STREAM_H
#define VLESS_NET_TLS_STREAM_H

#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "net/stream.h"

#include <openssl/ssl.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace vless {
namespace net {

/**
 * @brief TLS 流（服务器/客户端通用），实现 net::Stream 接口
 *
 * 基于 memory BIO 设计：不直接读写 fd，而是从底层 Stream 拉取
 * 密文喂给 SSL，再从 SSL 取出明文。因此 TLS 只是"换一个 Stream 实现"，
 * 协议层（Decoder/Encoder/VlessConnection）完全无感知。
 *
 * 读路径（read）：
 *   SSL_read() → 有明文直接返回
 *              → WANT_READ：从 inner stream 读密文 → BIO_write(rbio) → 重试
 *              → ZERO_RETURN：对端关闭（EOF）
 *
 * 写路径（writeFull）：
 *   SSL_write() → 密文进入 wbio → 取出 wbio 中所有密文 → inner.writeFull
 *   （SSL_write 返回 WANT_READ/WANT_WRITE 时先 flush 已有密文再重试）
 *
 * 握手（handshake）：
 *   server: SSL_accept() 循环（WANT_READ 时从 inner 读密文喂 rbio）
 *   client: SSL_connect() 同理
 */
class TlsStream : public Stream {
public:
    /**
     * @param inner 底层明文流（所有权归调用方，须在 TlsStream 生命周期内有效）
     * @param ctx   SSL_CTX（所有权归调用方）
     * @param isServer true=服务器（SSL_accept），false=客户端（SSL_connect）
     */
    TlsStream(Stream& inner, SSL_CTX* ctx, bool isServer);
    ~TlsStream() override;

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    /// 执行 TLS 握手（服务器 accept / 客户端 connect）；成功返回 true
    coro::Task<bool> handshake();

    /// 读取明文数据
    coro::Task<coro::RecvResult> read(size_t maxBytes = 8192) override;

    /// 写入全部明文数据（自动加密为密文发送）
    coro::Task<int> writeFull(const void* data, size_t len) override;

    /// 半关闭：发送 close_notify 并关闭底层写端
    coro::Task<int> shutdownWrite() override;

    int fd() const override { return inner_.fd(); }

private:
    /// 从底层流读密文并写入 rbio；返回 false 表示 EOF/错误
    coro::Task<bool> fillReadBio();

    /// 把 wbio 中的待发密文全部写入底层流；返回 false 表示写失败
    coro::Task<bool> flushWriteBio();

    Stream& inner_;
    SSL* ssl_ = nullptr;
    bool isServer_;
};

} // namespace net
} // namespace vless

#endif // VLESS_NET_TLS_STREAM_H
