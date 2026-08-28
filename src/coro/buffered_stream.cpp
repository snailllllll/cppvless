#include "coro/buffered_stream.h"

namespace vless {
namespace coro {

UringBufferedStream::UringBufferedStream(net::Stream& stream)
    : stream_(stream) {}

Task<std::vector<uint8_t>> UringBufferedStream::read(size_t need) {
    // 循环读取直到 buffer 充足
    while (available() < need) {
        auto rr = co_await stream_.read();
        if (rr.error() || rr.eof()) {
            // EOF 或错误：返回已缓冲的部分数据
            if (available() > 0) {
                co_return consume(available());
            }
            co_return std::vector<uint8_t>{};
        }
        buffer_.insert(buffer_.end(), rr.data.begin(), rr.data.end());
    }

    co_return consume(need);
}

Task<uint8_t> UringBufferedStream::readByte() {
    auto data = co_await read(1);
    if (data.empty()) {
        co_return 0;
    }
    co_return data[0];
}

std::vector<uint8_t> UringBufferedStream::drainRemaining() {
    auto result = std::vector<uint8_t>(
        buffer_.begin() + consumed_,
        buffer_.end()
    );
    consumed_ = buffer_.size();
    return result;
}

} // namespace coro
} // namespace vless
