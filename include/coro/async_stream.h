#ifndef VMESS_CORO_ASYNC_STREAM_H
#define VMESS_CORO_ASYNC_STREAM_H

#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "net/io_uring.h"
#include "common/log.h"

#include <cstdint>
#include <cstring>

namespace vmess {
namespace coro {

/**
 * @brief 异步流封装（Go-like: net.Conn 风格）
 * 
 * 封装 fd + IoUring，提供 read/write 高层接口：
 * 
 *   auto client = AsyncStream(clientFd, uring);
 *   auto target = AsyncStream(targetFd, uring);
 *   auto data = co_await client.read();      // 读取数据
 *   co_await target.writeFull(data);          // 写入全部数据
 *   co_await target.shutdownWrite();          // 半关闭（发送 FIN）
 *
 * 对标 Go:
 *   data := make([]byte, 4096)
 *   n, err := conn.Read(data)
 *   conn.Write(data[:n])
 *   conn.CloseWrite()  // 或 net.TCPConn.CloseWrite()
 */
class AsyncStream {
public:
    explicit AsyncStream(int fd, net::IoUring& uring)
        : fd_(fd), uring_(uring) {}

    /**
     * @brief 读取数据
     * @param maxBytes 缓冲区大小
     * @return RecvResult: 可区分 EOF / 错误 / 正常
     * 
     * 对标 Go: n, err := conn.Read(buf)
     *   rr.ok()     → n > 0, err == nil
     *   rr.eof()    → n == 0, err == io.EOF
     *   rr.error()  → n == 0, err != nil
     */
    Task<RecvResult> read(size_t maxBytes = 8192) {
        co_return co_await AsyncRecv(fd_, uring_, maxBytes);
    }

    /**
     * @brief 写入全部数据（处理部分写）
     * @return 实际写入字节数，<=0 表示错误
     * 
     * 对标 Go: conn.Write(data)  // Go 的 Write 保证全部写入或返回错误
     */
    Task<int> writeFull(const void* data, size_t len) {
        size_t offset = 0;
        while (offset < len) {
            int sent = co_await AsyncSend(fd_, uring_,
                static_cast<const uint8_t*>(data) + offset,
                len - offset);
            if (sent <= 0) {
                co_return (offset > 0) ? static_cast<int>(offset) : sent;
            }
            offset += static_cast<size_t>(sent);
        }
        co_return static_cast<int>(len);
    }

    Task<int> writeFull(const std::vector<uint8_t>& data) {
        co_return co_await writeFull(data.data(), data.size());
    }

    /**
     * @brief 半关闭写方向（发送 FIN）
     * 
     * 对标 Go: tcpConn.CloseWrite() 或 task.Close(writer)
     * 通知对端我们不再发送数据，但仍可接收
     */
    Task<int> shutdownWrite() {
        co_return co_await AsyncShutdown(fd_, uring_, SHUT_WR);
    }

    int fd() const { return fd_; }

private:
    int fd_;
    net::IoUring& uring_;
};

/**
 * @brief 单向数据拷贝：src → dst
 * 
 * 对标 Go: buf.Copy(dstWriter, srcReader)
 * 
 * @param dst 目标流
 * @param src 源流
 * @param stop 停止标志（另一方向设置）
 * @return true 正常结束（EOF），false 异常中断
 * 
 * 参考 Xray 的关闭传播模型：
 *   - 正常 EOF (readError+io.EOF) → shutdown dst 写端，传播半关闭
 *   - 读端错误 (readError+其他)   → shutdown dst 写端，通知对端"不会再有数据"
 *   - 写端错误 (writeError)       → shutdown src 读端（通过关闭 src fd 触发 EOF）
 * 
 * 关键设计：无论正常还是异常，都执行 shutdownWrite()。
 * 原因：另一个方向的协程可能正阻塞在 recv 上等待数据，
 * 如果不 shutdown，它会一直等下去，直到超时或对端主动关闭。
 * shutdown SHUT_WR 会发送 FIN，让对端 recv 返回 0（EOF），从而优雅退出。
 */
inline Task<bool> copyStream(AsyncStream& dst, AsyncStream& src, const bool& stop) {
    while (!stop) {
        auto rr = co_await src.read();
        if (rr.eof()) {
            // 对标 Go: buf.Copy 读到 EOF → 返回 nil（正常结束）
            // 半关闭目标写端，通知下游我们不再发送
            co_await dst.shutdownWrite();
            co_return true;
        }
        if (rr.error()) {
            // 对标 Go: readError → 仍然关闭 dst 写端
            // 参考 Xray: IsReadError(err) → 对端写方向也该关闭
            // 即使是 ECONNRESET 等异常，也要通知 dst 侧不再有数据到来
            LOG_WARN("copyStream", "src fd=", src.fd(), " read error: ", rr.result,
                     " (", strerror(-rr.result), ")");
            co_await dst.shutdownWrite();
            co_return false;
        }

        int written = co_await dst.writeFull(rr.data);
        if (written <= 0) {
            // 对标 Go: writeError → 关闭 src 读端
            // 写入失败意味着 dst 侧已断开，src 侧也没必要继续读了
            LOG_WARN("copyStream", "dst fd=", dst.fd(), " write error: ", written);
            co_return false;
        }
    }
    co_return false;
}

} // namespace coro
} // namespace vmess

#endif // VMESS_CORO_ASYNC_STREAM_H
