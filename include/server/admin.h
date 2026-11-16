#ifndef VLESS_SERVER_ADMIN_H
#define VLESS_SERVER_ADMIN_H

#include "common/config.h"
#include "proxy/vless/validator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace vless {
namespace server {

/// 回环上的 HTTP/1.1 管理面。独立线程，不进 io_uring。
class AdminServer {
public:
    AdminServer(common::ServerConfig& cfg, proxy::vless::Validator& users, std::string configPath);
    ~AdminServer();

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    /// port 为 0 时不监听，返回 true。绑定失败写 error 并返回 false。
    bool start(uint16_t port, std::string& error);
    void stop();

private:
    void loop();
    void handle(int fd);

    common::ServerConfig& cfg_;
    proxy::vless::Validator& users_;
    std::string configPath_;
    std::mutex cfgMu_;
    std::atomic<bool> stop_{false};
    int listenFd_ = -1;
    std::thread thread_;
};

} // namespace server
} // namespace vless

#endif
