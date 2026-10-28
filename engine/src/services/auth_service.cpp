#include "services/auth_service.h"

#include <nlohmann/json.hpp>

#include "security/crypto_utils.h"
#include <openssl/rand.h>

namespace fastllm {

AuthService::AuthService(AppContext& ctx) : ctx_(ctx),
    dummy_password_hash_(CryptoUtils::hash_password("non-account-timing-placeholder")) {}

UserInfo AuthService::extract_user(const HttpRequest& req) const {
    UserInfo info{0, 0, "", Role::USER};
    auto auth = req.header("Authorization", "");
    if (auth.substr(0, 7) == "Bearer ") {
        auto claims = ctx_.jwt->verify_token(auth.substr(7));
        if (claims.valid && ctx_.user_repository) {
            LoginUser account;
            const auto status = ctx_.user_repository->find_user_by_id(claims.user_id, account);
            if (status == UserLookupStatus::Unavailable) throw std::runtime_error("Authentication database unavailable");
            if (status != UserLookupStatus::Found || !account.active || account.tenant_id != claims.tenant_id
                || account.role != claims.role || account.auth_version != claims.auth_version) return info;
            if (!claims.session_id.empty() && !ctx_.user_repository->valid_auth_session(claims.session_id, account.id, account.auth_version)) return info;
            info.id = account.id;
            info.tenant_id = account.tenant_id;
            info.username = account.username;
            info.role = RbacManager::string_to_role(account.role);
            info.must_change_password = account.must_change_password;
        }
    }
    return info;
}

bool AuthService::require_auth(const HttpRequest& req, HttpResponse& resp, UserInfo& user) const {
    try { user = extract_user(req); }
    catch (const std::exception&) { resp.json_error(503, "Authentication unavailable"); return false; }
    if (user.id == 0) {
        resp.json_error(401, "Unauthorized");
        return false;
    }
    if (!RbacManager::check_route_permission(user.role, req.method_string(), req.path)) {
        resp.json_error(403, "Forbidden"); return false;
    }
    if (user.role == Role::GUEST) {
        if (!ctx_.guest_enabled || user.tenant_id != ctx_.guest_tenant_id) {
            resp.json_error(403, "Guest access is restricted"); return false;
        }
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
    if (!login_limiter_.allow_source(req.peer_ip)) {
        resp.set_header("Retry-After", "600"); resp.json_error(429, "Too many login attempts; try again later"); return;
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
            {"user", {{"id", id}, {"tenant_id", ctx_.guest_tenant_id}, {"username", name}, {"role", "guest"}, {"tenant_name", kGuestTenantName}}}}.dump());
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

        issue_login(login_user, resp);
    } catch (const nlohmann::json::exception&) {
        resp.json_error(400, "Invalid login request");
    } catch (const std::exception&) {
        resp.json_error(503, "Authentication unavailable");
    }
}

namespace {
nlohmann::json identity_json(const LoginUser& user) {
    return {{"id", user.id}, {"tenant_id", user.tenant_id}, {"username", user.username},
            {"role", user.role}, {"must_change_password", false}};
}
bool refresh_input(const HttpRequest& req, std::string& token) {
    if (req.body.size() > 2048) return false;
    auto data = nlohmann::json::parse(req.body, nullptr, false);
    if (!data.is_object() || !data.contains("refresh_token") || !data["refresh_token"].is_string()) return false;
    token = data["refresh_token"].get<std::string>();
    return token.size() == 97 && token[32] == '.'
        && token.substr(0,32).find_first_not_of("0123456789abcdef") == std::string::npos
        && token.substr(33).find_first_not_of("0123456789abcdef") == std::string::npos;
}
}

void AuthService::issue_login(const LoginUser& user, HttpResponse& resp) {
    const auto id = CryptoUtils::random_hex(16);
    const auto refresh = id + "." + CryptoUtils::random_hex(32);
    if (!ctx_.user_repository->create_auth_session(user.id, user.auth_version, id,
        CryptoUtils::sha256(refresh), ctx_.refresh_expiry_seconds)) {
        resp.json_error(503, "Could not create login session"); return;
    }
    resp.json(nlohmann::json({{"token", ctx_.jwt->generate_token(user.id,user.tenant_id,user.username,user.role,user.auth_version,id)},
        {"expires_in", ctx_.jwt->expiry_seconds()}, {"refresh_token", refresh},
        {"refresh_expires_in", ctx_.refresh_expiry_seconds}, {"user", identity_json(user)}}).dump());
}

