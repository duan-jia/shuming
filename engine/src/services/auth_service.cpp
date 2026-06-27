#include "services/auth_service.h"

#include <nlohmann/json.hpp>

#include "security/crypto_utils.h"

namespace fastllm {

AuthService::AuthService(AppContext& ctx) : ctx_(ctx) {}

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

void AuthService::login(const HttpRequest& req, HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");

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
        if (lookup_status == UserLookupStatus::NotFound) {
            resp.json_error(401, "Invalid credentials");
            return;
        }

        if (!CryptoUtils::verify_password(password, login_user.password_hash)) {
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
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

}
