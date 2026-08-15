#include "server/event_loop.h"
#include "server/vless_connection.h"
#include "common/log.h"
#include "proxy/vless/validator.h"
#include "net/tls.h"
#include <cstdlib>

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
#include <cstring>

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

namespace {

void printUsage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [port] [loglevel] [workers]\n"
        "       " << prog << " --tls-port <port> [--cert <path> --key <path>]\n"
        "                       [--cert-dir <dir>] [--cert-days <days>]\n"
        "                       [port] [loglevel] [workers]\n"
        "\n"
        "Options:\n"
        "  --tls-port <port>  启用 TLS 端口（与明文端口并存；默认 443）\n"
        "  --cert <path>      证书文件（PEM）。与 --key 一起指定则用正式证书\n"
        "  --key <path>       私钥文件（PEM）\n"
        "  --cert-dir <dir>   自签证书落盘目录（默认 ./certs，容器内 /etc/vmess/certs）\n"
        "  --cert-days <days> 自签证书有效期天数（默认 365）\n"
        "  --log-file <path>  日志落盘文件（异步日志追加写入；默认仅 stderr）\n"
        "\n"
        "证书语义（见 doc/18-server-tls-support.md）：\n"
        "  --tls-port + --cert/--key     → 使用正式证书\n"
        "  --tls-port 无证书             → 自签证书保底（自动生成，落盘复用）\n"
        "  --tls-port + 证书加载失败     → 报错退出（fail-fast，不静默降级）\n"
        "\n"
        "位置参数：port 明文端口（默认 1080）、loglevel（debug/info/warn/error）、\n"
        "          workers 线程数（默认 CPU 核数）\n";
}

} // namespace

int main(int argc, char* argv[]) {
    uint16_t port = 1080;
    uint16_t tlsPort = 0;
    std::string logLevel = "info";
    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    vmess::net::TlsConfig tlsCfg;
    std::string logFile;
    std::vector<std::string> positional;

    // 解析参数：支持命名参数（--xxx value）与位置参数混排
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--tls-port" && i + 1 < argc) {
            tlsCfg.enabled = true;
            tlsPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--cert" && i + 1 < argc) {
            tlsCfg.certFile = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            tlsCfg.keyFile = argv[++i];
        } else if (arg == "--cert-dir" && i + 1 < argc) {
            tlsCfg.certDir = argv[++i];
        } else if (arg == "--cert-days" && i + 1 < argc) {
            tlsCfg.certDays = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--log-file" && i + 1 < argc) {
            logFile = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::cerr << "[Main] Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        } else {
            positional.push_back(arg);
        }
    }

    if (!positional.empty()) {
        port = static_cast<uint16_t>(std::atoi(positional[0].c_str()));
    }
    if (positional.size() > 1) {
        logLevel = positional[1];
    }
    if (positional.size() > 2) {
        workerCount = std::max(1, std::atoi(positional[2].c_str()));
    }

    // 证书语义校验：--cert/--key 必须成对
    if (tlsCfg.certFile.empty() != tlsCfg.keyFile.empty()) {
        std::cerr << "[Main] Error: --cert and --key must be specified together" << std::endl;
        return 1;
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

    // TLS 上下文创建（证书三级判定：正式证书 / 自签保底 / 失败退出）
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> tlsCtx(nullptr, SSL_CTX_free);
    std::string tlsWarn;
    if (tlsCfg.enabled) {
        SSL_CTX* ctx = vmess::net::createServerSslContext(tlsCfg, &tlsWarn);
        if (!ctx) {
            std::cerr << "[Main] Error: failed to create TLS context"
                      << (tlsCfg.certFile.empty() ? " (self-signed fallback failed)"
                                                  : " (bad cert/key)")
                      << std::endl;
            return 1;
        }
        tlsCtx.reset(ctx);
    }

    std::cerr << "=== VLESS Server ===" << std::endl;
    std::cerr << "Port: " << port << std::endl;
    if (tlsCfg.enabled) {
        std::cerr << "TLS Port: " << tlsPort << std::endl;
        std::cerr << "TLS Cert: "
                  << (tlsCfg.certFile.empty() ? "self-signed (fallback)" : tlsCfg.certFile)
                  << std::endl;
        if (!tlsWarn.empty()) {
            std::cerr << "TLS Warn: " << tlsWarn << std::endl;
        }
    } else {
        std::cerr << "TLS: disabled" << std::endl;
    }
    std::cerr << "LogLevel: " << logLevel << std::endl;
    std::cerr << "Workers: " << workerCount << std::endl;
    std::cerr << "Users: " << validator.size() << std::endl;
    std::cerr << "Protocol: VLESS"
              << (tlsCfg.enabled ? " + TLS (built-in)" : " (plaintext, no TLS)")
              << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        std::vector<std::unique_ptr<vmess::server::EventLoop>> loops;
        std::vector<vmess::server::EventLoop*> loopPtrs;

        auto makeFactory = [&validator](SSL_CTX* tls) {
            return [&validator, tls](int clientFd, vmess::net::IoUring& uring) {
                return std::make_unique<vmess::server::VlessConnection>(
                    clientFd, uring, validator, tls);
            };
        };

        // 明文端口 worker 组
        for (unsigned int i = 0; i < workerCount; ++i) {
            loops.push_back(std::make_unique<vmess::server::EventLoop>(
                makeFactory(nullptr)));
        }
        // TLS 端口 worker 组（共享同一 SSL_CTX，证书状态一致）
        if (tlsCfg.enabled) {
            for (unsigned int i = 0; i < workerCount; ++i) {
                loops.push_back(std::make_unique<vmess::server::EventLoop>(
                    makeFactory(tlsCtx.get())));
            }
        }

        loopPtrs.reserve(loops.size());
        for (auto& loop : loops) {
            loopPtrs.push_back(loop.get());
        }
        g_loops = &loopPtrs;

        std::atomic<bool> hasError{false};
        std::string errorMessage;
        std::mutex errorMu;
        std::vector<std::thread> workers;
        workers.reserve(loops.size());

        const bool enableReusePort = workerCount > 1;
        const size_t plainCount = workerCount;
        for (size_t i = 0; i < loops.size(); ++i) {
            workers.emplace_back([&, i]() {
                try {
                    uint16_t runPort = (i < plainCount) ? port : tlsPort;
                    loops[i]->run(runPort, enableReusePort);
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
