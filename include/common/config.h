#ifndef VLESS_COMMON_CONFIG_H
#define VLESS_COMMON_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace vless {
namespace common {

/// 单个 VLESS 用户（UUID + 备注名）
struct UserConfig {
    std::string uuid;
    std::string name;
};

/// TLS 配置段
struct TlsConfigData {
    bool enabled = false;
    uint16_t port = 8848;
    std::string certFile;             // 正式证书（PEM）；空 = 自签保底
    std::string keyFile;              // 私钥（PEM），与 certFile 成对
    std::string certDir = "./certs";  // 自签证书落盘目录
    int certDays = 365;               // 自签证书有效期
    std::string certSha256;           // 运行时回填：服务器证书 DER 的 SHA-256（hex 小写），用于分享链接 pcs/pinSHA256
};

/// 服务端配置（对应 /etc/vless/config.json）
struct ServerConfig {
    uint16_t port = 1080;             // 明文 VLESS 端口
    std::string logLevel = "info";    // debug/info/warn/error
    int workers = 0;                  // 0 = 自动（CPU 核数）
    std::string host;                 // 公网地址（域名/IP），用于生成分享链接；空则不打印
    std::string remark = "cppvless";  // 分享链接备注名（客户端显示的节点名）
    TlsConfigData tls;
    std::vector<UserConfig> users;    // 认证用户（UUID）列表
};

/// 客户端配置（对应 /etc/vless/client.json）
struct ClientConfig {
    uint16_t socks5Port = 1080;       // 本地 SOCKS5 监听端口
    std::string remote = "127.0.0.1:443"; // 远端 VLESS 服务器 host:port
    std::string uuid;                 // VLESS 用户 UUID
    std::string logLevel = "info";
    int workers = 0;                  // 0 = 自动（CPU 核数）
    bool tlsEnabled = false;          // 启用 TLS 传输（VLESS+TLS）
    bool tlsInsecure = false;         // 跳过对端证书校验（自签证书场景）
};

/**
 * @brief 加载服务端配置。
 *
 *  - 文件存在：解析并填充 cfg（缺失字段保持默认值）。
 *  - 文件不存在（首次启动）：生成默认配置，其中 users 自动生成随机 UUID，
 *    并立即写入文件；*created 置为 true。
 *
 * @return true 成功；false 失败（error 携带原因）
 */
bool loadServerConfig(const std::string& path, ServerConfig& cfg,
                      bool* created, std::string& error);

/**
 * @brief 将服务端配置写入文件（自动创建父目录）。
 */
bool writeServerConfig(const std::string& path, const ServerConfig& cfg,
                       std::string& error);

/**
 * @brief 加载客户端配置。文件不存在或解析失败时返回 true 但 cfg 保持默认
 *        （客户端配置为可选），error 携带警告信息。
 */
bool loadClientConfig(const std::string& path, ClientConfig& cfg,
                      std::string& error);

/// 生成随机 UUID v4 字符串（OpenSSL RAND_bytes）
std::string generateUuid();

} // namespace common
} // namespace vless

#endif // VLESS_COMMON_CONFIG_H
