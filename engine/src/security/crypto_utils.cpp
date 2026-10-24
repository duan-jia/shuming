#include "security/crypto_utils.h"

#include <spdlog/spdlog.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace fastllm {

static const std::string BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string CryptoUtils::base64_encode(const unsigned char* data, size_t len) {
    std::string result;
    result.reserve(4 * ((len + 2) / 3));

    for (size_t i = 0; i < len; i += 3) {
        unsigned int val = (data[i] << 16);
        if (i + 1 < len) val |= (data[i + 1] << 8);
        if (i + 2 < len) val |= data[i + 2];

        result += BASE64_CHARS[(val >> 18) & 0x3F];
        result += BASE64_CHARS[(val >> 12) & 0x3F];
        result += (i + 1 < len) ? BASE64_CHARS[(val >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? BASE64_CHARS[val & 0x3F] : '=';
    }

    return result;
}

std::vector<unsigned char> CryptoUtils::base64_decode(const std::string& encoded) {
    std::vector<int> d(256, -1);
    for (int i = 0; i < 64; i++) d[BASE64_CHARS[i]] = i;

    std::vector<unsigned char> result;
    result.reserve(encoded.size() * 3 / 4);

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (d[c] == -1) break;
        val = (val << 6) + d[c];
        valb += 6;
        if (valb >= 0) {
            result.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return result;
}

std::string CryptoUtils::encrypt(const std::string& plaintext, const std::string& key) {
    unsigned char iv[12];
    RAND_bytes(iv, sizeof(iv));

    unsigned char tag[16];
    std::vector<unsigned char> ciphertext(plaintext.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()), iv);

    int len;
    EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                      reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size());

    int ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
    ciphertext_len += len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    // Format: IV(12) + TAG(16) + ciphertext
    std::vector<unsigned char> combined;
    combined.insert(combined.end(), iv, iv + 12);
    combined.insert(combined.end(), tag, tag + 16);
    combined.insert(combined.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);

    return base64_encode(combined.data(), combined.size());
}

std::string CryptoUtils::decrypt(const std::string& ciphertext_b64, const std::string& key) {
    auto combined = base64_decode(ciphertext_b64);
    if (combined.size() < 28) return "";

    unsigned char* iv = combined.data();
    unsigned char* tag = combined.data() + 12;
    unsigned char* ciphertext = combined.data() + 28;
    int ciphertext_len = combined.size() - 28;

    std::vector<unsigned char> plaintext(ciphertext_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()), iv);

    int len;
    EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext, ciphertext_len);

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);

    EVP_CIPHER_CTX_free(ctx);

    if (ret > 0) {
        return std::string(reinterpret_cast<char*>(plaintext.data()), ciphertext_len);
    }

    spdlog::warn("Decryption failed: authentication tag mismatch");
    return "";
}

std::string CryptoUtils::generate_key() {
    unsigned char key[32];
    RAND_bytes(key, sizeof(key));
    return std::string(reinterpret_cast<char*>(key), 32);
}

std::string CryptoUtils::hash_password(const std::string& password, int iterations) {
    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt));

    unsigned char hash[32];
    PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                      salt, sizeof(salt), iterations,
                      EVP_sha256(), sizeof(hash), hash);

    std::ostringstream oss;
    oss << "pbkdf2$" << iterations << "$"
        << base64_encode(salt, sizeof(salt)) << "$"
        << base64_encode(hash, sizeof(hash));
    return oss.str();
}

bool CryptoUtils::verify_password(const std::string& password, const std::string& stored) {
    if (stored.size() < 8 || stored.substr(0, 7) != "pbkdf2$") return false;

    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= stored.size(); ++i) {
        if (i == stored.size() || stored[i] == '$') {
            parts.push_back(stored.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 4) return false;

    int iterations = 0;
    try { iterations = std::stoi(parts[1]); } catch (...) { return false; }
    if (iterations < 1000 || iterations > 1000000
        || parts[1] != std::to_string(iterations)) return false;

    auto salt = base64_decode(parts[2]);
    auto expected = base64_decode(parts[3]);
    if (salt.size() != 16 || expected.size() != 32) return false;
    if (base64_encode(salt.data(), salt.size()) != parts[2]
        || base64_encode(expected.data(), expected.size()) != parts[3]) return false;

    unsigned char hash[32];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                      salt.data(), static_cast<int>(salt.size()), iterations,
                      EVP_sha256(), sizeof(hash), hash) != 1) return false;
    return CRYPTO_memcmp(hash, expected.data(), sizeof(hash)) == 0;
}

KeyManager::KeyManager(const std::string& master_key) : master_key_(master_key) {}

std::string KeyManager::get_dek(int tenant_id) {
    return derive_key("tenant:" + std::to_string(tenant_id));
}

std::string KeyManager::derive_key(const std::string& info) {
    unsigned char result[32];
    unsigned int result_len = 0;

    HMAC(EVP_sha256(), master_key_.data(), master_key_.size(),
         reinterpret_cast<const unsigned char*>(info.data()), info.size(),
         result, &result_len);

    return std::string(reinterpret_cast<char*>(result), result_len);
}

}
