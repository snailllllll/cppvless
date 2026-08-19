#include "server/event_loop.h"
#include "server/event_loop_runner.h"
#include "server/vless_connection.h"
#include "common/log.h"
#include "common/config.h"
#include "common/link.h"
#include "proxy/vless/validator.h"
#include "net/tls.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── CLI 参数（记录显式设置的覆盖项）──────────────────────────────────────
struct Cli {
    bool portSet = false;
    unsigned port = 1080;
    bool logLevelSet = false;
    std::string logLevel = "info";
    bool workersSet = false;
    unsigned workers = 0;
    bool tlsEnabledSet = false;
    bool tlsPortSet = false;
    unsigned tlsPort = 0;
    bool certSet = false;
    std::string cert;
    bool keySet = false;
    std::string key;
    bool certDirSet = false;
    std::string certDir;
    bool certDaysSet = false;
    int certDays = 365;
    bool publicHostSet = false;
    std::string publicHost;
};

void printUsage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [port] [loglevel] [workers]\n"
        "       " << prog << " [--config <path>]\n"
        "       " << prog << " --tls-port <port> [--cert <path> --key <path>]\n"
        "                       [--cert-dir <dir>] [--cert-days <days>]\n"
        "                       [port] [loglevel] [workers]\n"
        "\n"
        "Options:\n"
        "  --config <path>     配置文件路径（默认 /etc/vmess/config.json，或\n"
        "                      环境变量 VLESS_CONFIG）。首次启动自动生成并写入。\n"
        "                      配置文件字段：port/log_level/workers/tls/users，\n"
        "                      命令行参数可覆盖其中的对应字段。\n"
        "  --tls-port <port>  启用 TLS 端口（与明文端口并存；默认 443）\n"
        "  --cert <path>      证书文件（PEM）。与 --key 一起指定则用正式证书\n"
        "  --key <path>       私钥文件（PEM）\n"
        "  --cert-dir <dir>   自签证书落盘目录（默认 ./certs，容器内 /etc/vmess/certs）\n"
        "  --cert-days <days> 自签证书有效期天数（默认 365）\n"
        "  --public-host <host> 公网地址（域名/IP），用于生成 vless:// 分享链接与\n"
        "                      二维码（默认取配置文件 host 字段；均未配置则不打印）\n"
        "  --log-file <path>  日志落盘文件（异步日志追加写入；默认仅 stderr）\n"
        "\n"
        "证书语义（见 doc/18-server-tls-support.md）：\n"
        "  --tls-port + --cert/--key     → 使用正式证书\n"
        "  --tls-port 无证书             → 自签证书保底（自动生成，落盘复用）\n"
        "  --tls-port + 证书加载失败     → 报错退出（fail-fast，不静默降级）\n"
        "\n"
        "用户认证（优先级：命令行/环境变量 > 配置文件）：\n"
        "  1) 配置文件 users 字段（首次启动自动生成随机 UUID 并写入）\n"
        "  2) 环境变量 VLESS_USERS（逗号分隔 UUID 列表，追加）\n"
        "  3) 未配置任何用户时随机生成并持久化\n"
        "\n"
        "位置参数：port 明文端口（默认 1080）、loglevel（debug/info/warn/error）、\n"
        "          workers 线程数（默认 CPU 核数）\n";
}

/// 解析命令行，填充 cli 与 configPath / logFile。返回 false 表示应退出（报错或 --help）。
bool parseArgs(int argc, char* argv[], Cli& cli, std::string& configPath,
               std::string& logFile, std::vector<std::string>& positional) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--tls-port" && i + 1 < argc) {
            cli.tlsEnabledSet = true;
            cli.tlsPortSet = true;
            cli.tlsPort = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--cert" && i + 1 < argc) {
            cli.certSet = true;
            cli.cert = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            cli.keySet = true;
            cli.key = argv[++i];
        } else if (arg == "--cert-dir" && i + 1 < argc) {
            cli.certDirSet = true;
            cli.certDir = argv[++i];
        } else if (arg == "--cert-days" && i + 1 < argc) {
            cli.certDaysSet = true;
            cli.certDays = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--public-host" && i + 1 < argc) {
            cli.publicHostSet = true;
            cli.publicHost = argv[++i];
        } else if (arg == "--log-file" && i + 1 < argc) {
            logFile = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return false;
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::cerr << "[Main] Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    if (!positional.empty()) {
        cli.portSet = true;
        cli.port = static_cast<unsigned>(std::atoi(positional[0].c_str()));
    }
    if (positional.size() > 1) {
        cli.logLevelSet = true;
        cli.logLevel = positional[1];
    }
    if (positional.size() > 2) {
        cli.workersSet = true;
        cli.workers = std::max(1u, static_cast<unsigned>(std::atoi(positional[2].c_str())));
    }
    // CLI 层证书成对校验
    if (cli.certSet != cli.keySet) {
        std::cerr << "[Main] Error: --cert and --key must be specified together" << std::endl;
        return false;
    }
    return true;
}

