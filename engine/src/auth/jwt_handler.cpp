#include "auth/jwt_handler.h"

#include <spdlog/spdlog.h>
#include <jwt-cpp/jwt.h>
#include <chrono>
#include <algorithm>

namespace fastllm {

JwtHandler::JwtHandler(const std::string& secret, int expire_seconds)
    : secret_(secret), expire_seconds_(expire_seconds) {}

std::string JwtHandler::generate_token(int user_id, int tenant_id,
                                        const std::string& username,
                                        const std::string& role, int auth_version, const std::string& session_id) {
    auto now = std::chrono::system_clock::now();
    auto exp = now + std::chrono::seconds(role == "guest" ? std::min(expire_seconds_, 900) : expire_seconds_);

    auto token = jwt::create()
        .set_issuer("fast-llm-reasoner")
        .set_type("JWT")
        .set_issued_at(now)
        .set_expires_at(exp)
        .set_payload_claim("user_id", jwt::claim(std::to_string(user_id)))
        .set_payload_claim("tenant_id", jwt::claim(std::to_string(tenant_id)))
        .set_payload_claim("username", jwt::claim(username))
        .set_payload_claim("role", jwt::claim(role))
        .set_payload_claim("auth_version", jwt::claim(std::to_string(auth_version)))
        .set_payload_claim("sid", jwt::claim(session_id))
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

        if (!decoded.has_expires_at() || !decoded.has_issued_at()) throw std::runtime_error("Required JWT times missing");
        auto parse_id = [](const std::string& value) {
            if (value.empty() || value.size() > 10 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Invalid identity ID");
            const auto id = std::stoi(value);
            if (id <= 0) throw std::runtime_error("Invalid identity ID");
            return id;
        };
        claims.user_id = parse_id(decoded.get_payload_claim("user_id").as_string());
        claims.tenant_id = parse_id(decoded.get_payload_claim("tenant_id").as_string());
        claims.username = decoded.get_payload_claim("username").as_string();
        claims.role = decoded.get_payload_claim("role").as_string();
        if (claims.user_id <= 0 || claims.tenant_id <= 0 || claims.username.empty()
            || (claims.role != "admin" && claims.role != "user" && claims.role != "guest"))
            throw std::runtime_error("Invalid identity claims");
        if (decoded.has_payload_claim("auth_version")) {
            const auto value = decoded.get_payload_claim("auth_version").as_string();
            if (value.empty() || value.size() > 10 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Invalid credential version");
            claims.auth_version = std::stoi(value);
        }
        if (decoded.has_payload_claim("sid")) {
            claims.session_id = decoded.get_payload_claim("sid").as_string();
            if (!claims.session_id.empty() && (claims.session_id.size() != 32
                || claims.session_id.find_first_not_of("0123456789abcdef") != std::string::npos))
                throw std::runtime_error("Invalid login session");
        }
        claims.valid = true;

    } catch (const std::exception& e) {
        claims.error = e.what();
        spdlog::debug("JWT verify failed: {}", e.what());
    }

    return claims;
}

}
