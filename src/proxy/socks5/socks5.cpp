#include "proxy/socks5/socks5.h"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>

namespace vmess {
namespace proxy {
namespace socks5 {

AddressType Address::type() const {
    if (isIPv4()) return AddressType::IPv4;
    if (isIPv6()) return AddressType::IPv6;
    return AddressType::Domain;
}

std::string Address::toString() const {
    if (isIPv4()) {
        const auto& ip = std::get<std::array<uint8_t, 4>>(value);
        return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
               std::to_string(ip[2]) + "." + std::to_string(ip[3]);
    }
    if (isIPv6()) {
        const auto& ip = std::get<std::array<uint8_t, 16>>(value);
        char buf[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, ip.data(), buf, sizeof(buf)) == nullptr) {
            return "invalid-ipv6";
        }
        return std::string(buf);
    }
    return std::get<std::string>(value);
}

Address Address::fromHost(const std::string& host) {
    Address addr;
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        std::array<uint8_t, 4> ip;
        std::memcpy(ip.data(), &v4, 4);
        addr.value = ip;
        return addr;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        std::array<uint8_t, 16> ip;
        std::memcpy(ip.data(), &v6, 16);
        addr.value = ip;
        return addr;
    }
    addr.value = host;
    return addr;
}

namespace {

coro::Task<std::vector<uint8_t>> readExact(coro::UringBufferedStream& stream,
                                           size_t n,
                                           const char* err) {
    auto data = co_await stream.read(n);
    if (data.size() != n) {
        throw std::runtime_error(err);
    }
    co_return data;
}

void writeAddrPort(std::vector<uint8_t>& out, const Address& addr, uint16_t port) {
    out.push_back(static_cast<uint8_t>(addr.type()));
    if (addr.isIPv4()) {
        const auto& ip = std::get<std::array<uint8_t, 4>>(addr.value);
        out.insert(out.end(), ip.begin(), ip.end());
    } else if (addr.isIPv6()) {
        const auto& ip = std::get<std::array<uint8_t, 16>>(addr.value);
        out.insert(out.end(), ip.begin(), ip.end());
    } else {
        const auto& domain = std::get<std::string>(addr.value);
        if (domain.size() > 255) {
            throw std::runtime_error("domain too long for socks5 address");
        }
        out.push_back(static_cast<uint8_t>(domain.size()));
        out.insert(out.end(), domain.begin(), domain.end());
    }
    out.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(port & 0xFF));
}

} // namespace

coro::Task<bool> Parser::readGreeting(coro::UringBufferedStream& stream) {
    auto verBytes = co_await readExact(stream, 1, "failed to read socks5 version");
    if (verBytes[0] != 0x05) {
        co_return false;
    }

    auto nmethodsBytes = co_await readExact(stream, 1, "failed to read socks5 nmethods");
    uint8_t nmethods = nmethodsBytes[0];
    if (nmethods == 0) {
        co_return false;
    }

    auto methods = co_await readExact(stream, nmethods, "failed to read socks5 methods");
    for (uint8_t m : methods) {
        if (m == 0x00) {  // NO AUTHENTICATION REQUIRED
            co_return true;
        }
    }
    co_return false;
}

std::vector<uint8_t> Parser::encodeGreetingResponse() {
    return {0x05, 0x00};  // VER=5, METHOD=0 (no auth)
}

