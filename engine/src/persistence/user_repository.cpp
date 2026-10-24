#include "persistence/user_repository.h"

#include <mysql/mysql.h>

namespace fastllm {

UserRepository::UserRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::string UserRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

UserLookupStatus UserRepository::find_login_user(const std::string& username, LoginUser& user) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return UserLookupStatus::Unavailable;

    std::string sql = "SELECT u.id, u.tenant_id, u.username, u.role, u.password_hash, u.status, t.status FROM users u JOIN tenants t ON t.id=u.tenant_id WHERE u.username='"
        + escape_sql(conn->raw(), username) + "' LIMIT 1";
    auto* result = conn->query(sql);
    if (!result) {
        mysql_.release(conn);
        return UserLookupStatus::Unavailable;
    }

    auto* row = mysql_fetch_row(result);
    if (!row) {
        mysql_free_result(result);
        mysql_.release(conn);
        return UserLookupStatus::NotFound;
    }

    user.id = row[0] ? std::stoi(row[0]) : 0;
    user.tenant_id = row[1] ? std::stoi(row[1]) : 0;
    user.username = row[2] ? row[2] : "";
    user.role = row[3] ? row[3] : "user";
    user.password_hash = row[4] ? row[4] : "";
    user.active = row[5] && std::string(row[5]) == "active"
        && row[6] && std::string(row[6]) == "active";
    mysql_free_result(result);
    mysql_.release(conn);
    return UserLookupStatus::Found;
}

bool UserRepository::create_user(int tenant_id, const std::string& username,
                                 const std::string& password_hash,
                                 const std::string& role, int& user_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) VALUES ("
        + std::to_string(tenant_id) + ",'"
        + escape_sql(conn->raw(), username) + "','"
        + escape_sql(conn->raw(), password_hash) + "','"
        + escape_sql(conn->raw(), role) + "')";
    bool ok = conn->execute(sql);
    if (ok) {
        user_id = static_cast<int>(mysql_insert_id(conn->raw()));
    }
    mysql_.release(conn);
    return ok;
}

bool UserRepository::create_guest_user(int tenant_id, const std::string& username, int& user_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    const std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role) "
        "SELECT id, '" + escape_sql(conn->raw(), username)
        + "', '!guest-password-login-disabled', 'guest' FROM tenants WHERE id="
        + std::to_string(tenant_id) + " AND name='__guest__' AND status='active'";
    const bool ok = conn->execute(sql) && mysql_affected_rows(conn->raw()) == 1;
    if (ok) user_id = static_cast<int>(mysql_insert_id(conn->raw()));
    mysql_.release(conn);
    return ok;
}

std::vector<UserSummary> UserRepository::list_users() {
    std::vector<UserSummary> users;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return users;

    auto* res = conn->query("SELECT id, tenant_id, username, role, created_at FROM users ORDER BY id");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            UserSummary user;
            user.id = row[0] ? std::stoi(row[0]) : 0;
            user.tenant_id = row[1] ? std::stoi(row[1]) : 0;
            user.username = row[2] ? row[2] : "";
            user.role = row[3] ? row[3] : "user";
            user.created_at = row[4] ? row[4] : "";
            users.push_back(user);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return users;
}

bool UserRepository::delete_user(int user_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "DELETE FROM users WHERE id=" + std::to_string(user_id);
    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

}
