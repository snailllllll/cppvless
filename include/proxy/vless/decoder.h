#ifndef VMESS_PROXY_VLESS_DECODER_H
#define VMESS_PROXY_VLESS_DECODER_H

#include "proxy/vless/protocol.h"
#include "proxy/vless/validator.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"

namespace vmess {
namespace proxy {
namespace vless {

/**
 * @brief VLESS 请求头解码器（协程版本）
 * 
 * 直接翻译 Go 官方的 DecodeRequestHeader 逻辑：
 * https://github.com/v2fly/v2ray-core/blob/master/proxy/vless/encoding/encoding.go
 */
class Decoder {
public:
    /**
     * @brief 从缓冲流中解码 VLESS 请求头
     * @param stream 缓冲流（抽象接口，明文/TLS 均可）
     * @return 解析后的请求
     * 
     * 使用方式：
     *   auto req = co_await Decoder::decode(stream);
     */
    static coro::Task<Request> decode(coro::BufferedStream& stream, const Validator& validator);

    /**
     * @brief 编码响应头
     * @param version 协议版本
     * @return 编码后的字节序列
     */
    static std::array<uint8_t, 2> encodeResponse(uint8_t version);

private:
    static coro::Task<void> parseAddons(coro::BufferedStream& stream, Request& req);
    static coro::Task<void> readAddress(coro::BufferedStream& stream, Request& req);
};

} // namespace vless
} // namespace proxy
} // namespace vmess

#endif // VMESS_PROXY_VLESS_DECODER_H
