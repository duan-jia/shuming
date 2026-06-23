#pragma once

#include <string>
#include <unordered_map>

namespace fastllm {

class JwtHandler {
public:
    JwtHandler(const std::string& secret, int expire_seconds = 86400);

    std::string generate_token(int user_id, int tenant_id, const std::string& username, const std::string& role);

    struct TokenClaims {
        int user_id;
        int tenant_id;
        std::string username;
        std::string role;
        bool valid;
        std::string error;
    };

    TokenClaims verify_token(const std::string& token);

private:
    std::string secret_;
    int expire_seconds_;
};

}
