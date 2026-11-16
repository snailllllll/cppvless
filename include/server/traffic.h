#ifndef VLESS_SERVER_TRAFFIC_H
#define VLESS_SERVER_TRAFFIC_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vless {
namespace server {

/// 一把 UUID 的进程内计数。连接在握手成功后拿住 shared_ptr，转发路径不再加锁。
struct TrafficCounters {
    std::atomic<uint64_t> up{0};
    std::atomic<uint64_t> down{0};
    std::atomic<int64_t> conns{0};
};

std::shared_ptr<TrafficCounters> trafficFor(const std::array<uint8_t, 16>& id);

/// 本进程启动时生成的代际。容器重建会变，宿主机 boot_id 不会。
std::string trafficGeneration();

struct TrafficRow {
    std::string uuid;
    uint64_t up = 0;
    uint64_t down = 0;
    int64_t conns = 0;
};

std::vector<TrafficRow> trafficSnapshot();

} // namespace server
} // namespace vless

#endif
