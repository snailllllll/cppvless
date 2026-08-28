#ifndef VLESS_NET_TLS_H
#define VLESS_NET_TLS_H

#include <openssl/ssl.h>

#include <cstdint>
#include <string>

namespace vless {
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
    // 实际加载的服务器证书指纹：证书 DER 的 SHA-256（hex 小写，64 字符）。
    // 语义与 Xray pinned_peer_cert_sha256 一致（sha256(cert.Raw)），
    // 格式要求 hex（Xray infra/conf 用 hex.DecodeString 解析；base64 会报错），
    // 供分享链接输出 pcs=/pinSHA256= 供客户端固定证书（替代已移除的 allowInsecure）。
    std::string certSha256;
};

/**
 * @brief 创建服务器 SSL_CTX（证书三级判定入口）
 *
 * 语义分级（见 doc/18-server-tls-support.md §6.3.1）：
 *   - 配置了 --cert/--key  → 加载文件证书（失败报错退出，不降级）
 *   - 仅 --tls-port         → 自签保底：certDir 下已有有效证书则复用，
 *                             否则生成并落盘；剩余有效期 <30 天自动重签
 *
 * @param cfg TLS 配置（非 const：成功加载证书后回填 cfg.certSha256）
 * @param warnOut 自签保底触发时的提示信息（供调用方打印日志）
 * @return 创建成功的 SSL_CTX（服务器角色），失败返回 nullptr
 */
SSL_CTX* createServerSslContext(TlsConfig& cfg, std::string* warnOut = nullptr);

/**
 * @brief 创建客户端 SSL_CTX（TLS_client_method）
 *
 * @param insecure true 时跳过证书校验（自签证书场景，对应 Xray allowInsecure）；
 *                 false 时校验对端证书并信任系统 CA（正式证书场景）。
 * @return 创建成功的 SSL_CTX（客户端角色），失败返回 nullptr
 */
SSL_CTX* createClientSslContext(bool insecure);

} // namespace net
} // namespace vless

#endif // VLESS_NET_TLS_H
