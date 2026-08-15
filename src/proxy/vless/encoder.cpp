#include "proxy/vless/encoder.h"

#include <arpa/inet.h>
#include <cstring>

namespace vmess {
namespace proxy {
namespace vless {

std::vector<uint8_t> Encoder::encodeRequest(const Request& req) {
    std::vector<uint8_t> out;
    out.reserve(64 + req.addressString().size());

    // 1. Version (1B)
    out.push_back(0x00);

    // 2. UUID (16B)
    out.insert(out.end(), req.uuid.begin(), req.uuid.end());

    // 3. Addons: length(1B) + protobuf
    //    纯明文模式：无 flow / 无 encryption，addonsLen = 0
    out.push_back(0x00);

    // 4. Command (1B)
    out.push_back(static_cast<uint8_t>(req.command));

    // 5. Port (2B, big-endian)
    out.push_back(static_cast<uint8_t>((req.port >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(req.port & 0xFF));

    // 6. Address (类型 + 数据)
    writeAddress(out, req);

    return out;
}

std::vector<uint8_t> Encoder::encodeResponse(uint8_t version) {
    return {version, 0x00};
}

coro::Task<bool> Encoder::decodeResponse(coro::BufferedStream& stream) {
    auto data = co_await stream.read(2);
    if (data.size() != 2) {
        co_return false;
    }
    if (data[0] != 0) {
        co_return false;  // 非预期的版本号
    }
    uint8_t addonsLen = data[1];
    if (addonsLen > 0) {
        auto addons = co_await stream.read(addonsLen);
        if (addons.size() != addonsLen) {
            co_return false;
        }
    }
    co_return true;
}

void Encoder::writeAddress(std::vector<uint8_t>& out, const Request& req) {
    if (req.isIPv4()) {
        out.push_back(static_cast<uint8_t>(AddressType::IPv4));
        const auto& ip = std::get<std::array<uint8_t, 4>>(req.address);
        out.insert(out.end(), ip.begin(), ip.end());
    } else if (req.isIPv6()) {
        out.push_back(static_cast<uint8_t>(AddressType::IPv6));
        const auto& ip = std::get<std::array<uint8_t, 16>>(req.address);
        out.insert(out.end(), ip.begin(), ip.end());
    } else if (req.isDomain()) {
        const auto& domain = std::get<std::string>(req.address);
        if (domain.size() > 255) {
            throw std::runtime_error("domain too long for vless address");
        }
        out.push_back(static_cast<uint8_t>(AddressType::Domain));
        out.push_back(static_cast<uint8_t>(domain.size()));
        out.insert(out.end(), domain.begin(), domain.end());
    } else {
        throw std::runtime_error("invalid vless address type");
    }
}

} // namespace vless
} // namespace proxy
} // namespace vmess
