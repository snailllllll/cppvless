#include "server/admin.h"
#include "server/traffic.h"
#include "common/link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace vless {
namespace server {

namespace {

void respond(int fd, int code, const std::string& body) {
    const char* reason = "OK";
    if (code == 201) reason = "Created";
    else if (code == 400) reason = "Bad Request";
    else if (code == 404) reason = "Not Found";
    else if (code == 409) reason = "Conflict";
    else if (code == 413) reason = "Payload Too Large";
    else if (code == 500) reason = "Internal Server Error";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << reason
        << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\n\r\n" << body;
    const std::string bytes = oss.str();
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        off += static_cast<size_t>(n);
    }
}

bool jsonStringField(const std::string& body, const std::string& key, std::string& out) {
    const std::string token = "\"" + key + "\"";
    size_t p = body.find(token);
    if (p == std::string::npos) {
        return false;
    }
    p = body.find(':', p + token.size());
    if (p == std::string::npos) {
        return false;
    }
    p = body.find('"', p + 1);
    if (p == std::string::npos) {
        return false;
    }
    ++p;
    out.clear();
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) {
            out.push_back(body[p + 1]);
            p += 2;
            continue;
        }
        out.push_back(body[p]);
        ++p;
    }
    return p < body.size() && body[p] == '"';
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool readRequest(int fd, std::string& method, std::string& path, std::string& body) {
    timeval tv{};
    tv.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    while (buf.find("\r\n\r\n") == std::string::npos) {
        char tmp[1024];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(tmp, tmp + n);
        if (buf.size() > 8192) {
            respond(fd, 413, "{\"error\":\"headers too large\"}");
            return false;
        }
    }
    const size_t headerEnd = buf.find("\r\n\r\n");
    const std::string headers = buf.substr(0, headerEnd);
    body = buf.substr(headerEnd + 4);

    std::istringstream first(headers);
    std::string version;
    if (!(first >> method >> path >> version)) {
        respond(fd, 400, "{\"error\":\"bad request\"}");
        return false;
    }

    size_t contentLength = 0;
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string prefix = "content-length:";
        if (line.size() >= prefix.size()) {
            std::string lower = line;
            for (char& c : lower) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (lower.compare(0, prefix.size(), prefix) == 0) {
                contentLength = static_cast<size_t>(std::strtoul(line.c_str() + prefix.size(), nullptr, 10));
            }
        }
    }
    if (contentLength > 65536) {
        respond(fd, 413, "{\"error\":\"body too large\"}");
        return false;
    }
    while (body.size() < contentLength) {
        char tmp[1024];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;
        }
        body.append(tmp, tmp + n);
    }
    if (body.size() > contentLength) {
        body.resize(contentLength);
    }
    return true;
}

} // namespace

AdminServer::AdminServer(common::ServerConfig& cfg, proxy::vless::Validator& users,
                         std::string configPath)
    : cfg_(cfg), users_(users), configPath_(std::move(configPath)) {}

AdminServer::~AdminServer() {
    stop();
}

