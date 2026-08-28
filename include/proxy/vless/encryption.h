#ifndef VLESS_PROXY_VLESS_ENCRYPTION_H
#define VLESS_PROXY_VLESS_ENCRYPTION_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <array>

namespace vless {
namespace proxy {
namespace vless {

/**
 * @brief VLESS Encryption 加密方法
 */
enum class EncryptionMethod : uint8_t {
    None = 0,          // 无加密
    X25519_AES256GCM, // X25519 + AES-256-GCM
    X25519_Chacha20,   // X25519 + ChaCha20-Poly1305
};

/**
 * @brief AEAD 加密器（支持 AES-256-GCM 和 ChaCha20-Poly1305）
 */
class AeadCipher {
public:
    /**
     * @brief 创建 AEAD 加密器
     * @param method 加密方法
     * @param key 密钥（32字节）
     * @param forEncrypt true=加密, false=解密
     */
    static std::unique_ptr<AeadCipher> create(
        EncryptionMethod method,
        const uint8_t* key,
        bool forEncrypt);

    virtual ~AeadCipher() = default;

    /**
     * @brief 加密/解密数据
     * @param nonce 12字节随机数
     * @param plaintext 明文（加密时）或密文（解密时，包含16字节tag）
     * @param ciphertext 输出缓冲区
     * @return 成功返回输出长度，失败返回-1
     */
    virtual int process(
        const uint8_t* nonce,
        const uint8_t* in, size_t inLen,
        uint8_t* out) = 0;

    /**
     * @brief 获取 key 长度
     */
    virtual size_t keySize() const = 0;

    /**
     * @brief 获取 nonce 长度
     */
    virtual size_t nonceSize() const = 0;

    /**
     * @brief 获取 tag 长度
     */
    virtual size_t tagSize() const = 0;
};

/**
 * @brief VLESS Encryption 会话
 * 
 * 负责：
 * 1. X25519 密钥交换
 * 2. BLAKE3 密钥派生
 * 3. AEAD 加密/解密
 */
class EncryptionSession {
public:
    EncryptionSession(EncryptionMethod method);
    ~EncryptionSession();

    /**
     * @brief 生成 X25519 密钥对
     * @return 公钥（32字节）
     */
    std::array<uint8_t, 32> generateKeyPair();

    /**
     * @brief 获取公钥（在 generateKeyPair 后调用）
     */
    const std::array<uint8_t, 32>& publicKey() const { return publicKey_; }

    /**
     * @brief 使用对端公钥计算共享密钥并派生会话密钥
     * @param peerPublicKey 对端公钥（32字节）
     * @return 成功返回 true
     */
    bool computeSharedSecret(const uint8_t* peerPublicKey);

    /**
     * @brief 加密数据（client → server 方向）
     * @param plaintext 明文
     * @param ciphertext 输出（格式: nonce(12B) + encrypted + tag(16B)）
     * @return 密文长度，失败返回0
     */
    size_t encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext);

    /**
     * @brief 解密数据（server → client 方向）
     * @param ciphertext 密文（格式: nonce(12B) + encrypted + tag(16B)）
     * @param plaintext 输出
     * @return 成功返回 true
     */
    bool decrypt(const uint8_t* ciphertext, size_t cipherLen, std::vector<uint8_t>& plaintext);

    /**
     * @brief 解密数据（client → server 方向，使用 client 密钥）
     */
    bool decryptClient(const uint8_t* ciphertext, size_t cipherLen, std::vector<uint8_t>& plaintext);

    /**
     * @brief 加密数据（server → client 方向，使用 server 密钥）
     */
    size_t encryptServer(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext);

    EncryptionMethod method() const { return method_; }
    bool isReady() const { return ready_; }

private:
    EncryptionMethod method_;
    bool ready_ = false;

    // X25519 密钥对
    std::array<uint8_t, 32> privateKey_;
    std::array<uint8_t, 32> publicKey_;

    // 派生密钥（每个方向独立）
    std::array<uint8_t, 32> clientKey_;  // client → server
    std::array<uint8_t, 32> serverKey_;  // server → client
    uint64_t clientNonce_ = 0;
    uint64_t serverNonce_ = 0;

    // AEAD 加密器
    std::unique_ptr<AeadCipher> encryptCipher_;
    std::unique_ptr<AeadCipher> decryptCipher_;

    // 内部方法
    void deriveKeys(const uint8_t* sharedSecret);
    void incrementNonce(uint64_t& nonce, uint8_t* nonceBytes);
};

} // namespace vless
} // namespace proxy
} // namespace vless

#endif // VLESS_PROXY_VLESS_ENCRYPTION_H
