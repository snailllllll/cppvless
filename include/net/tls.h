#ifndef VMESS_NET_TLS_H
#define VMESS_NET_TLS_H

#include <openssl/ssl.h>

#include <cstdint>
#include <string>

namespace vmess {
namespace net {

/**
 * @brief TLS 服务器配置（CLI --tls-port 相关）
 */
struct TlsConfig {
    bool enabled = false;           // --tls-port 指定时置 true
    std::string certFile;           // --cert（可选；未指定时走自签保底）
    std::string keyFile;            // --key
    std::string certDir;            // --cert-dir 自签证书落盘目录
    int certDays = 365;             // --cert-days 自签有效期（天）
};

/**
 * @brief 创建服务器 SSL_CTX（证书三级判定入口）
 *
 * 语义分级（见 doc/18-server-tls-support.md §6.3.1）：
 *   - 配置了 --cert/--key  → 加载文件证书（失败报错退出，不降级）
 *   - 仅 --tls-port         → 自签保底：certDir 下已有有效证书则复用，
 *                             否则生成并落盘；剩余有效期 <30 天自动重签
 *
 * @param cfg TLS 配置
 * @param warnOut 自签保底触发时的提示信息（供调用方打印日志）
 * @return 创建成功的 SSL_CTX（服务器角色），失败返回 nullptr
 */
SSL_CTX* createServerSslContext(const TlsConfig& cfg, std::string* warnOut = nullptr);

} // namespace net
} // namespace vmess

#endif // VMESS_NET_TLS_H
