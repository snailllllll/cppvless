#ifndef VMESS_CORO_BUFFERED_STREAM_H
#define VMESS_CORO_BUFFERED_STREAM_H

#include "common/log.h"

#include <coroutine>
#include <cstdint>
#include <span>
#include <vector>
#include <array>

namespace vmess {
namespace coro {

/**
 * @brief 为 io_uring 协程设计的缓冲流
 * 
 * 核心设计：
 * - 主循环大块 recv (4KB) → feed 到 buffer
 * - 协程小量 read(1), read(16) → 从 buffer 消费
 * - buffer 足够时 await_ready() 直接返回，零 syscall
 */
class UringBufferedStream {
public:
    explicit UringBufferedStream(int fd) : fd_(fd) {}

    /**
     * @brief 读取 need 字节
     * @return ReadAwaitable 可等待对象
     * 
     * 使用方式：
     *   auto data = co_await stream.read(16);
     */
    auto read(size_t need) {
        struct ReadAwaitable {
            UringBufferedStream& s;
            size_t need;

            // 关键优化：buffer 够就不挂起
            bool await_ready() const {
                bool ready = s.available() >= need;
                LOG_DEBUG("UringBufferedStream", "fd=", s.fd_, " read(", need,
                          ") await_ready=", ready, ", available=", s.available());
                return ready;
            }

            void await_suspend(std::coroutine_handle<> h) {
                LOG_DEBUG("UringBufferedStream", "fd=", s.fd_, " read(", need, ") SUSPEND");
                s.pendingNeed_ = need;
                s.pendingHandle_ = h;
                s.needRead_ = true;  // 通知主循环：需要底层读
            }

            std::span<uint8_t> await_resume() {
                return s.consume(need);
            }
        };
        return ReadAwaitable{*this, need};
    }

    // 便捷方法
    auto readByte() { return read(1); }

    // ── 主循环使用 ──

    /**
     * @brief 喂数据到 buffer
     * @param data 数据指针
     * @param len 数据长度
     * 
     * 主循环在 CQE 到达时调用。
     * 如果 buffer 满足了 pending 的 read，自动 resume 协程。
     */
    void feed(const uint8_t* data, size_t len) {
        if (len == 0) return;
        LOG_DEBUG("UringBufferedStream", "fd=", fd_, " feed(", len,
                  "), pendingNeed=", pendingNeed_, ", before_avail=", available());
        buffer_.insert(buffer_.end(), data, data + len);

        if (pendingHandle_ && available() >= pendingNeed_) {
            LOG_DEBUG("UringBufferedStream", "fd=", fd_, " RESUME coroutine, avail=", available());
            auto h = pendingHandle_;
            pendingHandle_ = nullptr;
            h.resume();
            LOG_DEBUG("UringBufferedStream", "fd=", fd_, " RESUME returned");
        }
    }

    bool needsRead() const { return needRead_; }
    void clearNeedRead() { needRead_ = false; }
    int fd() const { return fd_; }

    // 供主循环 prepareRecv 的接收缓冲区
    uint8_t* recvBuffer() { return recvBuf_.data(); }
    static constexpr size_t recvBufferSize() { return 4096; }

    // 获取当前 buffer 中未消费的数据（用于零拷贝移交）
    std::span<uint8_t> drainRemaining() {
        auto result = std::span<uint8_t>(
            buffer_.data() + consumed_,
            buffer_.size() - consumed_
        );
        consumed_ = buffer_.size();
        return result;
    }

private:
    size_t available() const {
        return buffer_.size() - consumed_;
    }

    std::span<uint8_t> consume(size_t n) {
        auto* ptr = buffer_.data() + consumed_;
        consumed_ += n;

        // 全部消费完则回收内存
        if (consumed_ == buffer_.size()) {
            buffer_.clear();
            consumed_ = 0;
        }
        return {ptr, n};
    }

    int fd_;

    // 已缓冲数据
    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;

    // 底层接收缓冲区（供 io_uring 直接 DMA）
    alignas(alignof(std::max_align_t))
    std::array<uint8_t, 4096> recvBuf_;

    // pending 读请求
    size_t pendingNeed_ = 0;
    std::coroutine_handle<> pendingHandle_ = nullptr;
    bool needRead_ = false;
};

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_BUFFERED_STREAM_H
