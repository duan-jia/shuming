#include "services/auth_service.h"

#include <nlohmann/json.hpp>

#include "security/crypto_utils.h"
#include "security/guest_policy.h"
#include <openssl/rand.h>

namespace fastllm {

AuthService::AuthService(AppContext& ctx) : ctx_(ctx),
    dummy_password_hash_(CryptoUtils::hash_password("non-account-timing-placeholder")) {}

UserInfo AuthService::extract_user(const HttpRequest& req) const {
    UserInfo info{0, 0, "", Role::USER};
    auto auth = req.header("Authorization", "");
    if (auth.substr(0, 7) == "Bearer ") {
        auto claims = ctx_.jwt->verify_token(auth.substr(7));
        if (claims.valid) {
            info.id = claims.user_id;
            info.tenant_id = claims.tenant_id;
            info.username = claims.username;
            info.role = RbacManager::string_to_role(claims.role);
        }
    }
    return info;
}

bool AuthService::require_auth(const HttpRequest& req, HttpResponse& resp, UserInfo& user) const {
    user = extract_user(req);
    if (user.id == 0) {
        resp.json_error(401, "Unauthorized");
        return false;
    }
    if (user.role == Role::UNKNOWN) { resp.json_error(403, "Forbidden"); return false; }
    if (user.role == Role::GUEST) {
        if (!ctx_.guest_enabled || user.tenant_id != ctx_.guest_tenant_id
            || !guest_route_allowed(req.method_string(), req.path)) {
            resp.json_error(403, "Guest access is restricted"); return false;
        }
        if (req.method == HttpMethod::POST && req.path == "/api/sessions"
            && !ctx_.guest_limiter.allow_guest_session(user.id)) {
            resp.set_header("Retry-After", "900"); resp.json_error(429, "Guest session limit reached"); return false;
        }
    }
    return true;
}

bool AuthService::require_permission(const UserInfo& user, const std::string& resource,
                                     const std::string& action, HttpResponse& resp) const {
    if (!ctx_.rbac->check_permission(user.role, resource, action)) {
        resp.json_error(403, "Forbidden");
        return false;
    }
    return true;
}

void AuthService::guest_config(HttpResponse& resp) const {
    resp.set_header("Cache-Control", "no-store");
    resp.json(nlohmann::json{{"guest_enabled", ctx_.guest_enabled}}.dump());
}

void AuthService::guest_login(const HttpRequest& req, HttpResponse& resp) {
    resp.set_header("Cache-Control", "no-store");
    if (!ctx_.guest_enabled || ctx_.guest_tenant_id <= 0) {
        resp.json_error(403, "Guest login is disabled"); return;
    }
    if (!ctx_.guest_limiter.allow_guest_login(req.peer_ip)) {
        resp.set_header("Retry-After", "3600"); resp.json_error(429, "Guest login limit reached"); return;
    }
    // Client-supplied IDs, roles and quotas are never accepted.
    if (req.body.size() > 64 || (!req.body.empty() && req.body != "{}")) {
        resp.json_error(400, "Guest login takes no parameters"); return;
    }
    try {
        unsigned char bytes[24];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) { resp.json_error(503, "Authentication unavailable"); return; }
        const char* hex = "0123456789abcdef";
        std::string name = "guest_";
        for (auto byte : bytes) { name += hex[byte >> 4]; name += hex[byte & 15]; }
        int id = 0;
        if (!ctx_.user_repository || !ctx_.user_repository->create_guest_user(ctx_.guest_tenant_id, name, id)) {
            resp.json_error(503, "Guest account unavailable"); return;
        }
        auto token = ctx_.jwt->generate_token(id, ctx_.guest_tenant_id, name, "guest");
        resp.json(nlohmann::json{{"token", token}, {"expires_in", 900},
            {"user", {{"id", id}, {"tenant_id", ctx_.guest_tenant_id}, {"username", name}, {"role", "guest"}}}}.dump());
    } catch (...) { resp.json_error(503, "Guest account unavailable"); }
}

void AuthService::login(const HttpRequest& req, HttpResponse& resp) {
    resp.set_header("Cache-Control", "no-store");
    if (!login_limiter_.allow_source(req.peer_ip)) {
        resp.set_header("Retry-After", "600");
        resp.json_error(429, "Too many login attempts; try again later");
        return;
    }
    if (req.body.size() > 4096) { resp.json_error(400, "Invalid login request"); return; }
    try {
        auto body = nlohmann::json::parse(req.body);
        if (!body.is_object() || !body.contains("username") || !body["username"].is_string()
            || !body.contains("password") || !body["password"].is_string()) {
            resp.json_error(400, "Invalid login request"); return;
        }
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");

        if (username.empty() || username.size() > 256 || password.empty() || password.size() > 1024
            || username.find('\0') != std::string::npos || password.find('\0') != std::string::npos) {
            resp.json_error(400, "Invalid login request"); return;
        }
        if (!ctx_.user_repository) {
            resp.json_error(503, "Database unavailable");
            return;
        }

        LoginUser login_user;
        auto lookup_status = ctx_.user_repository->find_login_user(username, login_user);
        if (lookup_status == UserLookupStatus::Unavailable) {
            resp.json_error(503, "Database unavailable");
            return;
        }
        // Use the database's canonical identity so case/collation variants
        // cannot bypass per-account throttling.
        std::string key = lookup_status == UserLookupStatus::Found
            ? "id:" + std::to_string(login_user.id) : "missing:" + username;
        if (!login_limiter_.allow_account(key)) {
            resp.set_header("Retry-After", "600");
            resp.json_error(429, "Too many login attempts; try again later"); return;
        }
        bool password_ok = CryptoUtils::verify_password(password,
            lookup_status == UserLookupStatus::Found ? login_user.password_hash : dummy_password_hash_);
        if (lookup_status != UserLookupStatus::Found || !password_ok || !login_user.active
            || login_user.id <= 0 || login_user.tenant_id <= 0
            || (login_user.role != "admin" && login_user.role != "user")) {
            resp.json_error(401, "Invalid credentials");
            return;
        }

        std::string token = ctx_.jwt->generate_token(
            login_user.id, login_user.tenant_id, login_user.username, login_user.role);

        nlohmann::json j;
        j["token"] = token;
        j["user"]["id"] = login_user.id;
        j["user"]["username"] = login_user.username;
        j["user"]["role"] = login_user.role;
        j["user"]["tenant_id"] = login_user.tenant_id;
        resp.json(j.dump());
    } catch (const nlohmann::json::exception&) {
        resp.json_error(400, "Invalid login request");
    } catch (const std::exception&) {
        resp.json_error(503, "Authentication unavailable");
    }
}

}
