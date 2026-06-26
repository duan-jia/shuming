#include "services/admin_service.h"

#include <mysql/mysql.h>
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

std::string AdminService::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

void AdminService::create_tenant(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    if (!require_admin(user, resp)) return;

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string name = body.value("name", "");
        int token_limit = body.value("token_limit_daily", 100000);
        if (name.empty()) { resp.json_error(400, "name required"); return; }

        auto conn = ctx_.mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }
        std::string sql = "INSERT INTO tenants (name, token_limit_daily) VALUES ('"
            + escape_sql(conn->raw(), name) + "',"
            + std::to_string(token_limit) + ")";
        if (!conn->execute(sql)) {
            ctx_.mysql_pool->release(conn);
            resp.json_error(409, "Tenant name already exists");
            return;
        }
        int tenant_id = mysql_insert_id(conn->raw());
        ctx_.mysql_pool->release(conn);

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

    auto conn = ctx_.mysql_pool->acquire();
    if (!conn) { resp.json_error(500, "Database error"); return; }
    nlohmann::json result = nlohmann::json::array();
    auto* res = conn->query("SELECT id, name, token_limit_daily, status, created_at FROM tenants ORDER BY id");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            nlohmann::json t;
            t["id"] = row[0] ? std::stoi(row[0]) : 0;
            t["name"] = row[1] ? row[1] : "";
            t["token_limit_daily"] = row[2] ? std::stoi(row[2]) : 0;
            t["status"] = row[3] ? row[3] : "active";
            t["created_at"] = row[4] ? row[4] : "";
            result.push_back(t);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    ctx_.mysql_pool->release(conn);
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

        auto conn = ctx_.mysql_pool->acquire();
        if (!conn) { resp.json_error(500, "Database error"); return; }
        std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) VALUES ("
            + std::to_string(tenant_id) + ",'"
            + escape_sql(conn->raw(), username) + "','"
            + escape_sql(conn->raw(), hash) + "','"
            + escape_sql(conn->raw(), role) + "')";
        if (!conn->execute(sql)) {
            ctx_.mysql_pool->release(conn);
            resp.json_error(409, "Username already exists in this tenant");
            return;
        }
        int user_id = mysql_insert_id(conn->raw());
        ctx_.mysql_pool->release(conn);

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

    auto conn = ctx_.mysql_pool->acquire();
    if (!conn) { resp.json_error(500, "Database error"); return; }
    nlohmann::json result = nlohmann::json::array();
    std::string sql = "SELECT id, tenant_id, username, role, created_at FROM users ORDER BY id";
    auto* res = conn->query(sql);
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            nlohmann::json u;
            u["id"] = row[0] ? std::stoi(row[0]) : 0;
            u["tenant_id"] = row[1] ? std::stoi(row[1]) : 0;
            u["username"] = row[2] ? row[2] : "";
            u["role"] = row[3] ? row[3] : "user";
            u["created_at"] = row[4] ? row[4] : "";
            result.push_back(u);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    ctx_.mysql_pool->release(conn);
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

    auto conn = ctx_.mysql_pool->acquire();
    if (conn) {
        std::string sql = "DELETE FROM users WHERE id=" + id_str;
        conn->execute(sql);
        ctx_.mysql_pool->release(conn);
    }
    resp.json(R"({"status":"deleted"})");
}

}
