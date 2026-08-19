#include "net/tls.h"

#include "common/log.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace vmess {
namespace net {

namespace {

// 证书剩余有效期低于该天数时自动重签
constexpr int kRenewThresholdDays = 30;

std::string opensslErrorString() {
    char buf[256];
    unsigned long err = ERR_get_error();
    if (err == 0) return "no error";
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

bool ensureDir(const std::string& dir) {
    if (dir.empty()) return true;
    if (mkdir(dir.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) {
        struct stat st {};
        if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    LOG_ERROR("Tls", "failed to create cert dir: ", dir,
              " err=", strerror(errno));
    return false;
}

/**
 * @brief 生成自签证书（ECDSA P-256 + X509 v3）
 * @return 生成的证书 PEM 与私钥 PEM
 */
bool generateSelfSigned(std::string* certPem, std::string* keyPem, int days,
                        const std::string& cn) {
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    bool ok = false;

    do {
        // ECDSA P-256（跟随 Xray 官方 cert.Generate 选型：签发性能优于 RSA-2048）
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!pctx) break;
        if (EVP_PKEY_keygen_init(pctx) <= 0) { EVP_PKEY_CTX_free(pctx); break; }
        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0) {
            EVP_PKEY_CTX_free(pctx); break;
        }
        if (EVP_PKEY_keygen(pctx, &pkey) <= 0) { EVP_PKEY_CTX_free(pctx); break; }
        EVP_PKEY_CTX_free(pctx);
        if (!pkey) break;

        // X509 v3
        x509 = X509_new();
        if (!x509) break;

        X509_set_version(x509, 2);  // v3
        ASN1_INTEGER_set(X509_get_serialNumber(x509), (long)time(nullptr));

        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), (long)days * 24 * 3600);

        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char*)cn.c_str(), -1, -1, 0);
        X509_set_issuer_name(x509, name);

        // 自签：签名用自身私钥
        if (!X509_sign(x509, pkey, EVP_sha256())) break;

        // 序列化 PEM
        {
            BIO* bio = BIO_new(BIO_s_mem());
            if (!bio) break;
            if (PEM_write_bio_X509(bio, x509) <= 0) { BIO_free(bio); break; }
            char* ptr = nullptr;
            long len = BIO_get_mem_data(bio, &ptr);
            certPem->assign(ptr, (size_t)len);
            BIO_free(bio);
        }
        {
            BIO* bio = BIO_new(BIO_s_mem());
            if (!bio) break;
            if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) <= 0) {
                BIO_free(bio); break;
            }
            char* ptr = nullptr;
            long len = BIO_get_mem_data(bio, &ptr);
            keyPem->assign(ptr, (size_t)len);
            BIO_free(bio);
        }

        ok = true;
    } while (false);

    if (pkey) EVP_PKEY_free(pkey);
    if (x509) X509_free(x509);

    if (!ok) {
        LOG_ERROR("Tls", "generate self-signed cert failed: ", opensslErrorString());
    }
    return ok;
}

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_ERROR("Tls", "read file failed: ", path);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return !out->empty();
}

/// 解析证书 PEM，返回剩余有效期秒数；失败返回 -1
long certRemainingSeconds(const std::string& certPem) {
    BIO* bio = BIO_new_mem_buf(certPem.data(), (int)certPem.size());
    if (!bio) return -1;
    X509* x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!x509) return -1;

    const ASN1_TIME* notAfter = X509_get0_notAfter(x509);
    long rem = -1;
    if (notAfter) {
        struct tm t {};
        if (ASN1_TIME_to_tm(notAfter, &t)) {
            time_t expire = timegm(&t);
            rem = (long)(expire - time(nullptr));
        }
    }
    X509_free(x509);
    return rem;
}

} // namespace

