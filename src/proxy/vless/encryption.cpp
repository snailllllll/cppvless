#include "proxy/vless/encryption.h"

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <cstring>
#include <stdexcept>

namespace vmess {
namespace proxy {
namespace vless {

namespace {

bool deriveKeySha256(const char* label,
                     const uint8_t* secret,
                     size_t secretLen,
                     uint8_t out[32]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, label, std::strlen(label)) == 1 &&
              EVP_DigestUpdate(ctx, secret, secretLen) == 1;

    unsigned int outLen = 0;
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, out, &outLen) == 1 && outLen == 32;
    }

    EVP_MD_CTX_free(ctx);
    return ok;
}

} // namespace

// ── AES-256-GCM ─────────────────────────────────────────────────────────────

class Aes256GcmCipher : public AeadCipher {
public:
    Aes256GcmCipher(const uint8_t* key, bool forEncrypt)
        : forEncrypt_(forEncrypt) {
        memcpy(key_, key, 32);
    }

    int process(const uint8_t* nonce, const uint8_t* in, size_t inLen, uint8_t* out) override {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return -1;

        const EVP_CIPHER* cipher = EVP_aes_256_gcm();
        int len, ciphertextLen;

        if (forEncrypt_) {
            // 加密
            if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
                EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_, nonce) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            if (EVP_EncryptUpdate(ctx, out, &len, in, inLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen = len;

            if (EVP_EncryptFinal_ex(ctx, out + len, &len) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen += len;

            // 获取 GCM tag
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + ciphertextLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen += 16;
            EVP_CIPHER_CTX_free(ctx);
            return ciphertextLen;

        } else {
            // 解密（in 包含密文 + 16字节 tag）
            if (inLen < 16) return -1;
            size_t ciphertextLen = inLen - 16;

            if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
                EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_, nonce) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            // 设置 expected tag
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                     const_cast<uint8_t*>(in + ciphertextLen)) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            if (EVP_DecryptUpdate(ctx, out, &len, in, ciphertextLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            int plaintextLen = len;

            // Finalize 会验证 tag
            if (EVP_DecryptFinal_ex(ctx, out + len, &len) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            plaintextLen += len;
            EVP_CIPHER_CTX_free(ctx);
            return plaintextLen;
        }
    }

    size_t keySize() const override { return 32; }
    size_t nonceSize() const override { return 12; }
    size_t tagSize() const override { return 16; }

private:
    uint8_t key_[32];
    bool forEncrypt_;
};

// ── ChaCha20-Poly1305 ────────────────────────────────────────────────────────

class ChaCha20Cipher : public AeadCipher {
public:
    ChaCha20Cipher(const uint8_t* key, bool forEncrypt)
        : forEncrypt_(forEncrypt) {
        memcpy(key_, key, 32);
    }

    int process(const uint8_t* nonce, const uint8_t* in, size_t inLen, uint8_t* out) override {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return -1;

        const EVP_CIPHER* cipher = EVP_chacha20_poly1305();
        int len, ciphertextLen;

        if (forEncrypt_) {
            if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1 ||
                EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_, nonce) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            if (EVP_EncryptUpdate(ctx, out, &len, in, inLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen = len;

            if (EVP_EncryptFinal_ex(ctx, out + len, &len) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen += len;

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + ciphertextLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            ciphertextLen += 16;
            EVP_CIPHER_CTX_free(ctx);
            return ciphertextLen;

        } else {
            if (inLen < 16) return -1;
            size_t ciphertextLen2 = inLen - 16;

            if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1 ||
                EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_, nonce) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                     const_cast<uint8_t*>(in + ciphertextLen2)) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            if (EVP_DecryptUpdate(ctx, out, &len, in, ciphertextLen2) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            int plaintextLen = len;

            if (EVP_DecryptFinal_ex(ctx, out + len, &len) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }
            plaintextLen += len;
            EVP_CIPHER_CTX_free(ctx);
            return plaintextLen;
        }
    }

    size_t keySize() const override { return 32; }
    size_t nonceSize() const override { return 12; }
    size_t tagSize() const override { return 16; }

private:
    uint8_t key_[32];
    bool forEncrypt_;
};

// ── AeadCipher 工厂 ─────────────────────────────────────────────────────────

std::unique_ptr<AeadCipher> AeadCipher::create(
    EncryptionMethod method,
    const uint8_t* key,
    bool forEncrypt) {
    switch (method) {
        case EncryptionMethod::X25519_AES256GCM:
            return std::make_unique<Aes256GcmCipher>(key, forEncrypt);
        case EncryptionMethod::X25519_Chacha20:
            return std::make_unique<ChaCha20Cipher>(key, forEncrypt);
        default:
            return nullptr;
    }
}

// ── EncryptionSession ───────────────────────────────────────────────────────

EncryptionSession::EncryptionSession(EncryptionMethod method)
    : method_(method) {
}

EncryptionSession::~EncryptionSession() = default;

std::array<uint8_t, 32> EncryptionSession::generateKeyPair() {
    // 使用 OpenSSL 生成 X25519 密钥对
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) {
        throw std::runtime_error("EVP_PKEY_CTX_new_id failed");
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_keygen_init failed");
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_keygen failed");
    }
    EVP_PKEY_CTX_free(ctx);

    // 提取私钥和公钥
    size_t len = 32;
    EVP_PKEY_get_raw_private_key(pkey, privateKey_.data(), &len);
    len = 32;
    EVP_PKEY_get_raw_public_key(pkey, publicKey_.data(), &len);
    EVP_PKEY_free(pkey);

    return publicKey_;
}

bool EncryptionSession::computeSharedSecret(const uint8_t* peerPublicKey) {
    // 使用自己的私钥和对端公钥计算共享密钥
    EVP_PKEY* privKey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr, privateKey_.data(), 32);
    if (!privKey) return false;

    EVP_PKEY* peerKey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, peerPublicKey, 32);
    if (!peerKey) {
        EVP_PKEY_free(privKey);
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(privKey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(privKey);
        EVP_PKEY_free(peerKey);
        return false;
    }

    if (EVP_PKEY_derive_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(privKey);
        EVP_PKEY_free(peerKey);
        return false;
    }

    if (EVP_PKEY_derive_set_peer(ctx, peerKey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(privKey);
        EVP_PKEY_free(peerKey);
        return false;
    }

    size_t sharedLen = 32;
    uint8_t sharedSecret[32];
    if (EVP_PKEY_derive(ctx, sharedSecret, &sharedLen) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(privKey);
        EVP_PKEY_free(peerKey);
        return false;
    }

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(privKey);
    EVP_PKEY_free(peerKey);

    // 派生会话密钥
    deriveKeys(sharedSecret);
    return true;
}

void EncryptionSession::deriveKeys(const uint8_t* sharedSecret) {
    // 使用 OpenSSL SHA-256 派生双向会话密钥，避免依赖外部 third_party/blake3。
    if (!deriveKeySha256("VLESS Encryption: client key", sharedSecret, 32, clientKey_.data()) ||
        !deriveKeySha256("VLESS Encryption: server key", sharedSecret, 32, serverKey_.data())) {
        throw std::runtime_error("failed to derive session keys");
    }

    // 创建 AEAD 加密器
    encryptCipher_ = AeadCipher::create(method_, clientKey_.data(), true);
    decryptCipher_ = AeadCipher::create(method_, serverKey_.data(), false);

    ready_ = true;
}

void EncryptionSession::incrementNonce(uint64_t& nonce, uint8_t* nonceBytes) {
    // 将 uint64 nonce 转换为 12 字节（小端）
    memcpy(nonceBytes, &nonce, 8);
    memset(nonceBytes + 8, 0, 4);
    nonce++;
}

size_t EncryptionSession::encrypt(const std::vector<uint8_t>& plaintext,
                                   std::vector<uint8_t>& ciphertext) {
    if (!ready_ || !encryptCipher_) return 0;

    ciphertext.resize(12 + plaintext.size() + 16);

    // 生成 nonce
    uint8_t nonce[12];
    incrementNonce(clientNonce_, nonce);
    memcpy(ciphertext.data(), nonce, 12);

    // 加密
    int outLen = encryptCipher_->process(
        nonce,
        plaintext.data(), plaintext.size(),
        ciphertext.data() + 12);

    if (outLen < 0) return 0;
    ciphertext.resize(12 + outLen);
    return ciphertext.size();
}

bool EncryptionSession::decrypt(const uint8_t* ciphertext, size_t cipherLen,
                                 std::vector<uint8_t>& plaintext) {
    if (!ready_ || !decryptCipher_) return false;
    if (cipherLen < 12 + 16) return false;

    const uint8_t* nonce = ciphertext;
    const uint8_t* encrypted = ciphertext + 12;
    size_t encryptedLen = cipherLen - 12;

    plaintext.resize(encryptedLen - 16);

    int outLen = decryptCipher_->process(
        nonce, encrypted, encryptedLen, plaintext.data());

    if (outLen < 0) return false;
    plaintext.resize(outLen);
    return true;
}

bool EncryptionSession::decryptClient(const uint8_t* ciphertext, size_t cipherLen,
                                      std::vector<uint8_t>& plaintext) {
    // 使用 client 密钥解密（client → server 方向）
    if (!ready_) return false;
    if (cipherLen < 12 + 16) return false;

    // 临时创建 client 方向的解密器
    auto decipher = AeadCipher::create(method_, clientKey_.data(), false);
    if (!decipher) return false;

    const uint8_t* nonce = ciphertext;
    const uint8_t* encrypted = ciphertext + 12;
    size_t encryptedLen = cipherLen - 12;

    plaintext.resize(encryptedLen - 16);

    int outLen = decipher->process(nonce, encrypted, encryptedLen, plaintext.data());
    if (outLen < 0) return false;
    plaintext.resize(outLen);
    return true;
}

size_t EncryptionSession::encryptServer(const std::vector<uint8_t>& plaintext,
                                        std::vector<uint8_t>& ciphertext) {
    // 使用 server 密钥加密（server → client 方向）
    if (!ready_) return 0;

    auto encipher = AeadCipher::create(method_, serverKey_.data(), true);
    if (!encipher) return 0;

    ciphertext.resize(12 + plaintext.size() + 16);

    uint8_t nonce[12];
    incrementNonce(serverNonce_, nonce);
    memcpy(ciphertext.data(), nonce, 12);

    int outLen = encipher->process(
        nonce, plaintext.data(), plaintext.size(), ciphertext.data() + 12);

    if (outLen < 0) return 0;
    ciphertext.resize(12 + outLen);
    return ciphertext.size();
}

} // namespace vless
} // namespace proxy
} // namespace vmess
