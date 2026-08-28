#ifndef VLESS_PROXY_VLESS_VALIDATOR_H
#define VLESS_PROXY_VLESS_VALIDATOR_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vless {
namespace proxy {
namespace vless {

class Validator {
public:
    Validator() = default;

    bool addFromString(const std::string& uuid);
    void add(const std::array<uint8_t, 16>& uuid);
    bool contains(const std::array<uint8_t, 16>& uuid) const;
    size_t size() const { return users_.size(); }

    static bool parseUuid(const std::string& uuid, std::array<uint8_t, 16>& out);

private:
    std::vector<std::array<uint8_t, 16>> users_;
};

} // namespace vless
} // namespace proxy
} // namespace vless

#endif // VLESS_PROXY_VLESS_VALIDATOR_H
