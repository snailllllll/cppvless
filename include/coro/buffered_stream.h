#ifndef VMESS_CORO_BUFFERED_STREAM_H
#define VMESS_CORO_BUFFERED_STREAM_H

#include "common/log.h"
#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "net/io_uring.h"
#include "net/stream.h"

#include <cstdint>
#include <vector>

namespace vmess {
namespace coro {

/**
 * @brief 缓冲流抽象接口（协议解析层依赖）
 *
 * 协议层（VLESS Decoder/Encoder、SOCKS5 Parser）只需"精确读 N 字节"
 * 的能力，不关心底层是明文 fd 还是 TLS。UringBufferedStream 实现本接口，
 * 阶段 2 的 TlsStream 同样可通过 UringBufferedStream 包装后交给协议层。
 */
class BufferedStream {
public:
    virtual ~BufferedStream() = default;

    /// 精确读取 need 字节（缓冲不足时从底层流取数；EOF/错误返回已缓冲的部分或空）
    virtual Task<std::vector<uint8_t>> read(size_t need) = 0;

    /// 读取 1 字节
    virtual Task<uint8_t> readByte() = 0;

    /// 获取当前 buffer 中未消费的数据（返回拷贝，安全）
    virtual std::vector<uint8_t> drainRemaining() = 0;

    /// 底层 fd（调试/日志用）
    virtual int fd() const = 0;
};

/**
 * @brief 为 io_uring 协程设计的缓冲流（纯协程版本）
 * 
 * 核心设计：
 * - 基于 net::Stream 抽象取数（不直接持有 fd/uring）
 * - 协程小量 read(1), read(16) → 从 buffer 消费
 * - buffer 不够时，直接 co_await 底层 stream.read() 循环读取，直到数据充足
 * 
 * 返回 vector<uint8_t> 而非 span，确保数据所有权安全。
 *
 * 注意：协程方法实现位于 src/coro/buffered_stream.cpp
 * （GCC 协程 frame 符号在 header 内联 + vtable 引用时存在链接问题）。
 */
class UringBufferedStream : public BufferedStream {
public:
    explicit UringBufferedStream(net::Stream& stream);

    /// 精确读取 need 字节（协程版）
    Task<std::vector<uint8_t>> read(size_t need) override;

    /// 读取 1 字节
    Task<uint8_t> readByte() override;

    /// 获取当前 buffer 中未消费的数据（返回拷贝，安全）
    std::vector<uint8_t> drainRemaining() override;

    int fd() const override { return stream_.fd(); }

private:
    size_t available() const {
        return buffer_.size() - consumed_;
    }

    std::vector<uint8_t> consume(size_t n) {
        auto result = std::vector<uint8_t>(
            buffer_.begin() + consumed_,
            buffer_.begin() + consumed_ + n
        );
        consumed_ += n;

        // 全部消费完则回收内存
        if (consumed_ == buffer_.size()) {
            buffer_.clear();
            consumed_ = 0;
        }
        return result;
    }

    net::Stream& stream_;

    // 已缓冲数据
    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;
};

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_BUFFERED_STREAM_H