void AuthService::refresh(const HttpRequest& req, HttpResponse& resp) {
    resp.set_header("Cache-Control", "no-store");
    if (req.method != HttpMethod::POST) { resp.json_error(405, "POST required"); return; }
    std::string old;
    if (!refresh_input(req, old)) { resp.json_error(401, "Login session expired"); return; }
    if (!refresh_limiter_.allow_refresh(old.substr(0,32))) { resp.json_error(429,"Too many refresh requests"); return; }
    if (!ctx_.user_repository) { resp.json_error(503, "Authentication unavailable"); return; }
    try {
        const auto id = old.substr(0,32);
        const auto fresh = id + "." + CryptoUtils::random_hex(32);
        LoginUser user; int ttl = 0;
        const auto status = ctx_.user_repository->rotate_auth_session(id,CryptoUtils::sha256(old),CryptoUtils::sha256(fresh),user,ttl);
        if (status == UserLookupStatus::Unavailable) { resp.json_error(503, "Authentication unavailable"); return; }
        if (status != UserLookupStatus::Found) { resp.json_error(401, "Login session expired"); return; }
        resp.json(nlohmann::json({{"token",ctx_.jwt->generate_token(user.id,user.tenant_id,user.username,user.role,user.auth_version,id)},
            {"expires_in",ctx_.jwt->expiry_seconds()}, {"refresh_token",fresh}, {"refresh_expires_in",ttl}, {"user",identity_json(user)}}).dump());
    } catch (...) { resp.json_error(503, "Authentication unavailable"); }
}

void AuthService::logout(const HttpRequest& req, HttpResponse& resp) {
    resp.set_header("Cache-Control", "no-store");
    if (req.method != HttpMethod::POST) { resp.json_error(405, "POST required"); return; }
    const auto authorization = req.header("Authorization", "");
    if (authorization.rfind("Bearer ",0)==0) {
        const auto claims = ctx_.jwt->verify_token(authorization.substr(7));
        if (claims.valid && !claims.session_id.empty()) {
            if (!ctx_.user_repository || !ctx_.user_repository->revoke_access_session(claims.session_id,claims.user_id,claims.auth_version)) {
                resp.json_error(503,"Could not revoke login session"); return;
            }
            resp.json(R"({"status":"logged_out"})"); return;
        }
    }
    std::string token;
    if (refresh_input(req, token) && (!ctx_.user_repository
        || !ctx_.user_repository->revoke_auth_session(token.substr(0,32),CryptoUtils::sha256(token)))) {
        resp.json_error(503, "Could not revoke login session"); return;
    }
    resp.json(R"({"status":"logged_out"})");
}

void AuthService::me(const HttpRequest& req, HttpResponse& resp) {
    if (req.method != HttpMethod::GET) { resp.json_error(405,"GET required"); return; }
    UserInfo user{};
    if (!require_auth(req,resp,user)) return;
    resp.set_header("Cache-Control","no-store");
    resp.json(nlohmann::json({{"user",{{"id",user.id},{"username",user.username},{"tenant_id",user.tenant_id},
        {"role",RbacManager::role_to_string(user.role)},{"must_change_password",false}}}}).dump());
}

void AuthService::change_password(const HttpRequest& req, HttpResponse& resp) {
    resp.set_header("Cache-Control","no-store");
    if (req.method != HttpMethod::POST) { resp.json_error(405,"POST required"); return; }
    UserInfo user{};
    if (!require_auth(req,resp,user)) return;
    if (!password_limiter_.allow_account(std::to_string(user.id))) { resp.json_error(429,"Too many password attempts"); return; }
    if (req.body.size()>4096) { resp.json_error(400,"Invalid request"); return; }
    const auto body=nlohmann::json::parse(req.body,nullptr,false);
    if (!body.is_object() || !body.contains("current_password") || !body["current_password"].is_string()
        || !body.contains("new_password") || !body["new_password"].is_string()) { resp.json_error(400,"Passwords required"); return; }
    const auto old=body["current_password"].get<std::string>();
    const auto fresh=body["new_password"].get<std::string>();
    if (!CryptoUtils::valid_password(fresh) || old.size()>1024 || old.find('\0')!=std::string::npos) {
        resp.json_error(400,"New password must contain 8 to 16 characters"); return;
    }
    LoginUser account;
    if (ctx_.user_repository->find_user_by_id(user.id,account)!=UserLookupStatus::Found) { resp.json_error(503,"Account unavailable"); return; }
    if (!CryptoUtils::verify_password(old,account.password_hash)) { resp.json_error(403,"Current password is incorrect"); return; }
    try {
        if (!ctx_.user_repository->change_password(account,CryptoUtils::hash_password(fresh),false)) {
            resp.json_error(409,"Account changed; retry with current credentials"); return;
        }
        resp.json(R"({"status":"password_changed","reauthenticate":true})");
    } catch (...) { resp.json_error(503,"Password change failed"); }
}

}