SSL_CTX* createServerSslContext(const TlsConfig& cfg, std::string* warnOut) {
    if (!cfg.enabled) return nullptr;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        LOG_ERROR("Tls", "SSL_CTX_new failed: ", opensslErrorString());
        return nullptr;
    }

    // 正式证书（--cert/--key）：加载失败报错退出，不降级自签
    if (!cfg.certFile.empty() && !cfg.keyFile.empty()) {
        std::string certPem, keyPem;
        if (!readFile(cfg.certFile, &certPem) || !readFile(cfg.keyFile, &keyPem)) {
            LOG_ERROR("Tls", "failed to read cert/key file: ", cfg.certFile, " / ", cfg.keyFile);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        if (SSL_CTX_use_certificate_chain_file(ctx, cfg.certFile.c_str()) <= 0 ||
            SSL_CTX_use_PrivateKey_file(ctx, cfg.keyFile.c_str(), SSL_FILETYPE_PEM) <= 0 ||
            SSL_CTX_check_private_key(ctx) <= 0) {
            LOG_ERROR("Tls", "failed to load cert/key: ", cfg.certFile, " / ", cfg.keyFile,
                      " err=", opensslErrorString());
            SSL_CTX_free(ctx);
            return nullptr;
        }
        if (warnOut) warnOut->clear();
        return ctx;
    }

    // 自签保底（仅 --tls-port）：生成 / 复用 / 自动重签
    const std::string dir = cfg.certDir.empty() ? "./certs" : cfg.certDir;
    if (!ensureDir(dir)) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    const std::string certPath = dir + "/cert.pem";
    const std::string keyPath = dir + "/key.pem";

    std::string certPem, keyPem;
    bool haveExisting = readFile(certPath, &certPem) && readFile(keyPath, &keyPem);

    if (haveExisting) {
        long rem = certRemainingSeconds(certPem);
        if (rem > 0 && rem > (long)kRenewThresholdDays * 24 * 3600) {
            // 复用现有证书
            if (SSL_CTX_use_certificate_chain_file(ctx, certPath.c_str()) <= 0 ||
                SSL_CTX_use_PrivateKey_file(ctx, keyPath.c_str(), SSL_FILETYPE_PEM) <= 0 ||
                SSL_CTX_check_private_key(ctx) <= 0) {
                LOG_ERROR("Tls", "failed to load existing self-signed cert: ",
                          certPath, " err=", opensslErrorString());
                SSL_CTX_free(ctx);
                return nullptr;
            }
            if (warnOut) {
                *warnOut = "self-signed fallback: reusing existing cert " + certPath;
            }
            return ctx;
        }
        // 剩余 <30 天或已过期：重新生成
        LOG_WARN("Tls", "self-signed cert remaining < ", kRenewThresholdDays,
                 " days, regenerating");
    }

    // 生成新自签证书
    const std::string cn = "vmess-self-signed";
    if (!generateSelfSigned(&certPem, &keyPem, cfg.certDays, cn)) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // 落盘
    {
        std::ofstream out(certPath, std::ios::binary | std::ios::trunc);
        out << certPem;
    }
    {
        std::ofstream out(keyPath, std::ios::binary | std::ios::trunc);
        out << keyPem;
    }

    // 权限收紧：私钥仅 owner 可读写
    chmod(keyPath.c_str(), 0600);

    if (SSL_CTX_use_certificate_chain_file(ctx, certPath.c_str()) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, keyPath.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_check_private_key(ctx) <= 0) {
        LOG_ERROR("Tls", "failed to load generated self-signed cert: ",
                  certPath, " err=", opensslErrorString());
        SSL_CTX_free(ctx);
        return nullptr;
    }

    if (warnOut) {
        *warnOut = "self-signed fallback active: generated new cert " + certPath;
    }
    return ctx;
}

SSL_CTX* createClientSslContext(bool insecure) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        LOG_ERROR("Tls", "SSL_CTX_new(client) failed: ", opensslErrorString());
        return nullptr;
    }

    if (insecure) {
        // 自签证书场景：跳过对端证书校验（与 Xray allowInsecure 语义一致）
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        // 正式证书场景：校验对端证书，信任系统 CA
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ctx);
    }
    return ctx;
}

} // namespace net
} // namespace vmess
