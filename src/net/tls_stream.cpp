#include "net/tls_stream.h"

#include "common/log.h"

#include <openssl/err.h>

#include <cstring>

namespace vless {
namespace net {

namespace {
std::string sslErrorString(int ret) {
    unsigned long err = ERR_get_error();
    char buf[256];
    if (err != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        return std::string(buf);
    }
    switch (ret) {
        case SSL_ERROR_WANT_READ: return "WANT_READ";
        case SSL_ERROR_WANT_WRITE: return "WANT_WRITE";
        case SSL_ERROR_ZERO_RETURN: return "ZERO_RETURN";
        case SSL_ERROR_SYSCALL: return "SYSCALL";
        default: return "unknown";
    }
}
} // namespace

TlsStream::TlsStream(Stream& inner, SSL_CTX* ctx, bool isServer)
    : inner_(inner), isServer_(isServer) {
    ssl_ = SSL_new(ctx);
    if (ssl_) {
        // memory BIO：SSL 的读写全部走内存缓冲，由我们控制与底层 Stream 的数据交换
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl_, rbio, wbio);
        if (isServer_) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
        }
    }
}

TlsStream::~TlsStream() {
    if (ssl_) {
        SSL_free(ssl_);
    }
}

coro::Task<bool> TlsStream::handshake() {
    if (!ssl_) {
        LOG_ERROR("TlsStream", "ssl_ is null");
        co_return false;
    }

    while (true) {
        int ret = isServer_ ? SSL_accept(ssl_) : SSL_connect(ssl_);
        if (ret == 1) {
            // 握手完成：刷出可能残留的输出（如 session ticket）
            co_await flushWriteBio();
            co_return true;
        }

        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            // memory BIO 关键点：SSL 可能同时产生了输出（如 ServerHello/
            // NewSessionTicket），必须先刷给对端，再喂入新输入，
            // 否则对端收不到握手消息而卡死。
            if (!co_await flushWriteBio()) {
                co_return false;
            }
            if (!co_await fillReadBio()) {
                co_return false;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (!co_await flushWriteBio()) {
                co_return false;
            }
            continue;
        }

        LOG_ERROR("TlsStream", "handshake failed: ", sslErrorString(err));
        co_return false;
    }
}

coro::Task<coro::RecvResult> TlsStream::read(size_t maxBytes) {
    coro::RecvResult rr;

    if (!ssl_) {
        rr.result = -1;
        co_return rr;
    }

    std::vector<uint8_t> buf(maxBytes);
    while (true) {
        int n = SSL_read(ssl_, buf.data(), (int)buf.size());
        if (n > 0) {
            rr.data.assign(buf.begin(), buf.begin() + n);
            rr.result = n;
            co_return rr;
        }

        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            // 先刷出可能残留的输出，再喂入新输入（memory BIO 顺序）
            if (!co_await flushWriteBio()) {
                rr.result = -1;
                co_return rr;
            }
            if (!co_await fillReadBio()) {
                co_return rr;  // EOF/错误，rr.result 保持 0 或由 fillReadBio 设置
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            // 需要发送数据（如 renegotiation）才能继续读
            if (!co_await flushWriteBio()) {
                rr.result = -1;
                co_return rr;
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            // 对端发送 close_notify：视为 EOF
            co_return rr;  // rr.result == 0 → eof()
        }

        LOG_WARN("TlsStream", "read failed: ", sslErrorString(err));
        rr.result = -1;
        co_return rr;
    }
}

coro::Task<int> TlsStream::writeFull(const void* data, size_t len) {
    if (!ssl_) {
        co_return -1;
    }

    size_t offset = 0;
    while (offset < len) {
        int n = SSL_write(ssl_,
                          static_cast<const uint8_t*>(data) + offset,
                          (int)(len - offset));
        if (n > 0) {
            offset += (size_t)n;
            // 把本次产生的密文刷到底层流
            if (!co_await flushWriteBio()) {
                co_return (offset > 0) ? (int)offset : -1;
            }
            continue;
        }

        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            // 先刷输出再喂输入（memory BIO 顺序）
            if (!co_await flushWriteBio()) {
                co_return (offset > 0) ? (int)offset : -1;
            }
            if (!co_await fillReadBio()) {
                co_return (offset > 0) ? (int)offset : -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (!co_await flushWriteBio()) {
                co_return (offset > 0) ? (int)offset : -1;
            }
            continue;
        }

        LOG_WARN("TlsStream", "write failed: ", sslErrorString(err));
        co_return (offset > 0) ? (int)offset : -1;
    }

    co_return (int)len;
}

coro::Task<int> TlsStream::shutdownWrite() {
    // 发送 close_notify（尽力而为）
    if (ssl_) {
        SSL_shutdown(ssl_);
        co_await flushWriteBio();
    }
    co_return co_await inner_.shutdownWrite();
}

coro::Task<bool> TlsStream::fillReadBio() {
    auto rr = co_await inner_.read();
    if (rr.eof() || rr.error()) {
        // 底层 EOF/错误：透传
        co_return false;
    }
    BIO* rbio = SSL_get_rbio(ssl_);
    if (BIO_write(rbio, rr.data.data(), (int)rr.data.size()) <= 0) {
        LOG_ERROR("TlsStream", "BIO_write(rbio) failed");
        co_return false;
    }
    co_return true;
}

coro::Task<bool> TlsStream::flushWriteBio() {
    BIO* wbio = SSL_get_wbio(ssl_);
    while (BIO_ctrl_pending(wbio) > 0) {
        std::vector<uint8_t> buf(16384);
        int n = BIO_read(wbio, buf.data(), (int)buf.size());
        if (n <= 0) break;
        int sent = co_await inner_.writeFull(buf.data(), (size_t)n);
        if (sent != n) {
            LOG_ERROR("TlsStream", "flush to inner stream failed, sent=", sent);
            co_return false;
        }
    }
    co_return true;
}

} // namespace net
} // namespace vless
