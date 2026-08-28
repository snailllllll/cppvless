#ifndef VLESS_NET_STREAM_H
#define VLESS_NET_STREAM_H

#include "coro/task.h"
#include "coro/uring_awaitable.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vless {
namespace net {

/**
 * @brief 流抽象接口（对齐 Go net.Conn 语义）
 *
 * 明文（AsyncStream）与 TLS（TlsStream）都实现该接口，
 * 协议层（Decoder / Encoder / VlessConnection / Socks5Connection）
 * 只依赖此抽象——TLS 仅是换一个 Stream 实现，协议流程不变。
 *
 * 接口语义（对标 Go）：
 *   read()         → n, err := conn.Read(buf)   （RecvResult 区分 EOF/错误/正常）
 *   writeFull()    → conn.Write(data)           （保证全部写入或返回错误）
 *   shutdownWrite()-> conn.CloseWrite()         （半关闭，发送 FIN）
 *   fd()          → 底层 fd（调试/取消/日志）
 */
class Stream {
public:
    virtual ~Stream() = default;

    /// 读取数据：RecvResult 可区分 EOF / 错误 / 正常
    virtual coro::Task<coro::RecvResult> read(size_t maxBytes = 8192) = 0;

    /// 写入全部数据（处理部分写）；返回实际写入字节数，<=0 表示错误
    virtual coro::Task<int> writeFull(const void* data, size_t len) = 0;

    /// vector 便捷重载（虚，派生类可覆写；默认委托 void* 版本）
    virtual coro::Task<int> writeFull(const std::vector<uint8_t>& data) {
        return writeFull(data.data(), data.size());
    }

    /// 半关闭写方向（发送 FIN），通知对端不再发送数据但仍可接收
    virtual coro::Task<int> shutdownWrite() = 0;

    /// 底层 fd（调试/取消/日志用）
    virtual int fd() const = 0;
};

} // namespace net
} // namespace vless

#endif // VLESS_NET_STREAM_H