/// 命令行覆盖配置文件
void applyCliOverrides(const Cli& cli, vmess::common::ServerConfig& cfg) {
    if (cli.portSet) {
        cfg.port = static_cast<uint16_t>(cli.port);
    }
    if (cli.logLevelSet) {
        cfg.logLevel = cli.logLevel;
    }
    if (cli.workersSet) {
        cfg.workers = static_cast<int>(cli.workers);
    }
    if (cli.tlsEnabledSet) {
        cfg.tls.enabled = true;
        if (cli.tlsPortSet) {
            cfg.tls.port = static_cast<uint16_t>(cli.tlsPort);
        }
    }
    if (cli.certSet) {
        cfg.tls.certFile = cli.cert;
    }
    if (cli.keySet) {
        cfg.tls.keyFile = cli.key;
    }
    if (cli.certDirSet) {
        cfg.tls.certDir = cli.certDir;
    }
    if (cli.certDaysSet) {
        cfg.tls.certDays = cli.certDays;
    }
    if (cli.publicHostSet) {
        cfg.host = cli.publicHost;
    }
}

/// 追加 VLESS_USERS 环境变量中的 UUID
void appendEnvUsers(vmess::common::ServerConfig& cfg) {
    const char* usersEnv = std::getenv("VLESS_USERS");
    if (!usersEnv || usersEnv[0] == '\0') {
        return;
    }
    std::stringstream ss(usersEnv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::find_if(token.begin(), token.end(),
                                                [](unsigned char ch) { return !std::isspace(ch); }));
        token.erase(std::find_if(token.rbegin(), token.rend(),
                                 [](unsigned char ch) { return !std::isspace(ch); }).base(),
                    token.end());
        if (!token.empty()) {
            vmess::common::UserConfig uc;
            uc.uuid = token;
            uc.name = "env";
            cfg.users.push_back(std::move(uc));
        }
    }
}

/// 确保至少有一个用户：否则生成随机 UUID 并持久化到配置文件
void ensureDefaultUser(vmess::common::ServerConfig& cfg, const std::string& configPath) {
    if (!cfg.users.empty()) {
        return;
    }
    vmess::common::UserConfig uc;
    uc.uuid = vmess::common::generateUuid();
    uc.name = "default";
    cfg.users.push_back(uc);
    std::string writeErr;
    vmess::common::writeServerConfig(configPath, cfg, writeErr);
    if (!writeErr.empty()) {
        std::cerr << "[Main] Warn: failed to persist generated user: " << writeErr << std::endl;
    }
}

