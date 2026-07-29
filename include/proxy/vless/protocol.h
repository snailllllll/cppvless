#ifndef VMESS_PROXY_VLESS_PROTOCOL_H
#define VMESS_PROXY_VLESS_PROTOCOL_H

#include <cstdint>
#include <array>
#include <string>
#include <variant>
#include <vector>

namespace vmess {
namespace proxy {
namespace vless {

/**
 * @brief VLESS 地址类型
 */
enum class AddressType : uint8_t {
    IPv4 = 1,
    Domain = 2,
    IPv6 = 3
};

/**
 * @brief VLESS 指令类型
 */
enum class Command : uint8_t {
    TCP = 1,
    UDP = 2,
    Mux = 3,
    Rvs = 4
};

/**
 * @brief VLESS 请求结构
 */
struct Request {
    uint8_t version = 0;
    std::array<uint8_t, 16> uuid{};
    Command command = Command::TCP;
    uint16_t port = 0;
    std::string flow;                  // VLESS Flow 字段（如 "xtls-rprx-vision"）
    std::string encryption;            // VLESS Encryption 字段（如 "aes-256-gcm", "chacha20-poly1305"）
    std::vector<uint8_t> seed;         // Addons.Seed（Xray proto field=2）
    std::variant<
        std::array<uint8_t, 4>,   // IPv4
        std::array<uint8_t, 16>,  // IPv6
        std::string               // Domain
    > address;

    bool isIPv4() const {
        return std::holds_alternative<std::array<uint8_t, 4>>(address);
    }
    bool isIPv6() const {
        return std::holds_alternative<std::array<uint8_t, 16>>(address);
    }
    bool isDomain() const {
        return std::holds_alternative<std::string>(address);
    }

    std::string addressString() const;
};

/**
 * @brief VLESS 响应头（简化版）
 */
struct Response {
    uint8_t version = 0;
    // 简化版：无 addons
};

} // namespace vless
} // namespace proxy
} // namespace vmess

#endif // VMESS_PROXY_VLESS_PROTOCOL_H
