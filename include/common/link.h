#ifndef VLESS_COMMON_LINK_H
#define VLESS_COMMON_LINK_H

#include "common/config.h"

#include <string>
#include <vector>

namespace vless {
namespace common {

/**
 * @brief 生成标准 VLESS 分享链接（vless:// 格式，兼容 v2rayN / v2rayNG /
 *        Shadowrocket / Clash 等客户端扫码或粘贴导入）
 *
 * 格式：vless://<uuid>@<host>:<port>?encryption=none&security=<tls|none>&type=tcp&headerType=none#<remark>
 *
 * @param host  服务器公网地址（域名或 IP），需运维在配置中填写
 * @param cfg   服务端配置（端口 / TLS 信息决定链接的端口与 security）
 * @param user  用户（UUID + 备注名）
 * @param remark 链接备注（默认 "cppvless"）
 * @return vless:// 链接；host 为空时返回空串
 */
std::string buildVlessLink(const std::string& host,
                           const ServerConfig& cfg,
                           const UserConfig& user,
                           const std::string& remark = "cppvless");

/// 终端渲染的 ASCII 二维码（UTF-8 半块字符，深色背景终端），空文本返回空串
std::string renderQrText(const std::string& text, int border = 2);

/// 便捷函数：构建链接并返回终端二维码（多行文本），host 为空时返回空串
std::string buildVlessQrText(const std::string& host,
                             const ServerConfig& cfg,
                             const UserConfig& user,
                             const std::string& remark = "cppvless");

/**
 * @brief 探测本机公网 IP（IPv4/IPv6）。
 *
 * 依次尝试多个公网 IP 回显服务（api.ipify.org / icanhazip.com / ifconfig.me，
 * 纯 HTTP GET，无需外部命令），返回第一个成功且校验合法的地址。
 * 网络不通或超时返回空串。整个探测耗时约 1~3 秒（每个源单独超时）。
 *
 * @param timeoutMs 每个源的单次连接超时（毫秒，默认 3000）
 */
std::string detectPublicIp(int timeoutMs = 3000);

} // namespace common
} // namespace vless

#endif // VLESS_COMMON_LINK_H
