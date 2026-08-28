#include "coro/async_stream.h"

#include <sys/socket.h>

namespace vless {
namespace coro {

AsyncStream::AsyncStream(int fd, net::IoUring& uring)
    : fd_(fd), uring_(uring) {}

Task<RecvResult> AsyncStream::read(size_t maxBytes) {
    co_return co_await AsyncRecv(fd_, uring_, maxBytes);
}

Task<int> AsyncStream::writeFull(const void* data, size_t len) {
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

Task<int> AsyncStream::writeFull(const std::vector<uint8_t>& data) {
    co_return co_await writeFull(data.data(), data.size());
}

Task<int> AsyncStream::shutdownWrite() {
    co_return co_await AsyncShutdown(fd_, uring_, SHUT_WR);
}

} // namespace coro
} // namespace vless
