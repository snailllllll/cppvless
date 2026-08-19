#include "server/event_loop.h"
#include "server/event_loop_runner.h"
#include "client/socks5_connection.h"
#include "client/vless_client.h"
#include "common/log.h"
#include "common/config.h"
#include "net/tls.h"
#include "proxy/vless/validator.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── CLI 参数（记录显式设置的覆盖项）──────────────────────────────────────
struct Cli {
    bool socks5PortSet = false;
    unsigned socks5Port = 1080;
    bool remoteSet = false;
    std::string remote;
    bool uuidSet = false;
    std::string uuid;
    bool logLevelSet = false;
    std::string logLevel;
    bool workersSet = false;
    unsigned workers = 0;
    bool tlsEnabled = false;      // --tls
    bool tlsEnabledSet = false;
    bool tlsInsecure = false;     // --tls-insecure
    bool tlsInsecureSet = false;
};

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --config <path>        配置文件路径 (default: /etc/vmess/client.json)\n"
              << "                        支持字段: socks5_port/remote/uuid/log_level/workers\n"
              << "  --socks5-port <port>   本地 SOCKS5 监听端口 (default: 1080)\n"
              << "  --remote <host:port>   远端 VLESS 服务器地址 (default: 127.0.0.1:443)\n"
              << "  --uuid <uuid>          VLESS 用户 UUID (默认取配置文件；均无时用内置默认)\n"
              << "  --log <level>          日志级别: none|error|warn|info|debug (default: info)\n"
              << "  --log-file <path>      日志落盘文件（异步日志追加写入；默认仅 stderr）\n"
              << "  --workers <n>          worker 数 (default: CPU 核数)\n"
              << "  --tls                  启用 TLS 传输（VLESS+TLS）\n"
              << "  --tls-insecure         跳过对端证书校验（自签证书场景）\n"
              << std::endl;
}

