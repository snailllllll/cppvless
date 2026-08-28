#ifndef VLESS_CLIENT_VLESS_CLIENT_H
#define VLESS_CLIENT_VLESS_CLIENT_H

#include "coro/async_stream.h"
#include "coro/task.h"
#include "net/io_uring.h"
#include "net/stream.h"
#include "proxy/vless/protocol.h"
#include "proxy/socks5/socks5.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vless {
namespace client {

/**
 * @brief 客户端配置（连接远端 VLESS 服务器）
 */
struct VlessClientConfig {
    std::string remoteHost;                    // VLESS 服务器地址（IP 或域名）
    uint16_t remotePort = 443;                 // VLESS 服务器端口
    std::array<uint8_t, 16> uuid{};            // VLESS 用户 UUID
    bool tlsEnabled = false;                   // 启用 TLS 传输（VLESS+TLS）
    bool tlsInsecure = false;                  // 跳过对端证书校验（自签证书场景）
    std::shared_ptr<void> tlsCtx;              // 客户端 SSL_CTX（shared_ptr 管理，deleter=SSL_CTX_free）

    /// 解析 "host:port" 形式字符串（port 缺省用默认值）
    static VlessClientConfig fromString(const std::string& s);
};

/**
 * @brief 将 SOCKS5 请求转换为 VLESS 请求
 * @param req SOCKS5 请求（CONNECT / UDP ASSOCIATE）
 * @param cmd VLESS 指令（TCP / UDP）
 */
proxy::vless::Request toVlessRequest(const proxy::socks5::Request& req,
                                     proxy::vless::Command cmd);

/**
 * @brief VLESS 客户端握手结果
 */
struct VlessClientHandshakeResult {
    int remoteFd = -1;                  // 与远端 VLESS 服务器的 TCP 连接（<0 表示失败）
    std::shared_ptr<net::Stream> stream;    // 与远端的数据流（TLS 或明文）；中继阶段使用
    std::shared_ptr<coro::AsyncStream> raw; // 底层明文流（TLS 时被 TlsStream 引用，需保活）
    std::vector<uint8_t> remaining;     // 握手阶段缓冲流中多读出的数据（对端已开始转发）
};

/**
 * @brief 连接远端 VLESS 服务器并完成握手
 *
 * 流程：
 *   1. 解析远端地址（getaddrinfo）
 *   2. 创建 TCP socket 并异步 connect（多地址回退）
 *   3. 发送编码后的 VLESS 请求头
 *   4. 读取并校验响应头
 *
 * @param uring io_uring 实例
 * @param cfg 客户端配置
 * @param vlessReq VLESS 请求（uuid/command/port/address）
 * @return 握手结果（remoteFd >= 0 表示成功）
 */
coro::Task<VlessClientHandshakeResult> vlessConnectAndHandshake(
    net::IoUring& uring,
    const VlessClientConfig& cfg,
    const proxy::vless::Request& vlessReq);

} // namespace client
} // namespace vless

#endif // VLESS_CLIENT_VLESS_CLIENT_H
