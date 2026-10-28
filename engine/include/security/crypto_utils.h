#pragma once

#include <string>
#include <vector>

namespace fastllm {

class CryptoUtils {
public:
    static std::string encrypt(const std::string& plaintext, const std::string& key);
    static std::string decrypt(const std::string& ciphertext, const std::string& key);
    static std::string generate_key();
    static std::string base64_encode(const unsigned char* data, size_t len);
    static std::vector<unsigned char> base64_decode(const std::string& encoded);

    static bool valid_password(const std::string& password);
    static std::string random_hex(size_t bytes);
    static std::string sha256(const std::string& value);
    static std::string hash_password(const std::string& password, int iterations = 100000);
    static bool verify_password(const std::string& password, const std::string& stored);
};

class KeyManager {
public:
    explicit KeyManager(const std::string& master_key);

    std::string get_dek(int tenant_id);
    std::string get_master_key() const { return master_key_; }

private:
    std::string derive_key(const std::string& info);
    std::string master_key_;
};

}
