#include "common/link.h"

#include <cstdio>
#include <cctype>

// 二维码生成库（nayuki/QR-Code-generator，MIT，单头文件，vendor 于 third_party）
#include "qrcodegen.hpp"

namespace vmess {
namespace common {

namespace {

/// URL 编码（RFC 3986 保留字符 + 非 ASCII 均百分号编码）
std::string urlEncode(const std::string& in) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

/// 每个用户的链接端口 / security（明文或 TLS 选一，TLS 优先展示）
bool linkTarget(const ServerConfig& cfg, uint16_t& port, std::string& security) {
    if (cfg.tls.enabled) {
        port = cfg.tls.port;
        security = "tls";
        return true;
    }
    port = cfg.port;
    security = "none";
    return true;
}

} // namespace

std::string buildVlessLink(const std::string& host,
                           const ServerConfig& cfg,
                           const UserConfig& user,
                           const std::string& remark) {
    if (host.empty() || user.uuid.empty()) {
        return {};
    }

    uint16_t port = 0;
    std::string security;
    linkTarget(cfg, port, security);

    char portBuf[8];
    std::snprintf(portBuf, sizeof(portBuf), "%u", static_cast<unsigned>(port));

    std::string link = "vless://" + user.uuid + "@" + host + ":" + portBuf +
                       "?encryption=none&security=" + security +
                       "&type=tcp&headerType=none";
    // 自签证书（未配置正式证书）时提示客户端跳过证书校验
    if (security == "tls" && cfg.tls.certFile.empty()) {
        link += "&allowInsecure=1";
    }
    if (!remark.empty()) {
        link += "#" + urlEncode(remark);
    }
    return link;
}

std::string renderQrText(const std::string& text, int border) {
    if (text.empty()) {
        return {};
    }
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);

    auto isDark = [&](int x, int y) {
        const int px = x - border;
        const int py = y - border;
        return px >= 0 && px < qr.getSize() && py >= 0 && py < qr.getSize() &&
               qr.getModule(px, py);
    };

    const int size = qr.getSize() + border * 2;
    std::string out;
    out.reserve(static_cast<size_t>(size) * ((size + 1) / 2 + 1) * 2);

    // 每两行模块合成一行半块字符（▀▄█ 空格），黑模块用深色字符，
    // 适配深色背景终端（与 qrencode -t UTF8 一致）
    for (int y = 0; y < size; y += 2) {
        for (int x = 0; x < size; ++x) {
            const bool top = isDark(x, y);
            const bool bot = isDark(x, y + 1);
            if (top && bot) {
                out += ' ';
            } else if (top && !bot) {
                out += "\xe2\x96\x80";  // ▀
            } else if (!top && bot) {
                out += "\xe2\x96\x84";  // ▄
            } else {
                out += "\xe2\x96\x88";  // █
            }
        }
        out += '\n';
    }
    return out;
}

std::string buildVlessQrText(const std::string& host,
                             const ServerConfig& cfg,
                             const UserConfig& user,
                             const std::string& remark) {
    const std::string link = buildVlessLink(host, cfg, user, remark);
    return renderQrText(link);
}

} // namespace common
} // namespace vmess
