#ifndef VMESS_CORO_ASYNC_STREAM_H
#define VMESS_CORO_ASYNC_STREAM_H

#include "coro/task.h"
#include "coro/uring_awaitable.h"
#include "net/io_uring.h"
#include "net/stream.h"
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
 *
 * 实现 net::Stream 接口（明文流实现；TLS 场景由 TlsStream 实现同一接口）。
 *
 * 注意：协程方法实现位于 src/coro/async_stream.cpp（GCC 协程 frame 符号
 * 在 header 内联 + vtable 引用时存在链接问题，故下沉到 .cpp）。
 */
class AsyncStream : public net::Stream {
public:
    explicit AsyncStream(int fd, net::IoUring& uring);

    /// 读取数据：RecvResult 可区分 EOF / 错误 / 正常
    Task<RecvResult> read(size_t maxBytes = 8192) override;

    /// 写入全部数据（处理部分写）；返回实际写入字节数，<=0 表示错误
    Task<int> writeFull(const void* data, size_t len) override;

    /// vector 便捷重载
    Task<int> writeFull(const std::vector<uint8_t>& data) override;

    /// 半关闭写方向（发送 FIN）
    Task<int> shutdownWrite() override;

    int fd() const override { return fd_; }

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
inline Task<bool> copyStream(net::Stream& dst, net::Stream& src, const bool& stop) {
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
