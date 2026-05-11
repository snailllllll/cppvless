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

    // 3. 读取 addons（解析 Flow 字段）
    //    VLESS addons 格式: length(1B) + protobuf{Flow, Seed, ...}
    //    如果 Flow 为 "xtls-rprx-vision"，数据流需要 Vision 加解密处理
    co_await parseAddons(stream, req);

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

coro::Task<void> Decoder::parseAddons(coro::UringBufferedStream& stream, Request& req) {
    // VLESS addons 格式（参考 Xray-core proxy/vless/encoding/addons.go）:
    //   length(1B) + protobuf(Addons{Flow, Seed, ...})
    //
    // Protobuf 编码（简化解析）：
    //   field 1 (Flow): tag=0x0A, length-delimited → string
    //   field 2 (Seed): tag=0x12, length-delimited → string
    //
    // 常见 Flow 值：
    //   ""                        → 无 Flow，明文转发
    //   "xtls-rprx-vision"        → Vision 流控（需加解密处理）

    auto lenBytes = co_await stream.read(1);
    uint8_t addonsLen = lenBytes[0];

    if (addonsLen == 0) {
        // 无 addons
        co_return;
    }

    // 读取 addons protobuf 数据
    auto addonsData = co_await stream.read(addonsLen);

    // 简化 protobuf 解析：只提取 field 1 (Flow)
    // protobuf 格式: tag(varint) + value
    // tag = (field_number << 3) | wire_type
    // field 1, wire type 2 (length-delimited) → tag = 0x0A
    size_t pos = 0;
    while (pos < addonsData.size()) {
        uint8_t tag = addonsData[pos++];
        uint8_t fieldNum = tag >> 3;
        uint8_t wireType = tag & 0x07;

        if (wireType == 2) {
            // Length-delimited: 读取 varint 长度 + 数据
            uint8_t len = addonsData[pos++];
            if (pos + len > addonsData.size()) break;

            if (fieldNum == 1) {
                // Field 1 = Flow
                req.flow = std::string(
                    reinterpret_cast<const char*>(addonsData.data() + pos), len);
            } else if (fieldNum == 3) {
                // Field 3 = Encryption (Xray VLESS addons)
                req.encryption = std::string(
                    reinterpret_cast<const char*>(addonsData.data() + pos), len);
            }
            pos += len;
        } else if (wireType == 0) {
            // Varint: 跳过
            while (pos < addonsData.size() && (addonsData[pos] & 0x80)) {
                pos++;
            }
            if (pos < addonsData.size()) pos++;
        } else {
            // 不支持的 wire type，停止解析
            break;
        }
    }
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