bool AdminServer::start(uint16_t port, std::string& error) {
    if (port == 0) {
        return true;
    }
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error = "admin socket failed";
        return false;
    }
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = std::string("admin bind 127.0.0.1:") + std::to_string(port) + " failed: " +
                std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 16) < 0) {
        error = "admin listen failed";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    stop_.store(false);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void AdminServer::stop() {
    stop_.store(true);
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void AdminServer::loop() {
    while (!stop_.load()) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (stop_.load() || errno == EBADF) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        handle(fd);
        ::close(fd);
    }
}

void AdminServer::handle(int fd) {
    std::string method;
    std::string path;
    std::string body;
    if (!readRequest(fd, method, path, body)) {
        return;
    }

    if (method == "GET" && path == "/v1/stats") {
        std::ostringstream oss;
        oss << "{\"generation\":\"" << jsonEscape(trafficGeneration()) << "\",\"users\":[";
        const auto rows = trafficSnapshot();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i > 0) {
                oss << ',';
            }
            oss << "{\"uuid\":\"" << jsonEscape(rows[i].uuid) << "\",\"up\":" << rows[i].up
                << ",\"down\":" << rows[i].down << ",\"conns\":" << rows[i].conns << '}';
        }
        oss << "]}";
        respond(fd, 200, oss.str());
        return;
    }

    auto userPath = [&](const std::string& suffix) -> std::string {
        const std::string prefix = "/v1/users/";
        if (path.compare(0, prefix.size(), prefix) != 0) {
            return {};
        }
        std::string rest = path.substr(prefix.size());
        if (!suffix.empty()) {
            if (rest.size() <= suffix.size() || rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) {
                return {};
            }
            rest.resize(rest.size() - suffix.size());
        }
        return rest;
    };

    if (method == "GET") {
        const std::string uuid = userPath("/link");
        if (!uuid.empty()) {
            std::array<uint8_t, 16> parsed{};
            if (!proxy::vless::Validator::parseUuid(uuid, parsed)) {
                respond(fd, 400, "{\"error\":\"invalid uuid\"}");
                return;
            }
            std::lock_guard<std::mutex> lock(cfgMu_);
            const common::UserConfig* found = nullptr;
            for (const auto& u : cfg_.users) {
                std::array<uint8_t, 16> have{};
                if (proxy::vless::Validator::parseUuid(u.uuid, have) && have == parsed) {
                    found = &u;
                    break;
                }
            }
            if (found == nullptr) {
                respond(fd, 404, "{\"error\":\"unknown uuid\"}");
                return;
            }
            const std::string link = common::buildVlessLink(cfg_.host, cfg_, *found, cfg_.remark);
            if (link.empty()) {
                respond(fd, 409, "{\"error\":\"host unset\"}");
                return;
            }
            respond(fd, 200, std::string("{\"link\":\"") + jsonEscape(link) + "\"}");
            return;
        }
    }

    if (method == "POST" && path == "/v1/users") {
        std::string uuid;
        std::string name;
        if (!jsonStringField(body, "uuid", uuid)) {
            respond(fd, 400, "{\"error\":\"uuid required\"}");
            return;
        }
        jsonStringField(body, "name", name);
        std::array<uint8_t, 16> parsed{};
        if (!proxy::vless::Validator::parseUuid(uuid, parsed)) {
            respond(fd, 400, "{\"error\":\"invalid uuid\"}");
            return;
        }
        std::lock_guard<std::mutex> lock(cfgMu_);
        if (users_.contains(parsed)) {
            respond(fd, 409, "{\"error\":\"exists\"}");
            return;
        }
        common::UserConfig uc;
        uc.uuid = proxy::vless::Validator::formatUuid(parsed);
        uc.name = name;
        cfg_.users.push_back(uc);
        users_.add(parsed);
        std::string writeErr;
        if (!common::writeServerConfig(configPath_, cfg_, writeErr)) {
            cfg_.users.pop_back();
            users_.remove(parsed);
            respond(fd, 500, std::string("{\"error\":\"") + jsonEscape(writeErr) + "\"}");
            return;
        }
        trafficFor(parsed);
        respond(fd, 201, std::string("{\"uuid\":\"") + uc.uuid + "\"}");
        return;
    }

    if (method == "DELETE") {
        const std::string uuid = userPath("");
        std::array<uint8_t, 16> parsed{};
        if (uuid.empty() || !proxy::vless::Validator::parseUuid(uuid, parsed)) {
            respond(fd, 400, "{\"error\":\"invalid uuid\"}");
            return;
        }
        std::lock_guard<std::mutex> lock(cfgMu_);
        auto it = cfg_.users.end();
        for (auto i = cfg_.users.begin(); i != cfg_.users.end(); ++i) {
            std::array<uint8_t, 16> have{};
            if (proxy::vless::Validator::parseUuid(i->uuid, have) && have == parsed) {
                it = i;
                break;
            }
        }
        if (it == cfg_.users.end()) {
            respond(fd, 404, "{\"error\":\"unknown uuid\"}");
            return;
        }
        common::UserConfig saved = *it;
        cfg_.users.erase(it);
        users_.remove(parsed);
        std::string writeErr;
        if (!common::writeServerConfig(configPath_, cfg_, writeErr)) {
            cfg_.users.push_back(saved);
            users_.add(parsed);
            respond(fd, 500, std::string("{\"error\":\"") + jsonEscape(writeErr) + "\"}");
            return;
        }
        respond(fd, 200, "{\"ok\":true}");
        return;
    }

    respond(fd, 404, "{\"error\":\"not found\"}");
}

} // namespace server
} // namespace vless
