#include "server/event_loop.h"
#include "client/socks5_connection.h"
#include "client/vless_client.h"
#include "common/log.h"
#include "proxy/vless/validator.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::vector<vmess::server::EventLoop*>* g_loops = nullptr;

void signalHandler(int sig) {
    std::cerr << "\n[ClientMain] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_loops) {
        for (auto* loop : *g_loops) {
            if (loop) {
                loop->stop();
            }
        }
    }
}

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --socks5-port <port>   本地 SOCKS5 监听端口 (default: 1080)\n"
              << "  --remote <host:port>   远端 VLESS 服务器地址 (default: 127.0.0.1:443)\n"
              << "  --uuid <uuid>          VLESS 用户 UUID (default: e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b)\n"
              << "  --log <level>          日志级别: none|error|warn|info|debug (default: info)\n"
              << "  --log-file <path>      日志落盘文件（异步日志追加写入；默认仅 stderr）\n"
              << "  --workers <n>          worker 数 (default: CPU 核数)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    uint16_t socks5Port = 1080;
    std::string remoteAddr = "127.0.0.1:443";
    std::string uuidStr = "e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b";
    std::string logLevel = "info";
    std::string logFile;
    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string();
        };
        if (arg == "--socks5-port") {
            socks5Port = static_cast<uint16_t>(std::atoi(next().c_str()));
        } else if (arg == "--remote") {
            remoteAddr = next();
        } else if (arg == "--uuid") {
            uuidStr = next();
        } else if (arg == "--log") {
            logLevel = next();
        } else if (arg == "--log-file") {
            logFile = next();
        } else if (arg == "--workers") {
            workerCount = std::max(1, std::atoi(next().c_str()));
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "[ClientMain] Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    // 日志落盘：优先 --log-file，其次 VLESS_LOG_FILE 环境变量
    if (logFile.empty()) {
        const char* env = std::getenv("VLESS_LOG_FILE");
        if (env && env[0] != '\0') {
            logFile = env;
        }
    }
    vmess::common::Logger::instance().setLogFile(logFile);

    vmess::common::setLogLevel(vmess::common::parseLogLevel(logLevel));

    // 解析 UUID
    std::array<uint8_t, 16> uuid{};
    if (!vmess::proxy::vless::Validator::parseUuid(uuidStr, uuid)) {
        std::cerr << "[ClientMain] Invalid uuid: " << uuidStr << std::endl;
        return 1;
    }

    // 解析远端地址
    vmess::client::VlessClientConfig cfg = vmess::client::VlessClientConfig::fromString(remoteAddr);
    cfg.uuid = uuid;

    std::cerr << "=== VLESS Client (SOCKS5) ===" << std::endl;
    std::cerr << "Socks5 Listen: 127.0.0.1:" << socks5Port << std::endl;
    std::cerr << "Remote: " << cfg.remoteHost << ":" << cfg.remotePort << std::endl;
    std::cerr << "Uuid: " << uuidStr << std::endl;
    std::cerr << "LogLevel: " << logLevel << std::endl;
    std::cerr << "Workers: " << workerCount << std::endl;
    std::cerr << "Protocol: VLESS (plaintext, no TLS)" << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        std::vector<std::unique_ptr<vmess::server::EventLoop>> loops;
        loops.reserve(workerCount);
        for (unsigned int i = 0; i < workerCount; ++i) {
            // 工厂模式：每个 accept 的 fd 创建一个 SOCKS5 连接
            loops.push_back(std::make_unique<vmess::server::EventLoop>(
                [cfg](int clientFd, vmess::net::IoUring& uring) {
                    return std::make_unique<vmess::client::Socks5Connection>(
                        clientFd, uring, cfg);
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
                    loops[i]->run(socks5Port, enableReusePort);
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
        std::cerr << "[ClientMain] Error: " << e.what() << std::endl;
        return 1;
    }

    std::cerr << "[ClientMain] Client stopped" << std::endl;
    return 0;
}
