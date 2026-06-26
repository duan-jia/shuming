#include "services/auth_service.h"

#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

#include "security/crypto_utils.h"

namespace fastllm {

AuthService::AuthService(AppContext& ctx) : ctx_(ctx) {}

std::string AuthService::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

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

        auto conn = ctx_.mysql_pool->acquire();
        if (!conn) {
            resp.json_error(503, "Database unavailable");
            return;
        }
        std::string sql = "SELECT id, tenant_id, role, password_hash FROM users WHERE username='"
            + escape_sql(conn->raw(), username) + "'";
        auto* result = conn->query(sql);
        if (!result) {
            ctx_.mysql_pool->release(conn);
            resp.json_error(500, "Database error");
            return;
        }

        auto* row = mysql_fetch_row(result);
        if (!row) {
            mysql_free_result(result);
            ctx_.mysql_pool->release(conn);
            resp.json_error(401, "Invalid credentials");
            return;
        }

        int user_id = std::stoi(row[0]);
        int tenant_id = std::stoi(row[1]);
        std::string role = row[2] ? row[2] : "user";
        std::string password_hash = row[3] ? row[3] : "";
        mysql_free_result(result);
        ctx_.mysql_pool->release(conn);

        if (!CryptoUtils::verify_password(password, password_hash)) {
            resp.json_error(401, "Invalid credentials");
            return;
        }

        std::string token = ctx_.jwt->generate_token(user_id, tenant_id, username, role);

        nlohmann::json j;
        j["token"] = token;
        j["user"]["id"] = user_id;
        j["user"]["username"] = username;
        j["user"]["role"] = role;
        j["user"]["tenant_id"] = tenant_id;
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

}
