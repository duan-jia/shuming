#pragma once

#include <string>
#include <unordered_map>

namespace fastllm {

class JwtHandler {
public:
    JwtHandler(const std::string& secret, int expire_seconds = 900);

    std::string generate_token(int user_id, int tenant_id, const std::string& username, const std::string& role, int auth_version = 0, const std::string& session_id = "");
    int expiry_seconds() const { return expire_seconds_; }

    struct TokenClaims {
        int user_id;
        int tenant_id;
        std::string username;
        std::string role;
        bool valid;
        std::string error;
        int auth_version = 0;
        std::string session_id;
    };

    TokenClaims verify_token(const std::string& token);

private:
    std::string secret_;
    int expire_seconds_;
};

}
