#include "server/event_loop.h"
#include "server/vless_connection.h"
#include "common/log.h"
#include "proxy/vless/validator.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <cctype>

static std::vector<vmess::server::EventLoop*>* g_loops = nullptr;

void signalHandler(int sig) {
    std::cerr << "\n[Main] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_loops) {
        for (auto* loop : *g_loops) {
            if (loop) {
                loop->stop();
            }
        }
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 1080;
    std::string logLevel = "info";
    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        logLevel = argv[2];
    }
    if (argc > 3) {
        workerCount = std::max(1, std::atoi(argv[3]));
    }

    vmess::common::setLogLevel(vmess::common::parseLogLevel(logLevel));

    vmess::proxy::vless::Validator validator;
    bool hasConfiguredUser = false;
    const char* usersEnv = std::getenv("VLESS_USERS");
    if (usersEnv && usersEnv[0] != '\0') {
        std::stringstream ss(usersEnv);
        std::string token;
        while (std::getline(ss, token, ',')) {
            token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), token.end());
            if (!token.empty() && validator.addFromString(token)) {
                hasConfiguredUser = true;
            }
        }
    }
    if (!hasConfiguredUser) {
        // 保持旧行为：未配置时使用默认用户。
        validator.addFromString("e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b");
    }

    std::cerr << "=== VLESS Server ===" << std::endl;
    std::cerr << "Port: " << port << std::endl;
    std::cerr << "LogLevel: " << logLevel << std::endl;
    std::cerr << "Workers: " << workerCount << std::endl;
    std::cerr << "Users: " << validator.size() << std::endl;
    std::cerr << "Protocol: VLESS (plaintext, no TLS)" << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        std::vector<std::unique_ptr<vmess::server::EventLoop>> loops;
        loops.reserve(workerCount);
        for (unsigned int i = 0; i < workerCount; ++i) {
            // 工厂模式：每个 accept 的 fd 创建一个 VLESS 连接
            loops.push_back(std::make_unique<vmess::server::EventLoop>(
                [&validator](int clientFd, vmess::net::IoUring& uring) {
                    return std::make_unique<vmess::server::VlessConnection>(
                        clientFd, uring, validator);
                }));
        }

        std::vector<vmess::server::EventLoop*> loopPtrs;
        loopPtrs.reserve(workerCount);
        for (auto& loop : loops) {
            loopPtrs.push_back(loop.get());
        }
        g_loops = &loopPtrs;

        std::atomic<bool> hasError{false};
        std::string errorMessage;
        std::mutex errorMu;
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        const bool enableReusePort = workerCount > 1;
        for (unsigned int i = 0; i < workerCount; ++i) {
            workers.emplace_back([&, i]() {
                try {
                    loops[i]->run(port, enableReusePort);
                } catch (const std::exception& e) {
                    {
                        std::lock_guard<std::mutex> lock(errorMu);
                        if (!hasError.load()) {
                            errorMessage = e.what();
                        }
                    }
                    hasError = true;
                    for (auto* loop : loopPtrs) {
                        if (loop) {
                            loop->stop();
                        }
                    }
                }
            });
        }

        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }

        if (hasError) {
            throw std::runtime_error(errorMessage.empty() ? "worker thread failed" : errorMessage);
        }
    } catch (const std::exception& e) {
        std::cerr << "[Main] Error: " << e.what() << std::endl;
        return 1;
    }

    std::cerr << "[Main] Server stopped" << std::endl;
    return 0;
}
