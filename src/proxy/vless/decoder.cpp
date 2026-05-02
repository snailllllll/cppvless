#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"

#include <algorithm>
#include <stdexcept>

namespace vmess {
namespace proxy {
namespace vless {

// 写死的 VLESS UUID：e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b
constexpr std::array<uint8_t, 16> EXPECTED_UUID = {
    0xe3, 0xe7, 0x40, 0xb0, 0x2c, 0x3a, 0x4b, 0x0e,
    0x9f, 0x1a, 0x2c, 0x8f, 0x7d, 0x5e, 0x3a, 0x1b
};

coro::Task<Request> Decoder::decode(coro::UringBufferedStream& stream) {
    Request req;

    // 1. 读取版本 (1B)
    auto versionBytes = co_await stream.read(1);
    req.version = versionBytes[0];

    if (req.version != 0) {
        throw std::runtime_error("invalid vless version");
    }

    // 2. 读取 UUID (16B) 并校验
    auto uuidBytes = co_await stream.read(16);
    std::copy(uuidBytes.begin(), uuidBytes.end(), req.uuid.begin());
    if (!std::equal(uuidBytes.begin(), uuidBytes.end(), EXPECTED_UUID.begin())) {
        throw std::runtime_error("invalid vless uuid");
    }

    // 3. 读取 addons 长度并跳过 (1B + M B)
    co_await skipAddons(stream);

    // 4. 读取指令 (1B)
    auto cmdBytes = co_await stream.read(1);
    req.command = static_cast<Command>(cmdBytes[0]);

    // 5. 根据指令处理
    switch (req.command) {
        case Command::Mux:
            // Mux 模式：固定地址
            req.address = std::string("v1.mux.cool");
            req.port = 0;
            break;

        case Command::TCP:
        case Command::UDP: {
            // 6. 读取端口 (2B, big-endian)
            auto portBytes = co_await stream.read(2);
            req.port = (static_cast<uint16_t>(portBytes[0]) << 8) |
                       static_cast<uint16_t>(portBytes[1]);

            // 7. 读取地址
            co_await readAddress(stream, req);
            break;
        }

        default:
            throw std::runtime_error("invalid vless command");
    }

    co_return req;
}

std::array<uint8_t, 2> Decoder::encodeResponse(uint8_t version) {
    // VLESS 响应头：version(1B) + addons_len(1B)
    // 简化：无 addons，addons_len = 0
    return {version, 0x00};
}

coro::Task<void> Decoder::skipAddons(coro::UringBufferedStream& stream) {
    // 读取 addons 长度 (1B)
    auto lenBytes = co_await stream.read(1);
    uint8_t addonsLen = lenBytes[0];

    // 如果 addonsLen > 0，跳过 addons 数据
    if (addonsLen > 0) {
        co_await stream.read(addonsLen);
    }

    co_return;
}

coro::Task<void> Decoder::readAddress(coro::UringBufferedStream& stream, Request& req) {
    // 读取地址类型 (1B)
    auto typeBytes = co_await stream.read(1);
    auto addrType = static_cast<AddressType>(typeBytes[0]);

    switch (addrType) {
        case AddressType::IPv4: {
            auto ip = co_await stream.read(4);
            std::array<uint8_t, 4> ipv4;
            std::copy(ip.begin(), ip.end(), ipv4.begin());
            req.address = ipv4;
            break;
        }

        case AddressType::Domain: {
            // 读取域名长度 (1B)
            auto lenBytes = co_await stream.read(1);
            uint8_t domainLen = lenBytes[0];

            // 读取域名
            auto domain = co_await stream.read(domainLen);
            req.address = std::string(
                reinterpret_cast<const char*>(domain.data()),
                domain.size()
            );
            break;
        }

        case AddressType::IPv6: {
            auto ip = co_await stream.read(16);
            std::array<uint8_t, 16> ipv6;
            std::copy(ip.begin(), ip.end(), ipv6.begin());
            req.address = ipv6;
            break;
        }

        default:
            throw std::runtime_error("invalid address type");
    }

    co_return;
}

std::string Request::addressString() const {
    if (isIPv4()) {
        auto& ip = std::get<std::array<uint8_t, 4>>(address);
        return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
               std::to_string(ip[2]) + "." + std::to_string(ip[3]);
    } else if (isIPv6()) {
        // 简化：返回 "IPv6" 占位
        return "IPv6";
    } else {
        return std::get<std::string>(address);
    }
}

} // namespace vless
} // namespace proxy
} // namespace vmess
