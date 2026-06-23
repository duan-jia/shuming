#include "auth/jwt_handler.h"

#include <spdlog/spdlog.h>
#include <jwt-cpp/jwt.h>
#include <chrono>

namespace fastllm {

JwtHandler::JwtHandler(const std::string& secret, int expire_seconds)
    : secret_(secret), expire_seconds_(expire_seconds) {}

std::string JwtHandler::generate_token(int user_id, int tenant_id,
                                        const std::string& username,
                                        const std::string& role) {
    auto now = std::chrono::system_clock::now();
    auto exp = now + std::chrono::seconds(expire_seconds_);

    auto token = jwt::create()
        .set_issuer("fast-llm-reasoner")
        .set_type("JWT")
        .set_issued_at(now)
        .set_expires_at(exp)
        .set_payload_claim("user_id", jwt::claim(std::to_string(user_id)))
        .set_payload_claim("tenant_id", jwt::claim(std::to_string(tenant_id)))
        .set_payload_claim("username", jwt::claim(username))
        .set_payload_claim("role", jwt::claim(role))
        .sign(jwt::algorithm::hs256{secret_});

    return token;
}

JwtHandler::TokenClaims JwtHandler::verify_token(const std::string& token) {
    TokenClaims claims{0, 0, "", "", false, ""};

    try {
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret_})
            .with_issuer("fast-llm-reasoner");

        verifier.verify(decoded);

        claims.user_id = std::stoi(decoded.get_payload_claim("user_id").as_string());
        claims.tenant_id = std::stoi(decoded.get_payload_claim("tenant_id").as_string());
        claims.username = decoded.get_payload_claim("username").as_string();
        claims.role = decoded.get_payload_claim("role").as_string();
        claims.valid = true;

    } catch (const std::exception& e) {
        claims.error = e.what();
        spdlog::debug("JWT verify failed: {}", e.what());
    }

    return claims;
}

}
