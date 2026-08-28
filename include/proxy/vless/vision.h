#ifndef VLESS_PROXY_VLESS_VISION_H
#define VLESS_PROXY_VLESS_VISION_H

#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <random>

namespace vless {
namespace proxy {
namespace vless {

// Vision padding commands (from Xray proxy/proxy.go)
constexpr uint8_t VISION_CMD_CONTINUE = 0x00;
constexpr uint8_t VISION_CMD_END      = 0x01;
constexpr uint8_t VISION_CMD_DIRECT   = 0x02;

// Frame size limits
constexpr size_t VISION_MAX_FRAME_SIZE = 8192;
constexpr size_t VISION_FULL_HEADER_SIZE = 21;   // UUID(16) + cmd(1) + contentLen(2) + paddingLen(2)
constexpr size_t VISION_SHORT_HEADER_SIZE = 5;    // cmd(1) + contentLen(2) + paddingLen(2)

// Known flow values
constexpr const char* VLESS_FLOW_VISION = "xtls-rprx-vision";

/**
 * Shared TLS detection state between VisionReader and VisionWriter.
 * Both directions detect TLS characteristics to coordinate padding.
 * Single-threaded access (io_uring coroutines yield at co_await points).
 */
struct VisionContext {
    int packetsToFilter = 8;
    bool isTLS = false;
    bool isTLS12orAbove = false;
    bool enableXtls = false;
    uint16_t cipher = 0;
    int32_t remainingServerHello = -1;
};

/**
 * Vision unpadding for uplink (client → target).
 *
 * Removes Vision padding frames and extracts content data.
 * Padding frame format:
 *   [userUUID (16B, first frame)] [command (1B)] [contentLen (2B)]
 *   [paddingLen (2B)] [content] [padding]
 *
 * After CommandPaddingDirect, data passes through unchanged.
 * Handles partial frames across multiple process() calls.
 */
class VisionReader {
public:
    VisionReader(const std::array<uint8_t, 16>& uuid,
                 std::shared_ptr<VisionContext> ctx);

    /**
     * Process data from client, removing Vision padding.
     * Returns unpadded content. After directCopy(), returns input unchanged.
     */
    std::vector<uint8_t> process(const std::vector<uint8_t>& data);

    bool directCopy() const { return directCopy_; }

private:
    std::array<uint8_t, 16> uuid_;
    std::shared_ptr<VisionContext> ctx_;

    bool withinPadding_ = true;
    bool directCopy_ = false;
    int32_t remainingCommand_ = -1;   // -1 = initial, waiting for UUID
    int32_t remainingContent_ = -1;
    int32_t remainingPadding_ = -1;
    int currentCommand_ = 0;
};

/**
 * Vision padding for downlink (target → client).
 *
 * Adds Vision padding frames around content data.
 * Detects TLS handshake characteristics to decide when to stop padding:
 *   - TLS 1.3 Application Data → CommandPaddingDirect, then direct copy
 *   - Non-TLS or TLS 1.2 → CommandPaddingEnd after a few packets
 */
class VisionWriter {
public:
    VisionWriter(const std::array<uint8_t, 16>& uuid,
                 std::shared_ptr<VisionContext> ctx);

    /**
     * Process data from target, adding Vision padding.
     * Returns padded data for client. After directCopy(), returns input unchanged.
     */
    std::vector<uint8_t> process(const std::vector<uint8_t>& data);

    bool directCopy() const { return directCopy_; }

private:
    std::array<uint8_t, 16> uuid_;
    std::shared_ptr<VisionContext> ctx_;

    bool uuidWritten_ = false;
    bool isPadding_ = true;
    bool directCopy_ = false;

    std::mt19937 rng_{std::random_device{}()};

    // Padding seed parameters (default from Xray testseed)
    uint32_t testseed_[4] = {900, 500, 900, 256};

    void filterTls(const uint8_t* data, size_t len);
    std::vector<uint8_t> padding(const uint8_t* data, size_t len,
                                  uint8_t command, bool longPadding);
    static bool isCompleteRecord(const uint8_t* data, size_t len);
};

} // namespace vless
} // namespace proxy
} // namespace vless

#endif // VLESS_PROXY_VLESS_VISION_H
