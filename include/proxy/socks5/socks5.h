#ifndef VMESS_PROXY_SOCKS5_SOCKS5_H
#define VMESS_PROXY_SOCKS5_SOCKS5_H

#include "coro/buffered_stream.h"
#include "coro/task.h"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vmess {
namespace proxy {
namespace socks5 {

/**
 * @brief SOCKS5 命令类型
 */
enum class Command : uint8_t {
    Connect = 0x01,
    Bind = 0x02,
    UdpAssociate = 0x03
};

/**
 * @brief SOCKS5 地址类型（注意：与 VLESS 的 ATYP 编号不同！）
 */
enum class AddressType : uint8_t {
    IPv4 = 0x01,
    Domain = 0x03,
    IPv6 = 0x04
};

/**
 * @brief SOCKS5 响应码
 */
enum class Reply : uint8_t {
    Success = 0x00,
    GeneralFailure = 0x01,
    NotAllowed = 0x02,
    NetworkUnreachable = 0x03,
    HostUnreachable = 0x04,
    ConnectionRefused = 0x05,
    TtlExpired = 0x06,
    CommandNotSupported = 0x07,
    AddrTypeNotSupported = 0x08
};

/**
 * @brief SOCKS5 目标地址（IPv4 / IPv6 / 域名）
 */
struct Address {
    std::variant<
        std::array<uint8_t, 4>,   // IPv4
        std::array<uint8_t, 16>,  // IPv6
        std::string               // Domain
    > value;

    bool isIPv4() const { return std::holds_alternative<std::array<uint8_t, 4>>(value); }
    bool isIPv6() const { return std::holds_alternative<std::array<uint8_t, 16>>(value); }
    bool isDomain() const { return std::holds_alternative<std::string>(value); }

    AddressType type() const;
    std::string toString() const;

    /// 解析 "1.2.3.4" / "::1" / "example.com" → Address（IP 优先，其余作域名）
    static Address fromHost(const std::string& host);
};

/**
 * @brief 解析后的 SOCKS5 请求
 */
struct Request {
    Command cmd = Command::Connect;
    Address address;
    uint16_t port = 0;
};

/**
 * @brief SOCKS5 协议解析/编码（协程版本，风格对齐 VLESS Decoder）
 *
 * 流程：
 *   1. Parser::readGreeting()      → 客户端 → 代理：VER/NMETHODS/METHODS
 *   2. encodeGreetingResponse()    → 代理 → 客户端：VER/METHOD(0x00 无认证)
 *   3. Parser::readRequest()       → 客户端 → 代理：CMD/RSV/ATYP/ADDR/PORT
 *   4. encodeReply()               → 代理 → 客户端：VER/REP/RSV/ATYP/BND/PORT
 *   5. UDP 数据报：parseUdpDatagram() / encodeUdpDatagram()
 *
 * 注意：写入操作由连接层通过 coro::AsyncStream 完成，本类只负责解析与编码。
 */
class Parser {
public:
    /**
     * @brief 读取握手 greeting 并校验（仅支持无认证 0x00）
     * @return true 客户端支持无认证方法
     */
    static coro::Task<bool> readGreeting(coro::UringBufferedStream& stream);

    /**
     * @brief 编码握手响应：VER=5, METHOD=0（无认证）
     */
    static std::vector<uint8_t> encodeGreetingResponse();

    /**
     * @brief 读取 SOCKS5 请求（CONNECT / UDP ASSOCIATE）
     * @throw std::runtime_error 解析失败时
     */
    static coro::Task<Request> readRequest(coro::UringBufferedStream& stream);

    /**
     * @brief 编码响应头
     * @param reply 响应码
     * @param bindIp 绑定地址（IPv4 字符串，如 "0.0.0.0"）
     * @param bindPort 绑定端口
     */
    static std::vector<uint8_t> encodeReply(Reply reply,
                                            const std::string& bindIp,
                                            uint16_t bindPort);

    /**
     * @brief 解析 SOCKS5 UDP 数据报
     *        RSV(2) + FRAG(1) + ATYP(1) + DST.ADDR + DST.PORT + DATA
     * @param data 收到的 UDP 载荷
     * @param outAddr 输出目标地址
     * @param outPort 输出目标端口
     * @param outPayload 输出去掉头部后的数据
     * @return true 解析成功（FRAG==0 且头部合法）
     */
    static bool parseUdpDatagram(const std::vector<uint8_t>& data,
                                 Address& outAddr,
                                 uint16_t& outPort,
                                 std::vector<uint8_t>& outPayload);

    /**
     * @brief 编码 SOCKS5 UDP 数据报（用于回程，回显目标地址）
     * @param addr 目标地址（回显）
     * @param port 目标端口（回显）
     * @param payload 数据
     */
    static std::vector<uint8_t> encodeUdpDatagram(const Address& addr,
                                                  uint16_t port,
                                                  const std::vector<uint8_t>& payload);
};

} // namespace socks5
} // namespace proxy
} // namespace vmess

#endif // VMESS_PROXY_SOCKS5_SOCKS5_H
