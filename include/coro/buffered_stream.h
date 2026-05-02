#ifndef VMESS_CORO_BUFFERED_STREAM_H
#define VMESS_CORO_BUFFERED_STREAM_H

#include "common/log.h"
#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "net/io_uring.h"

#include <cstdint>
#include <vector>

namespace vmess {
namespace coro {

/**
 * @brief 为 io_uring 协程设计的缓冲流（纯协程版本）
 * 
 * 核心设计：
 * - 协程小量 read(1), read(16) → 从 buffer 消费
 * - buffer 不够时，直接 co_await AsyncRecv 循环读取，直到数据充足
 * - 不再依赖 prepareIO/onIOComplete 回调对
 * 
 * 返回 vector<uint8_t> 而非 span，确保数据所有权安全。
 */
class UringBufferedStream {
public:
    explicit UringBufferedStream(int fd, net::IoUring& uring)
        : fd_(fd), uring_(uring) {}

    /**
     * @brief 读取 need 字节（协程版）
     * @return Task<std::vector<uint8_t>>
     * 
     * 使用方式：
     *   auto data = co_await stream.read(16);
     * 
     * 如果 buffer 已有足够数据，不发起任何 syscall。
     * 否则循环 co_await AsyncRecv 直到数据充足。
     */
    Task<std::vector<uint8_t>> read(size_t need) {
        // 循环 recv 直到 buffer 充足
        while (available() < need) {
            auto data = co_await AsyncRecv(fd_, uring_);
            if (data.empty()) {
                // EOF 或错误：返回已缓冲的部分数据
                if (available() > 0) {
                    co_return consume(available());
                }
                co_return std::vector<uint8_t>{};
            }
            buffer_.insert(buffer_.end(), data.begin(), data.end());
        }

        co_return consume(need);
    }

    /// 读取 1 字节
    Task<uint8_t> readByte() {
        auto data = co_await read(1);
        if (data.empty()) {
            co_return 0;
        }
        co_return data[0];
    }

    /// 获取当前 buffer 中未消费的数据（返回拷贝，安全）
    std::vector<uint8_t> drainRemaining() {
        auto result = std::vector<uint8_t>(
            buffer_.begin() + consumed_,
            buffer_.end()
        );
        consumed_ = buffer_.size();
        return result;
    }

    int fd() const { return fd_; }

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

    int fd_;
    net::IoUring& uring_;

    // 已缓冲数据
    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;
};

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_BUFFERED_STREAM_H
