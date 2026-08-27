#include "common/link.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

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

// ── 公网 IP 探测（纯 POSIX socket，不依赖外部命令）─────────────────────────

/// 公网 IP 回显服务（按顺序尝试；纯 HTTP，无需 TLS 握手）
const char* kIpEchoHosts[] = {
    "api.ipify.org",
    "icanhazip.com",
    "ifconfig.me",
};

bool isValidIp(const std::string& s) {
    struct in_addr a4 {};
    struct in6_addr a6 {};
    return inet_pton(AF_INET, s.c_str(), &a4) == 1 ||
           inet_pton(AF_INET6, s.c_str(), &a6) == 1;
}

/// 非阻塞 connect + poll 等待，返回 0 成功 / -1 失败
int connectWithTimeout(int fd, const sockaddr* addr, socklen_t len, int timeoutMs) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, addr, len);
    if (rc == 0) {
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }

    pollfd pfd{fd, POLLOUT, 0};
    const int pr = poll(&pfd, 1, timeoutMs);
    if (pr <= 0) {
        return -1;
    }
    int soErr = 0;
    socklen_t elen = sizeof(soErr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &elen);
    return soErr == 0 ? 0 : -1;
}

/// 解析回显响应 body（跳过头，去首尾空白，校验合法 IP）
std::string parseEchoBody(std::string body) {
    const size_t sep = body.find("\r\n\r\n");
    std::string payload = (sep == std::string::npos) ? body : body.substr(sep + 4);
    const size_t b = payload.find_first_not_of(" \t\r\n");
    const size_t e = payload.find_last_not_of(" \t\r\n");
    if (b != std::string::npos) {
        payload = payload.substr(b, e - b + 1);
    } else {
        payload.clear();
    }
    return isValidIp(payload) ? payload : std::string();
}

/// 系统代理环境 fallback：curl（或 wget）带代理设置请求，成功返回 IP，否则空串
std::string queryViaCurl(const char* host, int timeoutMs) {
    const int sec = (timeoutMs > 0) ? ((timeoutMs + 999) / 1000) : 3;
    std::string cmd =
        "curl -s --connect-timeout " + std::to_string(sec) + " --max-time " +
        std::to_string(sec + 2) + " http://" + host + "/ 2>/dev/null";
    if (std::system("command -v curl >/dev/null 2>&1") != 0) {
        return {};
    }
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        return {};
    }
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), p) != nullptr) {
        out += buf;
    }
    pclose(p);
    return parseEchoBody(std::move(out));
}

/// 对一个回显服务发 GET 并解析响应 body（成功返回合法 IP，否则空串）
std::string queryEchoHost(const char* host, int timeoutMs) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, "80", &hints, &res) != 0) {
        return {};
    }

    std::string ip;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connectWithTimeout(fd, ai->ai_addr, ai->ai_addrlen, timeoutMs) != 0) {
            close(fd);
            continue;
        }

        // 收发超时兜底
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const std::string req =
            std::string("GET / HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n";
        if (send(fd, req.data(), req.size(), 0) < 0) {
            close(fd);
            continue;
        }

        std::string body;
        char buf[512];
        for (;;) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            body.append(buf, static_cast<size_t>(n));
        }
        close(fd);

        const std::string parsed = parseEchoBody(std::move(body));
        if (!parsed.empty()) {
            ip = parsed;
            break;
        }
    }

    freeaddrinfo(res);
    return ip;
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
    if (security == "tls") {
        // SNI：host 为域名时显式指定（IP 场景客户端默认以 address 兜底，无需输出）
        if (!isValidIp(host)) {
            link += "&sni=" + urlEncode(host);
        }
        link += "&fp=chrome";
        // 自签证书（未配置正式证书）：固定证书指纹替代已移除的 allowInsecure。
        // Xray 26.2.6+ 已移除 allowInsecure，必须用 pinnedPeerCertSha256（v2rayN 9460）。
        // 格式：证书 DER 的 SHA-256 hex（Xray infra/conf 用 hex.DecodeString 解析，
        // base64 会报 `encoding/hex: invalid byte`；hex 只含 0-9a-f，无需 URL 编码）。
        // 字段：pcs 为 v2rayN 26.x 字段；pinSHA256 兼容旧版客户端（Shadowrocket 等）。
        if (cfg.tls.certFile.empty()) {
            if (!cfg.tls.certSha256.empty()) {
                link += "&pcs=" + cfg.tls.certSha256;
                link += "&pinSHA256=" + cfg.tls.certSha256;
            } else {
                link += "&allowInsecure=1";  // 指纹不可用（异常场景）时回退旧行为
            }
        }
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

std::string detectPublicIp(int timeoutMs) {
    // 1) 纯 socket（云服务器/直连出网场景，无需外部命令）
    for (const char* host : kIpEchoHosts) {
        const std::string ip = queryEchoHost(host, timeoutMs);
        if (!ip.empty()) {
            return ip;
        }
    }
    // 2) fallback：curl（有系统代理/受限网络环境，如公司内网）
    for (const char* host : kIpEchoHosts) {
        const std::string ip = queryViaCurl(host, timeoutMs);
        if (!ip.empty()) {
            return ip;
        }
    }
    return {};
}

} // namespace common
} // namespace vmess
