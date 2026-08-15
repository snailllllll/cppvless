#include "proxy/vless/decoder.h"
#include "proxy/vless/protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <stdexcept>

namespace vmess {
namespace proxy {
namespace vless {

namespace {

uint64_t decodeVarint(const std::vector<uint8_t>& bytes, size_t& pos) {
    uint64_t value = 0;
    int shift = 0;
    while (pos < bytes.size() && shift <= 63) {
        uint8_t b = bytes[pos++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return value;
        }
        shift += 7;
    }
    throw std::runtime_error("invalid addons varint");
}

} // namespace

coro::Task<Request> Decoder::decode(coro::BufferedStream& stream, const Validator& validator) {
    Request req;
    auto readExact = [&](size_t n, const char* err) -> coro::Task<std::vector<uint8_t>> {
        auto data = co_await stream.read(n);
        if (data.size() != n) {
            throw std::runtime_error(err);
        }
        co_return data;
    };

    // 1. 读取版本 (1B)
    auto versionBytes = co_await readExact(1, "failed to read vless version");
    req.version = versionBytes[0];

    if (req.version != 0) {
        throw std::runtime_error("invalid vless version");
    }

    // 2. 读取 UUID (16B) 并校验
    auto uuidBytes = co_await readExact(16, "failed to read vless uuid");
    std::copy(uuidBytes.begin(), uuidBytes.end(), req.uuid.begin());
    if (!validator.contains(req.uuid)) {
        throw std::runtime_error("invalid vless uuid");
    }

    // 3. 读取 addons（解析 Flow 字段）
    //    VLESS addons 格式: length(1B) + protobuf{Flow, Seed, ...}
    //    如果 Flow 为 "xtls-rprx-vision"，数据流需要 Vision 加解密处理
    co_await parseAddons(stream, req);

    // 4. 读取指令 (1B)
    auto cmdBytes = co_await readExact(1, "failed to read vless command");
    req.command = static_cast<Command>(cmdBytes[0]);

    // 5. 根据指令处理
    switch (req.command) {
        case Command::Mux:
            // Mux 模式：固定地址
            req.address = std::string("v1.mux.cool");
            req.port = 0;
            break;
        case Command::Rvs:
            // Reverse 模式：固定地址
            req.address = std::string("v1.rvs.cool");
            req.port = 0;
            break;

        case Command::TCP:
        case Command::UDP: {
            // 6. 读取端口 (2B, big-endian)
            auto portBytes = co_await readExact(2, "failed to read vless port");
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

coro::Task<void> Decoder::parseAddons(coro::BufferedStream& stream, Request& req) {
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
    if (lenBytes.size() != 1) {
        throw std::runtime_error("failed to read addons length");
    }
    uint8_t addonsLen = lenBytes[0];

    if (addonsLen == 0) {
        // 无 addons
        co_return;
    }

    // 读取 addons protobuf 数据
    auto addonsData = co_await stream.read(addonsLen);
    if (addonsData.size() != addonsLen) {
        throw std::runtime_error("failed to read addons payload");
    }

    // protobuf 解析：提取 field 1 (Flow) / field 2 (Seed)
    size_t pos = 0;
    while (pos < addonsData.size()) {
        uint64_t tag = decodeVarint(addonsData, pos);
        uint64_t fieldNum = tag >> 3;
        uint64_t wireType = tag & 0x07;

        if (wireType == 2) {
            uint64_t len64 = decodeVarint(addonsData, pos);
            if (len64 > addonsData.size() - pos) {
                throw std::runtime_error("invalid addons length-delimited field");
            }
            size_t len = static_cast<size_t>(len64);

            if (fieldNum == 1) {
                req.flow = std::string(
                    reinterpret_cast<const char*>(addonsData.data() + pos), len);
            } else if (fieldNum == 2) {
                req.seed.assign(addonsData.begin() + static_cast<std::ptrdiff_t>(pos),
                                addonsData.begin() + static_cast<std::ptrdiff_t>(pos + len));
            }
            pos += len;
        } else if (wireType == 0) {
            (void)decodeVarint(addonsData, pos);
        } else {
            throw std::runtime_error("unsupported addons wire type");
        }
    }
}

coro::Task<void> Decoder::readAddress(coro::BufferedStream& stream, Request& req) {
    // 读取地址类型 (1B)
    auto typeBytes = co_await stream.read(1);
    if (typeBytes.size() != 1) {
        throw std::runtime_error("failed to read address type");
    }
    auto addrType = static_cast<AddressType>(typeBytes[0]);

    switch (addrType) {
        case AddressType::IPv4: {
            auto ip = co_await stream.read(4);
            if (ip.size() != 4) {
                throw std::runtime_error("failed to read ipv4 address");
            }
            std::array<uint8_t, 4> ipv4;
            std::copy(ip.begin(), ip.end(), ipv4.begin());
            req.address = ipv4;
            break;
        }

        case AddressType::Domain: {
            // 读取域名长度 (1B)
            auto lenBytes = co_await stream.read(1);
            if (lenBytes.size() != 1) {
                throw std::runtime_error("failed to read domain length");
            }
            uint8_t domainLen = lenBytes[0];

            // 读取域名
            auto domain = co_await stream.read(domainLen);
            if (domain.size() != domainLen) {
                throw std::runtime_error("failed to read domain value");
            }
            req.address = std::string(
                reinterpret_cast<const char*>(domain.data()),
                domain.size()
            );
            break;
        }

        case AddressType::IPv6: {
            auto ip = co_await stream.read(16);
            if (ip.size() != 16) {
                throw std::runtime_error("failed to read ipv6 address");
            }
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
        auto& ip = std::get<std::array<uint8_t, 16>>(address);
        char buf[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, ip.data(), buf, sizeof(buf)) == nullptr) {
            return "invalid-ipv6";
        }
        return std::string(buf);
    } else {
        return std::get<std::string>(address);
    }
}

} // namespace vless
} // namespace proxy
} // namespace vmess
