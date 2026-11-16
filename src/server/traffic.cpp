#include "server/traffic.h"
#include "proxy/vless/validator.h"

#include <openssl/rand.h>

#include <mutex>
#include <unordered_map>

namespace vless {
namespace server {

namespace {

struct KeyHash {
    size_t operator()(const std::array<uint8_t, 16>& id) const {
        uint64_t x = 0;
        for (int i = 0; i < 8; ++i) {
            x = (x << 8) | id[static_cast<size_t>(i)];
        }
        return std::hash<uint64_t>{}(x);
    }
};

std::mutex gMu;
std::unordered_map<std::array<uint8_t, 16>, std::shared_ptr<TrafficCounters>, KeyHash> gTable;
std::string gGeneration;

std::string randomHex(size_t nbytes) {
    std::string raw(nbytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(nbytes)) != 1) {
        return {};
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(nbytes * 2);
    for (size_t i = 0; i < nbytes; ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        out[i * 2] = hex[c >> 4];
        out[i * 2 + 1] = hex[c & 0x0f];
    }
    return out;
}

} // namespace

std::shared_ptr<TrafficCounters> trafficFor(const std::array<uint8_t, 16>& id) {
    std::lock_guard<std::mutex> lock(gMu);
    auto& slot = gTable[id];
    if (!slot) {
        slot = std::make_shared<TrafficCounters>();
    }
    return slot;
}

std::string trafficGeneration() {
    std::lock_guard<std::mutex> lock(gMu);
    if (gGeneration.empty()) {
        gGeneration = randomHex(16);
        if (gGeneration.empty()) {
            gGeneration = "unknown";
        }
    }
    return gGeneration;
}

std::vector<TrafficRow> trafficSnapshot() {
    std::lock_guard<std::mutex> lock(gMu);
    std::vector<TrafficRow> rows;
    rows.reserve(gTable.size());
    for (const auto& kv : gTable) {
        TrafficRow row;
        row.uuid = proxy::vless::Validator::formatUuid(kv.first);
        row.up = kv.second->up.load(std::memory_order_relaxed);
        row.down = kv.second->down.load(std::memory_order_relaxed);
        row.conns = kv.second->conns.load(std::memory_order_relaxed);
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace server
} // namespace vless