/// 解析命令行；返回 false 表示应退出（报错或 --help）
bool parseArgs(int argc, char* argv[], Cli& cli, std::string& configPath,
               std::string& logFile) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string();
        };
        if (arg == "--config") {
            configPath = next();
        } else if (arg == "--socks5-port") {
            cli.socks5PortSet = true;
            cli.socks5Port = static_cast<unsigned>(std::atoi(next().c_str()));
        } else if (arg == "--remote") {
            cli.remoteSet = true;
            cli.remote = next();
        } else if (arg == "--uuid") {
            cli.uuidSet = true;
            cli.uuid = next();
        } else if (arg == "--log") {
            cli.logLevelSet = true;
            cli.logLevel = next();
        } else if (arg == "--log-file") {
            logFile = next();
        } else if (arg == "--workers") {
            cli.workersSet = true;
            cli.workers = std::max(1u, static_cast<unsigned>(std::atoi(next().c_str())));
        } else if (arg == "--tls") {
            cli.tlsEnabled = true;
            cli.tlsEnabledSet = true;
        } else if (arg == "--tls-insecure") {
            cli.tlsInsecure = true;
            cli.tlsInsecureSet = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return false;
        } else {
            std::cerr << "[ClientMain] Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

/// 命令行覆盖配置
void applyCliOverrides(const Cli& cli, vmess::common::ClientConfig& cfg) {
    if (cli.socks5PortSet) {
        cfg.socks5Port = static_cast<uint16_t>(cli.socks5Port);
    }
    if (cli.remoteSet) {
        cfg.remote = cli.remote;
    }
    if (cli.uuidSet) {
        cfg.uuid = cli.uuid;
    }
    if (cli.logLevelSet) {
        cfg.logLevel = cli.logLevel;
    }
    if (cli.workersSet) {
        cfg.workers = static_cast<int>(cli.workers);
    }
    if (cli.tlsEnabledSet) {
        cfg.tlsEnabled = cli.tlsEnabled;
    }
    if (cli.tlsInsecureSet) {
        cfg.tlsInsecure = cli.tlsInsecure;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // ── 1. 解析命令行 ────────────────────────────────────────────────────
    Cli cli;
    std::string configPath;
    std::string logFile;
    if (!parseArgs(argc, argv, cli, configPath, logFile)) {
        return 1;
    }

    // 配置路径：--config > VLESS_CLIENT_CONFIG > 默认
    if (configPath.empty()) {
        const char* env = std::getenv("VLESS_CLIENT_CONFIG");
        if (env && env[0] != '\0') {
            configPath = env;
        }
    }
    if (configPath.empty()) {
        configPath = "/etc/vmess/client.json";
    }

    // ── 2. 加载客户端配置（可选；文件不存在时用内置默认，不报错）────────
    vmess::common::ClientConfig ccfg;
    std::string cfgWarn;
    vmess::common::loadClientConfig(configPath, ccfg, cfgWarn);
    applyCliOverrides(cli, ccfg);

    // 兼容旧行为：任何来源都未提供 UUID 时使用内置默认
    if (ccfg.uuid.empty()) {
        ccfg.uuid = "e3e740b0-2c3a-4b0e-9f1a-2c8f7d5e3a1b";
    }

    // ── 3. 日志 ──────────────────────────────────────────────────────────
    if (logFile.empty()) {
        const char* env = std::getenv("VLESS_LOG_FILE");
        if (env && env[0] != '\0') {
            logFile = env;
        }
    }
    vmess::common::Logger::instance().setLogFile(logFile);
    vmess::common::setLogLevel(vmess::common::parseLogLevel(ccfg.logLevel));

    // ── 4. 解析 UUID 与远端地址 ──────────────────────────────────────────
    std::array<uint8_t, 16> uuid{};
    if (!vmess::proxy::vless::Validator::parseUuid(ccfg.uuid, uuid)) {
        std::cerr << "[ClientMain] Invalid uuid: " << ccfg.uuid << std::endl;
        return 1;
    }
    vmess::client::VlessClientConfig cfg = vmess::client::VlessClientConfig::fromString(ccfg.remote);
    cfg.uuid = uuid;
    cfg.tlsEnabled = ccfg.tlsEnabled;
    cfg.tlsInsecure = ccfg.tlsInsecure;
    if (ccfg.tlsEnabled) {
        SSL_CTX* ctx = vmess::net::createClientSslContext(ccfg.tlsInsecure);
        if (!ctx) {
            std::cerr << "[ClientMain] failed to create client TLS context" << std::endl;
            return 1;
        }
        cfg.tlsCtx = std::shared_ptr<void>(ctx, SSL_CTX_free);
    }

    // worker 数（0 = 自动）
    unsigned int workerCount = ccfg.workers > 0
        ? static_cast<unsigned int>(ccfg.workers)
        : std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    std::cerr << "=== VLESS Client (SOCKS5) ===" << std::endl;
    std::cerr << "Config: " << configPath << (cfgWarn.empty() ? "" : " (using defaults)") << std::endl;
    std::cerr << "Socks5 Listen: 127.0.0.1:" << ccfg.socks5Port << std::endl;
    std::cerr << "Remote: " << cfg.remoteHost << ":" << cfg.remotePort << std::endl;
    std::cerr << "Uuid: " << ccfg.uuid << std::endl;
    std::cerr << "LogLevel: " << ccfg.logLevel << std::endl;
    std::cerr << "Workers: " << workerCount << std::endl;
    std::cerr << "Protocol: VLESS + "
              << (ccfg.tlsEnabled ? "TLS (tls-insecure="
                                      + std::string(ccfg.tlsInsecure ? "on" : "off") + ")"
                                  : "plaintext (no TLS)")
              << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;

    // ── 5. 组装 EventLoop 组并运行 ───────────────────────────────────────
    std::vector<std::unique_ptr<vmess::server::EventLoop>> loops;
    std::vector<uint16_t> ports;
    loops.reserve(workerCount);
    for (unsigned int i = 0; i < workerCount; ++i) {
        // 工厂模式：每个 accept 的 fd 创建一个 SOCKS5 连接
        loops.push_back(std::make_unique<vmess::server::EventLoop>(
            [cfg](int clientFd, vmess::net::IoUring& uring) {
                return std::make_unique<vmess::client::Socks5Connection>(
                    clientFd, uring, cfg);
            }));
        ports.push_back(ccfg.socks5Port);
    }

    std::string runError;
    const bool reusePort = workerCount > 1;
    const int rc = vmess::server::runEventLoops(loops, ports, reusePort, &runError);
    if (rc != 0) {
        std::cerr << "[ClientMain] Error: "
                  << (runError.empty() ? "event loop failed" : runError) << std::endl;
        return 1;
    }

    std::cerr << "[ClientMain] Client stopped" << std::endl;
    return 0;
}
