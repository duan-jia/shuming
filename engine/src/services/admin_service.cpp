#include "services/admin_service.h"

#include <nlohmann/json.hpp>

#include "security/crypto_utils.h"

namespace fastllm {

AdminService::AdminService(AppContext& ctx) : ctx_(ctx) {}

bool AdminService::require_admin(const UserInfo& user, HttpResponse& resp) {
    if (user.role != Role::ADMIN) {
        resp.json_error(403, "Admin only");
        return false;
    }
    return true;
}

void AdminService::create_tenant(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string name = body.value("name", "");
        int token_limit = body.value("token_limit_daily", 100000);
        if (name.empty()) { resp.json_error(400, "name required"); return; }

        int tenant_id = 0;
        if (!ctx_.tenant_repository || !ctx_.tenant_repository->create_tenant(name, token_limit, tenant_id)) {
            resp.json_error(409, "Tenant name already exists");
            return;
        }

        nlohmann::json j;
        j["id"] = tenant_id;
        j["name"] = name;
        j["token_limit_daily"] = token_limit;
        j["status"] = "created";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void AdminService::list_tenants(const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    nlohmann::json result = nlohmann::json::array();
    if (ctx_.tenant_repository) {
        for (const auto& tenant : ctx_.tenant_repository->list_tenants()) {
            nlohmann::json t;
            t["id"] = tenant.id;
            t["name"] = tenant.name;
            t["token_limit_daily"] = tenant.token_limit_daily;
            t["status"] = tenant.status;
            t["created_at"] = tenant.created_at;
            result.push_back(t);
        }
    }
    nlohmann::json j;
    j["tenants"] = result;
    resp.json(j.dump());
}

void AdminService::create_user(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        int tenant_id = body.value("tenant_id", user.tenant_id);
        std::string role = body.value("role", "user");

        if (username.empty() || password.empty()) {
            resp.json_error(400, "username and password required");
            return;
        }
        if (role != "admin" && role != "user") {
            resp.json_error(400, "role must be 'admin' or 'user'");
            return;
        }

        std::string hash = CryptoUtils::hash_password(password);

        int user_id = 0;
        if (!ctx_.user_repository
            || !ctx_.user_repository->create_user(tenant_id, username, hash, role, user_id)) {
            resp.json_error(409, "Username already exists in this tenant");
            return;
        }

        nlohmann::json j;
        j["id"] = user_id;
        j["username"] = username;
        j["tenant_id"] = tenant_id;
        j["role"] = role;
        j["status"] = "created";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void AdminService::list_users(const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    nlohmann::json result = nlohmann::json::array();
    if (ctx_.user_repository) {
        for (const auto& user_summary : ctx_.user_repository->list_users()) {
            nlohmann::json u;
            u["id"] = user_summary.id;
            u["tenant_id"] = user_summary.tenant_id;
            u["username"] = user_summary.username;
            u["role"] = user_summary.role;
            u["created_at"] = user_summary.created_at;
            result.push_back(u);
        }
    }
    nlohmann::json j;
    j["users"] = result;
    resp.json(j.dump());
}

void AdminService::delete_user(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    std::string id_str = params.get("id");
    if (id_str.empty()) { resp.json_error(400, "User ID required"); return; }
    int target_id = std::stoi(id_str);
    if (target_id == user.id) { resp.json_error(400, "Cannot delete yourself"); return; }

    if (ctx_.user_repository) {
        ctx_.user_repository->delete_user(target_id);
    }
    resp.json(R"({"status":"deleted"})");
}

}