/// 启动横幅（含用户 UUID 列表，便于部署后从日志直接获取）
void printBanner(const vmess::common::ServerConfig& cfg, const std::string& configPath,
                 bool cfgCreated, unsigned int workerCount,
                 const vmess::net::TlsConfig& tlsCfg, const std::string& tlsWarn) {
    std::cerr << "=== VLESS Server ===" << std::endl;
    std::cerr << "Config: " << configPath
              << (cfgCreated ? " (generated on first run)" : "") << std::endl;
    std::cerr << "Port: " << cfg.port << std::endl;
    if (tlsCfg.enabled) {
        std::cerr << "TLS Port: " << cfg.tls.port << std::endl;
        std::cerr << "TLS Cert: "
                  << (tlsCfg.certFile.empty() ? "self-signed (fallback)" : tlsCfg.certFile)
                  << std::endl;
        if (!tlsWarn.empty()) {
            std::cerr << "TLS Warn: " << tlsWarn << std::endl;
        }
    } else {
        std::cerr << "TLS: disabled" << std::endl;
    }
    std::cerr << "LogLevel: " << cfg.logLevel << std::endl;
    std::cerr << "Workers: " << workerCount << std::endl;
    std::cerr << "Users: " << cfg.users.size() << std::endl;
    for (size_t i = 0; i < cfg.users.size(); ++i) {
        std::array<uint8_t, 16> parsed{};
        const bool ok = vmess::proxy::vless::Validator::parseUuid(cfg.users[i].uuid, parsed);
        std::cerr << "  user[" << i << "]: " << cfg.users[i].uuid
                  << (cfg.users[i].name.empty() ? "" : " (" + cfg.users[i].name + ")")
                  << (ok ? "" : " [INVALID]") << std::endl;
    }

    // 分享链接 + 二维码（需配置公网地址 host / --public-host）
    if (!cfg.host.empty()) {
        for (size_t i = 0; i < cfg.users.size(); ++i) {
            const std::string link = vmess::common::buildVlessLink(cfg.host, cfg, cfg.users[i]);
            if (link.empty()) {
                continue;
            }
            std::cerr << std::endl;
            std::cerr << "Share link[" << i << "] (客户端扫码/粘贴导入):" << std::endl;
            std::cerr << "  " << link << std::endl;
            std::cerr << vmess::common::renderQrText(link);
        }
    } else {
        std::cerr << std::endl;
        std::cerr << "Hint: 配置 host（配置文件 host 字段）或 --public-host <域名/IP>"
                     " 可输出 vless:// 分享链接与二维码" << std::endl;
    }
    std::cerr << "Protocol: VLESS"
              << (tlsCfg.enabled ? " + TLS (built-in)" : " (plaintext, no TLS)")
              << std::endl;
    std::cerr << "Press Ctrl+C to stop" << std::endl;
    std::cerr << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    // ── 1. 解析命令行 ────────────────────────────────────────────────────
    Cli cli;
    std::string configPath;
    std::string logFile;
    std::vector<std::string> positional;
    if (!parseArgs(argc, argv, cli, configPath, logFile, positional)) {
        return 1;
    }

    // 配置路径：--config > VLESS_CONFIG > 默认
    if (configPath.empty()) {
        const char* env = std::getenv("VLESS_CONFIG");
        if (env && env[0] != '\0') {
            configPath = env;
        }
    }
    if (configPath.empty()) {
        configPath = "/etc/vmess/config.json";
    }

    // ── 2. 加载配置（首启自动生成随机 UUID 并落盘）──────────────────────
    vmess::common::ServerConfig cfg;
    bool cfgCreated = false;
    std::string cfgError;
    if (!vmess::common::loadServerConfig(configPath, cfg, &cfgCreated, cfgError)) {
        std::cerr << "[Main] Error: " << cfgError << std::endl;
        return 1;
    }
    applyCliOverrides(cli, cfg);

    // 合并后证书成对校验（配置文件来源同样约束）
    if (cfg.tls.certFile.empty() != cfg.tls.keyFile.empty()) {
        std::cerr << "[Main] Error: tls cert_file and key_file must be specified together"
                  << std::endl;
        return 1;
    }

    // ── 3. 用户认证器 ────────────────────────────────────────────────────
    appendEnvUsers(cfg);
    ensureDefaultUser(cfg, configPath);

    vmess::proxy::vless::Validator validator;
    for (const auto& u : cfg.users) {
        if (!validator.addFromString(u.uuid)) {
            std::cerr << "[Main] Warn: invalid uuid ignored: \"" << u.uuid << "\""
                      << (u.name.empty() ? "" : " (name: " + u.name + ")") << std::endl;
        }
    }

    // ── 4. 日志 ──────────────────────────────────────────────────────────
    if (logFile.empty()) {
        const char* env = std::getenv("VLESS_LOG_FILE");
        if (env && env[0] != '\0') {
            logFile = env;
        }
    }
    vmess::common::Logger::instance().setLogFile(logFile);
    vmess::common::setLogLevel(vmess::common::parseLogLevel(cfg.logLevel));

    // ── 5. TLS 上下文（正式证书 / 自签保底 / 失败退出）──────────────────
    vmess::net::TlsConfig tlsCfg;
    tlsCfg.enabled = cfg.tls.enabled;
    tlsCfg.certFile = cfg.tls.certFile;
    tlsCfg.keyFile = cfg.tls.keyFile;
    tlsCfg.certDir = cfg.tls.certDir;
    tlsCfg.certDays = cfg.tls.certDays;

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

    // ── 6. worker 数（0 = 自动）──────────────────────────────────────────
    unsigned int workerCount = cfg.workers > 0
        ? static_cast<unsigned int>(cfg.workers)
        : std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    // ── 7. 组装 EventLoop 组 ─────────────────────────────────────────────
    auto makeFactory = [&validator](SSL_CTX* tls) {
        return [&validator, tls](int clientFd, vmess::net::IoUring& uring) {
            return std::make_unique<vmess::server::VlessConnection>(
                clientFd, uring, validator, tls);
        };
    };

    std::vector<std::unique_ptr<vmess::server::EventLoop>> loops;
    std::vector<uint16_t> ports;
    for (unsigned int i = 0; i < workerCount; ++i) {
        loops.push_back(std::make_unique<vmess::server::EventLoop>(makeFactory(nullptr)));
        ports.push_back(cfg.port);
    }
    if (tlsCfg.enabled) {
        for (unsigned int i = 0; i < workerCount; ++i) {
            loops.push_back(std::make_unique<vmess::server::EventLoop>(makeFactory(tlsCtx.get())));
            ports.push_back(cfg.tls.port);
        }
    }

    // ── 8. 启动横幅 + 运行 ───────────────────────────────────────────────
    printBanner(cfg, configPath, cfgCreated, workerCount, tlsCfg, tlsWarn);

    std::string runError;
    const bool reusePort = workerCount > 1;
    const int rc = vmess::server::runEventLoops(loops, ports, reusePort, &runError);
    if (rc != 0) {
        std::cerr << "[Main] Error: "
                  << (runError.empty() ? "event loop failed" : runError) << std::endl;
        return 1;
    }

    std::cerr << "[Main] Server stopped" << std::endl;
    return 0;
}
