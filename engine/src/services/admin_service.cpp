#include "services/admin_service.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>

#include "security/crypto_utils.h"

namespace fastllm {

namespace {
bool parse_quota(const nlohmann::json& value, std::optional<long long>& quota) {
    if (value.is_null()) { quota.reset(); return true; }
    if (!value.is_number_integer() || value < 1 || value > kMaxDailyTokenQuota) return false;
    quota = value.get<long long>();
    return true;
}
}

AdminService::AdminService(AppContext& ctx) : ctx_(ctx) {}

bool AdminService::require_permission(const UserInfo& user, Permission permission, HttpResponse& resp) {
    if (!RbacManager::check_permission(user.role, permission)) {
        resp.json_error(403, "Admin only");
        return false;
    }
    return true;
}

void AdminService::create_tenant(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    if (!require_permission(user, Permission::TENANT_MANAGE, resp)) return;

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string name = body.value("name", "");
        std::optional<long long> quota;
        if (!parse_quota(body.value("token_limit_daily", nlohmann::json(100000)), quota) || !quota) {
            resp.json_error(400, "token_limit_daily must be a positive safe integer"); return;
        }
        const long long token_limit = *quota;
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
    if (!require_permission(user, Permission::TENANT_MANAGE, resp)) return;

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
    if (!require_permission(user, Permission::USER_MANAGE, resp)) return;

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        int tenant_id = body.value("tenant_id", user.tenant_id);
        std::string role = body.value("role", "user");
        std::optional<long long> quota;
        if (!parse_quota(body.value("daily_token_quota", nlohmann::json(nullptr)), quota)) {
            resp.json_error(400, "daily_token_quota must be null or an integer from 1 to 9007199254740991"); return;
        }

        const bool must_change = false;
        if (username.empty() || username.size()>64 || username.find('\0')!=std::string::npos || !CryptoUtils::valid_password(password)) {
            resp.json_error(400, "Username and a password of 8 to 16 characters required");
            return;
        }
        if (role != "admin" && role != "user") {
            resp.json_error(400, "role must be 'admin' or 'user'");
            return;
        }

        std::string hash = CryptoUtils::hash_password(password);

        int user_id = 0;
        if (!ctx_.user_repository
            || !ctx_.user_repository->create_user(tenant_id, username, hash, role, user_id, quota, must_change)) {
            resp.json_error(409, "Username already exists in this tenant");
            return;
        }

        nlohmann::json j;
        j["id"] = user_id;
        j["username"] = username;
        j["tenant_id"] = tenant_id;
        j["role"] = role;
        j["must_change_password"] = must_change;
        j["daily_token_quota"] = quota ? nlohmann::json(*quota) : nlohmann::json(nullptr);
        j["status"] = "created";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void AdminService::list_users(const UserInfo& user, HttpResponse& resp) {
    if (!require_permission(user, Permission::USER_MANAGE, resp)) return;

    nlohmann::json result = nlohmann::json::array();
    if (ctx_.user_repository) {
        for (const auto& user_summary : ctx_.user_repository->list_users()) {
            nlohmann::json u;
            u["id"] = user_summary.id;
            u["tenant_id"] = user_summary.tenant_id;
            u["username"] = user_summary.username;
            u["role"] = user_summary.role;
            u["created_at"] = user_summary.created_at;
            u["must_change_password"] = false;
            u["daily_token_quota"] = user_summary.daily_token_quota
                ? nlohmann::json(*user_summary.daily_token_quota) : nlohmann::json(nullptr);
            u["effective_daily_token_quota"] = user_summary.effective_daily_token_quota;
            u["today_token_limit"] = user_summary.today_token_limit;
            u["today_tokens_used"] = user_summary.today_tokens_used;
            u["today_remaining_tokens"] = std::max(0LL, user_summary.today_token_limit - user_summary.today_tokens_used);
            result.push_back(u);
        }
    }
    nlohmann::json j;
    j["users"] = result;
    resp.json(j.dump());
}

void AdminService::reset_user_password(const HttpRequest& req, const RouteParams& params,
                                        const UserInfo& user, HttpResponse& resp) {
    if (!require_permission(user,Permission::USER_MANAGE,resp)) return;
    resp.set_header("Cache-Control","no-store");
    const auto id=params.get("id"); int target=0;
    const auto parsed=std::from_chars(id.data(),id.data()+id.size(),target);
    if (parsed.ec!=std::errc{} || parsed.ptr!=id.data()+id.size() || target<=0) { resp.json_error(400,"Invalid user ID"); return; }
    if (target==user.id) { resp.json_error(400,"Use your account password settings"); return; }
    if (req.body.size()>4096) { resp.json_error(400,"Invalid request"); return; }
    const auto body=nlohmann::json::parse(req.body,nullptr,false);
    if (!body.is_object() || !body.contains("password") || !body["password"].is_string()
        || !CryptoUtils::valid_password(body["password"].get<std::string>())) {
        resp.json_error(400,"Password must contain 8 to 16 characters"); return;
    }
    LoginUser account;
    if (!ctx_.user_repository) { resp.json_error(503,"Database unavailable"); return; }
    const auto found=ctx_.user_repository->find_user_by_id(target,account);
    if (found==UserLookupStatus::NotFound) { resp.json_error(404,"User not found"); return; }
    if (found!=UserLookupStatus::Found) { resp.json_error(503,"Database unavailable"); return; }
    if (account.role=="guest") { resp.json_error(400,"Guests have no password login"); return; }
    try {
        if (!ctx_.user_repository->change_password(account,CryptoUtils::hash_password(body["password"].get<std::string>()),false)) {
            resp.json_error(409,"Account changed; retry"); return;
        }
        resp.json(R"({"status":"password_reset","must_change_password":false})");
    } catch (...) { resp.json_error(503,"Password reset failed"); }
}

void AdminService::set_user_quota(const HttpRequest& req, const RouteParams& params,
                                  const UserInfo& user, HttpResponse& resp) {
    if (!require_permission(user, Permission::USER_MANAGE, resp)) return;
    const auto id = params.get("id");
    int target_id = 0;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), target_id);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || target_id <= 0) {
        resp.json_error(400, "Invalid user ID"); return;
    }
    std::optional<long long> quota;
    try {
        const auto body = nlohmann::json::parse(req.body);
        if (!body.is_object() || !body.contains("daily_token_quota")
            || !parse_quota(body.at("daily_token_quota"), quota)) {
            resp.json_error(400, "daily_token_quota must be null or an integer from 1 to 9007199254740991"); return;
        }
    } catch (const nlohmann::json::exception&) {
        resp.json_error(400, "Invalid JSON request"); return;
    }
    if (!ctx_.user_repository) { resp.json_error(503, "User database unavailable"); return; }
    const auto status = ctx_.user_repository->set_daily_token_quota(target_id, quota);
    if (status == UserLookupStatus::NotFound) { resp.json_error(404, "User not found"); return; }
    if (status != UserLookupStatus::Found) { resp.json_error(503, "Failed to update quota"); return; }
    resp.json(nlohmann::json({{"status", "updated"}, {"id", target_id},
        {"daily_token_quota", quota ? nlohmann::json(*quota) : nlohmann::json(nullptr)},
        {"applies_to_today", true}}).dump());
}

void AdminService::delete_user(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    if (!require_permission(user, Permission::USER_MANAGE, resp)) return;

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