coro::Task<Request> Parser::readRequest(coro::UringBufferedStream& stream) {
    Request req;

    auto verBytes = co_await readExact(stream, 1, "failed to read socks5 request version");
    if (verBytes[0] != 0x05) {
        throw std::runtime_error("invalid socks5 request version");
    }

    auto cmdBytes = co_await readExact(stream, 1, "failed to read socks5 command");
    req.cmd = static_cast<Command>(cmdBytes[0]);

    auto rsvBytes = co_await readExact(stream, 1, "failed to read socks5 rsv");
    (void)rsvBytes;

    auto atypBytes = co_await readExact(stream, 1, "failed to read socks5 address type");
    auto atyp = static_cast<AddressType>(atypBytes[0]);

    switch (atyp) {
        case AddressType::IPv4: {
            auto ip = co_await readExact(stream, 4, "failed to read socks5 ipv4");
            std::array<uint8_t, 4> v4;
            std::copy(ip.begin(), ip.end(), v4.begin());
            req.address.value = v4;
            break;
        }
        case AddressType::Domain: {
            auto lenBytes = co_await readExact(stream, 1, "failed to read socks5 domain length");
            uint8_t len = lenBytes[0];
            auto domain = co_await readExact(stream, len, "failed to read socks5 domain");
            req.address.value = std::string(
                reinterpret_cast<const char*>(domain.data()), domain.size());
            break;
        }
        case AddressType::IPv6: {
            auto ip = co_await readExact(stream, 16, "failed to read socks5 ipv6");
            std::array<uint8_t, 16> v6;
            std::copy(ip.begin(), ip.end(), v6.begin());
            req.address.value = v6;
            break;
        }
        default:
            throw std::runtime_error("unsupported socks5 address type");
    }

    auto portBytes = co_await readExact(stream, 2, "failed to read socks5 port");
    req.port = (static_cast<uint16_t>(portBytes[0]) << 8) |
               static_cast<uint16_t>(portBytes[1]);

    co_return req;
}

std::vector<uint8_t> Parser::encodeReply(Reply reply,
                                         const std::string& bindIp,
                                         uint16_t bindPort) {
    Address bind = Address::fromHost(bindIp.empty() ? "0.0.0.0" : bindIp);
    std::vector<uint8_t> out;
    out.reserve(22);
    out.push_back(0x05);  // VER
    out.push_back(static_cast<uint8_t>(reply));  // REP
    out.push_back(0x00);  // RSV
    writeAddrPort(out, bind, bindPort);  // ATYP + BND.ADDR + BND.PORT
    return out;
}

bool Parser::parseUdpDatagram(const std::vector<uint8_t>& data,
                              Address& outAddr,
                              uint16_t& outPort,
                              std::vector<uint8_t>& outPayload) {
    if (data.size() < 4) {
        return false;
    }
    // RSV(2) + FRAG(1)
    uint8_t frag = data[2];
    if (frag != 0) {
        return false;  // 不支持分片
    }
    size_t pos = 3;
    auto atyp = static_cast<AddressType>(data[pos++]);

    switch (atyp) {
        case AddressType::IPv4: {
            if (data.size() < pos + 4 + 2) return false;
            std::array<uint8_t, 4> v4;
            std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos),
                      data.begin() + static_cast<std::ptrdiff_t>(pos + 4), v4.begin());
            outAddr.value = v4;
            pos += 4;
            break;
        }
        case AddressType::Domain: {
            if (data.size() < pos + 1) return false;
            uint8_t len = data[pos++];
            if (data.size() < pos + len + 2) return false;
            outAddr.value = std::string(
                reinterpret_cast<const char*>(data.data() + pos), len);
            pos += len;
            break;
        }
        case AddressType::IPv6: {
            if (data.size() < pos + 16 + 2) return false;
            std::array<uint8_t, 16> v6;
            std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos),
                      data.begin() + static_cast<std::ptrdiff_t>(pos + 16), v6.begin());
            outAddr.value = v6;
            pos += 16;
            break;
        }
        default:
            return false;
    }

    if (data.size() < pos + 2) return false;
    outPort = (static_cast<uint16_t>(data[pos]) << 8) |
              static_cast<uint16_t>(data[pos + 1]);
    pos += 2;

    outPayload.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    return true;
}

std::vector<uint8_t> Parser::encodeUdpDatagram(const Address& addr,
                                               uint16_t port,
                                               const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(24 + payload.size());
    out.push_back(0x00);  // RSV
    out.push_back(0x00);  // RSV
    out.push_back(0x00);  // FRAG
    writeAddrPort(out, addr, port);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace socks5
} // namespace proxy
} // namespace vmess
