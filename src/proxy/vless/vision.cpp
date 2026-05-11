#include "proxy/vless/vision.h"
#include "common/log.h"

#include <algorithm>
#include <cstring>

namespace vmess {
namespace proxy {
namespace vless {

// TLS detection constants (from Xray proxy/proxy.go)
static constexpr uint8_t kTls13SupportedVersions[] = {0x00, 0x2b, 0x00, 0x02, 0x03, 0x04};
static constexpr uint8_t kTlsClientHandshakeStart[] = {0x16, 0x03};
static constexpr uint8_t kTlsServerHandshakeStart[] = {0x16, 0x03, 0x03};
static constexpr uint8_t kTlsApplicationDataStart[] = {0x17, 0x03, 0x03};
static constexpr uint8_t kTlsHandshakeTypeClientHello = 0x01;
static constexpr uint8_t kTlsHandshakeTypeServerHello = 0x02;

// ── VisionReader ────────────────────────────────────────────────────────

VisionReader::VisionReader(const std::array<uint8_t, 16>& uuid,
                           std::shared_ptr<VisionContext> ctx)
    : uuid_(uuid), ctx_(std::move(ctx)) {}

std::vector<uint8_t> VisionReader::process(const std::vector<uint8_t>& data) {
    if (data.empty()) return data;
    if (directCopy_) return data;

    if (!withinPadding_) {
        directCopy_ = true;
        return data;
    }

    std::vector<uint8_t> output;
    output.reserve(data.size());
    size_t pos = 0;

    // Initial state: look for UUID prefix to start unpadding
    if (remainingCommand_ == -1 && remainingContent_ == -1 && remainingPadding_ == -1) {
        if (data.size() >= VISION_FULL_HEADER_SIZE &&
            std::equal(data.begin(), data.begin() + 16, uuid_.begin())) {
            pos = 16;
            remainingCommand_ = 5;
        } else {
            // Not a Vision padding frame — shouldn't happen with proper clients
            LOG_WARN("VisionReader", "Data doesn't start with expected UUID, passing through");
            directCopy_ = true;
            return data;
        }
    }

    // Process data through the unpadding state machine
    while (pos < data.size()) {
        if (remainingCommand_ > 0) {
            // Reading command header (5 bytes: command + contentLen + paddingLen)
            while (remainingCommand_ > 0 && pos < data.size()) {
                uint8_t byte = data[pos++];
                switch (remainingCommand_) {
                    case 5: currentCommand_ = static_cast<int>(byte); break;
                    case 4: remainingContent_ = static_cast<int32_t>(byte) << 8; break;
                    case 3: remainingContent_ |= static_cast<int32_t>(byte); break;
                    case 2: remainingPadding_ = static_cast<int32_t>(byte) << 8; break;
                    case 1: remainingPadding_ |= static_cast<int32_t>(byte); break;
                }
                remainingCommand_--;
            }
        } else if (remainingContent_ > 0) {
            // Reading content → output
            size_t toRead = std::min(static_cast<size_t>(remainingContent_), data.size() - pos);
            output.insert(output.end(), data.begin() + pos, data.begin() + pos + toRead);
            pos += toRead;
            remainingContent_ -= static_cast<int32_t>(toRead);
        } else if (remainingPadding_ > 0) {
            // Skipping padding
            size_t toSkip = std::min(static_cast<size_t>(remainingPadding_), data.size() - pos);
            pos += toSkip;
            remainingPadding_ -= static_cast<int32_t>(toSkip);
        } else {
            // Frame complete — check command
            if (currentCommand_ == VISION_CMD_CONTINUE) {
                // More frames follow — read next command header
                remainingCommand_ = 5;
            } else {
                // END or DIRECT — stop unpadding
                withinPadding_ = false;
                if (currentCommand_ == VISION_CMD_DIRECT) {
                    directCopy_ = true;
                }
                LOG_DEBUG("VisionReader", "Unpadding done, command=",
                          currentCommand_, " directCopy=", directCopy_);
                // Append remaining data as-is (post-padding plaintext)
                if (pos < data.size()) {
                    output.insert(output.end(), data.begin() + pos, data.end());
                }
                break;
            }
        }
    }

    // TLS detection on unpadded content (updates shared context for VisionWriter)
    if (ctx_ && ctx_->packetsToFilter > 0 && !output.empty() && output.size() >= 6) {
        if (std::equal(kTlsClientHandshakeStart,
                       kTlsClientHandshakeStart + sizeof(kTlsClientHandshakeStart),
                       output.begin()) &&
            output[5] == kTlsHandshakeTypeClientHello) {
            ctx_->isTLS = true;
            LOG_DEBUG("VisionReader", "Detected TLS ClientHello in uplink");
        }
        ctx_->packetsToFilter--;
    }

    return output;
}

// ── VisionWriter ────────────────────────────────────────────────────────

VisionWriter::VisionWriter(const std::array<uint8_t, 16>& uuid,
                           std::shared_ptr<VisionContext> ctx)
    : uuid_(uuid), ctx_(std::move(ctx)) {}

std::vector<uint8_t> VisionWriter::process(const std::vector<uint8_t>& data) {
    if (data.empty()) return data;
    if (directCopy_) return data;

    // TLS detection on raw target data
    if (ctx_ && ctx_->packetsToFilter > 0) {
        filterTls(data.data(), data.size());
    }

    if (!isPadding_) {
        // Padding phase ended
        directCopy_ = true;
        return data;
    }

    // Decide padding command based on TLS state
    bool isComplete = isCompleteRecord(data.data(), data.size());
    bool longPadding = ctx_ && ctx_->isTLS;

    // TLS Application Data detected → end padding phase
    if (ctx_ && ctx_->isTLS && data.size() >= 6 &&
        std::equal(kTlsApplicationDataStart,
                   kTlsApplicationDataStart + sizeof(kTlsApplicationDataStart),
                   data.begin()) &&
        isComplete) {
        uint8_t command;
        if (ctx_->enableXtls) {
            command = VISION_CMD_DIRECT;
            directCopy_ = true;
        } else {
            command = VISION_CMD_END;
        }
        isPadding_ = false;
        LOG_DEBUG("VisionWriter", "TLS Application Data detected, command=",
                  static_cast<int>(command), " enableXtls=",
                  (ctx_ ? ctx_->enableXtls : false));
        return padding(data.data(), data.size(), command, true);
    }

    // Not TLS 1.2+ and few packets left → end padding early
    if (ctx_ && !ctx_->isTLS12orAbove && ctx_->packetsToFilter <= 1) {
        isPadding_ = false;
        return padding(data.data(), data.size(), VISION_CMD_END, longPadding);
    }

    // Continue padding
    return padding(data.data(), data.size(), VISION_CMD_CONTINUE, longPadding);
}

void VisionWriter::filterTls(const uint8_t* data, size_t len) {
    if (!ctx_) return;

    ctx_->packetsToFilter--;

    if (len >= 6) {
        // Detect TLS ServerHello
        if (std::equal(kTlsServerHandshakeStart,
                       kTlsServerHandshakeStart + sizeof(kTlsServerHandshakeStart),
                       data) &&
            data[5] == kTlsHandshakeTypeServerHello) {
            ctx_->remainingServerHello =
                (static_cast<int32_t>(data[3]) << 8 | static_cast<int32_t>(data[4])) + 5;
            ctx_->isTLS12orAbove = true;
            ctx_->isTLS = true;

            if (len >= 79 && ctx_->remainingServerHello >= 79) {
                int32_t sessionIdLen = static_cast<int32_t>(data[43]);
                if (43 + sessionIdLen + 3 <= static_cast<int32_t>(len)) {
                    ctx_->cipher =
                        (static_cast<uint16_t>(data[43 + sessionIdLen + 1]) << 8) |
                         static_cast<uint16_t>(data[43 + sessionIdLen + 2]);
                }
            }
            LOG_DEBUG("VisionWriter", "TLS ServerHello detected, remainingServerHello=",
                      ctx_->remainingServerHello);
        }
        // Detect TLS ClientHello
        else if (std::equal(kTlsClientHandshakeStart,
                            kTlsClientHandshakeStart + sizeof(kTlsClientHandshakeStart),
                            data) &&
                 data[5] == kTlsHandshakeTypeClientHello) {
            ctx_->isTLS = true;
            LOG_DEBUG("VisionWriter", "TLS ClientHello detected in downlink");
        }
    }

    // Scan for TLS 1.3 supported_versions extension in ServerHello
    if (ctx_->remainingServerHello > 0) {
        size_t end = std::min(static_cast<size_t>(ctx_->remainingServerHello), len);
        ctx_->remainingServerHello -= static_cast<int32_t>(len);

        for (size_t i = 0; i + sizeof(kTls13SupportedVersions) <= end; i++) {
            if (std::equal(kTls13SupportedVersions,
                           kTls13SupportedVersions + sizeof(kTls13SupportedVersions),
                           data + i)) {
                // TLS 1.3 detected — check cipher suite
                // TLS_AES_128_CCM_8_SHA256 (0x1305) is excluded
                if (ctx_->cipher != 0x1305) {
                    ctx_->enableXtls = true;
                }
                ctx_->packetsToFilter = 0;
                LOG_DEBUG("VisionWriter", "TLS 1.3 detected, cipher=0x",
                          std::hex, ctx_->cipher, std::dec,
                          " enableXtls=", ctx_->enableXtls);
                return;
            }
        }

        if (ctx_->remainingServerHello <= 0) {
            // Scanned entire ServerHello, no TLS 1.3 → assume TLS 1.2
            ctx_->packetsToFilter = 0;
            LOG_DEBUG("VisionWriter", "TLS 1.2 or older (no TLS 1.3 in ServerHello)");
            return;
        }
    }

    if (ctx_->packetsToFilter <= 0) {
        LOG_DEBUG("VisionWriter", "TLS filtering done, isTLS=", ctx_->isTLS,
                  " isTLS12orAbove=", ctx_->isTLS12orAbove,
                  " enableXtls=", ctx_->enableXtls);
    }
}

std::vector<uint8_t> VisionWriter::padding(const uint8_t* data, size_t len,
                                            uint8_t command, bool longPadding) {
    int32_t contentLen = static_cast<int32_t>(len);
    int32_t paddingLen = 0;

    // Calculate padding length (matching Xray's XtlsPadding logic)
    if (contentLen < static_cast<int32_t>(testseed_[0]) && longPadding) {
        std::uniform_int_distribution<int32_t> dist(0, static_cast<int32_t>(testseed_[1]));
        paddingLen = dist(rng_) + static_cast<int32_t>(testseed_[2]) - contentLen;
    } else {
        std::uniform_int_distribution<int32_t> dist(0, static_cast<int32_t>(testseed_[3]));
        paddingLen = dist(rng_);
    }

    // Clamp padding to fit max frame size
    size_t headerSize = uuidWritten_ ? VISION_SHORT_HEADER_SIZE : VISION_FULL_HEADER_SIZE;
    int32_t maxPadding = static_cast<int32_t>(VISION_MAX_FRAME_SIZE - headerSize - contentLen);
    if (paddingLen > maxPadding) {
        paddingLen = std::max(maxPadding, 0);
    }

    // Build frame: [UUID?] [cmd] [contentLen] [paddingLen] [content] [random padding]
    size_t totalSize = headerSize + contentLen + paddingLen;
    std::vector<uint8_t> frame(totalSize);
    size_t offset = 0;

    // Write UUID (first frame only)
    if (!uuidWritten_) {
        std::copy(uuid_.begin(), uuid_.end(), frame.begin() + offset);
        offset += 16;
        uuidWritten_ = true;
    }

    // Write command + contentLen (big-endian) + paddingLen (big-endian)
    frame[offset++] = command;
    frame[offset++] = static_cast<uint8_t>(contentLen >> 8);
    frame[offset++] = static_cast<uint8_t>(contentLen);
    frame[offset++] = static_cast<uint8_t>(paddingLen >> 8);
    frame[offset++] = static_cast<uint8_t>(paddingLen);

    // Write content
    if (data && len > 0) {
        std::copy(data, data + len, frame.begin() + offset);
        offset += len;
    }

    // Write random padding bytes
    if (paddingLen > 0) {
        std::uniform_int_distribution<uint8_t> byteDist(0, 255);
        for (int32_t i = 0; i < paddingLen; i++) {
            frame[offset++] = byteDist(rng_);
        }
    }

    LOG_DEBUG("VisionWriter", "Padding frame: contentLen=", contentLen,
              " paddingLen=", paddingLen, " command=", static_cast<int>(command));

    return frame;
}

bool VisionWriter::isCompleteRecord(const uint8_t* data, size_t len) {
    // Check if data consists of complete TLS records
    // TLS record: [type(1)] [version(2)] [length(2)] [payload(length)]
    size_t headerLen = 5;
    size_t recordLen = 0;
    size_t i = 0;

    while (i < len) {
        if (headerLen > 0) {
            uint8_t byte = data[i++];
            switch (headerLen) {
                case 5:
                    if (byte != 0x17) return false;  // Application Data type
                    break;
                case 4:
                    if (byte != 0x03) return false;
                    break;
                case 3:
                    if (byte != 0x03) return false;
                    break;
                case 2:
                    recordLen = static_cast<size_t>(byte) << 8;
                    break;
                case 1:
                    recordLen |= byte;
                    break;
            }
            headerLen--;
        } else if (recordLen > 0) {
            size_t remaining = len - i;
            if (remaining < recordLen) {
                return false;
            }
            i += recordLen;
            recordLen = 0;
            headerLen = 5;
        } else {
            return false;
        }
    }

    return headerLen == 5 && recordLen == 0;
}

} // namespace vless
} // namespace proxy
} // namespace vmess
