#include "proxy/vless/validator.h"

#include <algorithm>
#include <cstdio>

namespace vless {
namespace proxy {
namespace vless {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

} // namespace

bool Validator::parseUuid(const std::string& uuid, std::array<uint8_t, 16>& out) {
    std::string compact;
    compact.reserve(32);
    for (char c : uuid) {
        if (c == '-') continue;
        compact.push_back(c);
    }

    if (compact.size() != 32) {
        return false;
    }

    for (size_t i = 0; i < 16; ++i) {
        int hi = hexValue(compact[i * 2]);
        int lo = hexValue(compact[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool Validator::addFromString(const std::string& uuid) {
    std::array<uint8_t, 16> parsed{};
    if (!parseUuid(uuid, parsed)) {
        return false;
    }
    add(parsed);
    return true;
}

void Validator::add(const std::array<uint8_t, 16>& uuid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (std::find(users_.begin(), users_.end(), uuid) == users_.end()) {
        users_.push_back(uuid);
    }
}

bool Validator::remove(const std::array<uint8_t, 16>& uuid) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find(users_.begin(), users_.end(), uuid);
    if (it == users_.end()) {
        return false;
    }
    users_.erase(it);
    return true;
}

bool Validator::contains(const std::array<uint8_t, 16>& uuid) const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::find(users_.begin(), users_.end(), uuid) != users_.end();
}

size_t Validator::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return users_.size();
}

std::string Validator::formatUuid(const std::array<uint8_t, 16>& uuid) {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
                  uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
                  uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(buf);
}

} // namespace vless
} // namespace proxy
} // namespace vless
