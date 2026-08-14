#ifndef VMESS_PROXY_VLESS_ENCODER_H
#define VMESS_PROXY_VLESS_ENCODER_H

#include "proxy/vless/protocol.h"
#include "coro/buffered_stream.h"
#include "coro/task.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vmess {
namespace proxy {
namespace vless {

/**
 * @brief VLESS 请求头编码器（客户端方向）
 *
 * 是 Decoder::decode 的逆操作，完全对齐 Xray-core 的 EncodeRequestHeader：
 *   version(1B) + uuid(16B) + addonsLen(1B) + addons + command(1B)
 *   + port(2B BE) + addrType(1B) + addr
 *
 * 注意：当前为纯明文 VLESS（addonsLen=0，无 flow/encryption），
 * 与服务端默认配置保持一致，后续可在此扩展 Vision/Encryption。
 */
class Encoder {
public:
    /**
     * @brief 编码 VLESS 请求头
     * @param req 请求（uuid/command/port/address 已填充）
     * @return 编码后的字节序列
     */
    static std::vector<uint8_t> encodeRequest(const Request& req);

    /**
     * @brief 编码 VLESS 响应头（客户端收到服务端响应的验证用）
     */
    static std::vector<uint8_t> encodeResponse(uint8_t version);

    /**
     * @brief 从缓冲流中解码 VLESS 响应头（客户端方向）
     * @param stream 与服务端连接的缓冲流
     * @return true 响应合法（version==0），false 非法
     *
     * 纯明文模式下服务端只返回 2 字节：version + addonsLen(0)。
     * 若将来启用 Encryption，服务端会在响应头后追加 32B 公钥，
     * 此处先按 addonsLen 跳过 addons 数据。
     */
    static coro::Task<bool> decodeResponse(coro::UringBufferedStream& stream);

    /**
     * @brief 将 IP 字节数组与端口构造为地址字节（内部使用）
     */
    static void writeAddress(std::vector<uint8_t>& out, const Request& req);
};

} // namespace vless
} // namespace proxy
} // namespace vmess

#endif // VMESS_PROXY_VLESS_ENCODER_H
