#include "persistence/user_repository.h"
#include "persistence/tenant_repository.h"
#include "persistence/mysql_transaction.h"

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

    std::string sql = "SELECT u.id, u.tenant_id, u.username, u.role, u.password_hash, u.status, t.status, u.auth_version, u.must_change_password FROM users u JOIN tenants t ON t.id=u.tenant_id WHERE u.username='"
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
    user.auth_version = row[7] ? std::stoi(row[7]) : 0;
    user.must_change_password = row[8] && std::string(row[8]) == "1";
    mysql_free_result(result);
    mysql_.release(conn);
    return UserLookupStatus::Found;
}

bool UserRepository::create_user(int tenant_id, const std::string& username,
                                 const std::string& password_hash,
                                 const std::string& role, int& user_id, std::optional<long long> daily_token_quota, bool must_change_password) {
    if (daily_token_quota && (*daily_token_quota < 1 || *daily_token_quota > kMaxDailyTokenQuota)) return false;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "INSERT INTO users (tenant_id, username, password_hash, role, daily_token_quota, must_change_password) VALUES ("
        + std::to_string(tenant_id) + ",'"
        + escape_sql(conn->raw(), username) + "','"
        + escape_sql(conn->raw(), password_hash) + "','"
        + escape_sql(conn->raw(), role) + "',"
        + (daily_token_quota ? std::to_string(*daily_token_quota) : "NULL") + "," + (must_change_password ? "1" : "0") + ")";
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
        + std::to_string(tenant_id) + " AND name='" + kGuestTenantName + "' AND status='active'";
    const bool ok = conn->execute(sql) && mysql_affected_rows(conn->raw()) == 1;
    if (ok) user_id = static_cast<int>(mysql_insert_id(conn->raw()));
    mysql_.release(conn);
    return ok;
}

std::vector<UserSummary> UserRepository::list_users() {
    std::vector<UserSummary> users;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return users;

    auto* res = conn->query("SELECT u.id, u.tenant_id, u.username, u.role, u.created_at, u.daily_token_quota, "
        "COALESCE(u.daily_token_quota,t.token_limit_daily), "
        "COALESCE(d.token_limit,COALESCE(u.daily_token_quota,t.token_limit_daily)), "
        "COALESCE(d.tokens_used,0),u.must_change_password FROM users u JOIN tenants t ON t.id=u.tenant_id "
        "LEFT JOIN user_token_usage_daily d ON d.user_id=u.id AND d.usage_date=CURDATE() ORDER BY u.id");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            UserSummary user;
            user.id = row[0] ? std::stoi(row[0]) : 0;
            user.tenant_id = row[1] ? std::stoi(row[1]) : 0;
            user.username = row[2] ? row[2] : "";
            user.role = row[3] ? row[3] : "user";
            user.created_at = row[4] ? row[4] : "";
            if (row[5]) user.daily_token_quota = std::stoll(row[5]);
            user.effective_daily_token_quota = row[6] ? std::stoll(row[6]) : 0;
            user.today_token_limit = row[7] ? std::stoll(row[7]) : 0;
            user.today_tokens_used = row[8] ? std::stoll(row[8]) : 0;
            user.must_change_password = row[9] && std::string(row[9]) == "1";
            users.push_back(user);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return users;
}

UserLookupStatus UserRepository::set_daily_token_quota(int user_id, std::optional<long long> quota) {
    if (quota && (*quota < 1 || *quota > kMaxDailyTokenQuota)) return UserLookupStatus::Unavailable;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return UserLookupStatus::Unavailable;
    UserLookupStatus status = UserLookupStatus::Unavailable;
    {
        MysqlTransaction tx(*conn);
        if (tx.active()) {
            // Serialize quota edits and user deletion. Daily usage updates keep
            // tokens_used intact, including concurrent billing settlements.
            auto* res = conn->query("SELECT id FROM users WHERE id=" + std::to_string(user_id) + " FOR UPDATE");
            if (res) {
                const bool found = mysql_fetch_row(res) != nullptr;
                mysql_free_result(res);
                if (!found) {
                    status = UserLookupStatus::NotFound;
                } else if (conn->execute("UPDATE users SET daily_token_quota="
                        + (quota ? std::to_string(*quota) : "NULL")
                        + " WHERE id=" + std::to_string(user_id))
                    && conn->execute("INSERT INTO user_token_usage_daily "
                        "(tenant_id,user_id,usage_date,token_limit,tokens_used) "
                        "SELECT u.tenant_id,u.id,CURDATE(),COALESCE(u.daily_token_quota,t.token_limit_daily),0 "
                        "FROM users u JOIN tenants t ON t.id=u.tenant_id WHERE u.id="
                        + std::to_string(user_id)
                        + " ON DUPLICATE KEY UPDATE token_limit=VALUES(token_limit)")
                    && tx.commit()) {
                    status = UserLookupStatus::Found;
                }
            }
        }
    }
    mysql_.release(conn);
    return status;
}

bool UserRepository::delete_user(int user_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "DELETE FROM users WHERE id=" + std::to_string(user_id);
    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

namespace {
UserLookupStatus load_identity(MysqlConnection& conn, int id, LoginUser& user, bool lock = false) {
    auto* res = conn.query("SELECT u.id,u.tenant_id,u.username,u.role,u.password_hash,u.status,t.status,u.auth_version,u.must_change_password "
        "FROM users u JOIN tenants t ON t.id=u.tenant_id WHERE u.id=" + std::to_string(id) + (lock ? " FOR UPDATE" : ""));
    if (!res) return UserLookupStatus::Unavailable;
    auto* row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return UserLookupStatus::NotFound; }
    user.id = std::stoi(row[0]); user.tenant_id = std::stoi(row[1]);
    user.username = row[2]; user.role = row[3]; user.password_hash = row[4];
    user.active = std::string(row[5]) == "active" && std::string(row[6]) == "active";
    user.auth_version = std::stoi(row[7]); user.must_change_password = std::string(row[8]) == "1";
    mysql_free_result(res);
    return UserLookupStatus::Found;
}
}

UserLookupStatus UserRepository::find_user_by_id(int id, LoginUser& user) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return UserLookupStatus::Unavailable;
    const auto result = load_identity(*conn, id, user);
    mysql_.release(conn);
    return result;
}

bool UserRepository::change_password(const LoginUser& expected, const std::string& hash, bool must_change) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    bool ok = false;
    {
        MysqlTransaction tx(*conn);
        ok = tx.active() && conn->execute("UPDATE users SET password_hash='" + escape_sql(conn->raw(), hash)
            + "',auth_version=auth_version+1,must_change_password=" + (must_change ? "1" : "0")
            + " WHERE id=" + std::to_string(expected.id) + " AND auth_version=" + std::to_string(expected.auth_version)
            + " AND password_hash='" + escape_sql(conn->raw(), expected.password_hash) + "'")
            && mysql_affected_rows(conn->raw()) == 1
            && conn->execute("DELETE FROM auth_sessions WHERE user_id=" + std::to_string(expected.id))
            && tx.commit();
    }
    mysql_.release(conn);
    return ok;
}

bool UserRepository::create_auth_session(int user_id, int version, const std::string& id,
                                        const std::string& hash, int ttl) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    const bool ok = conn->execute("INSERT INTO auth_sessions (id,user_id,auth_version,refresh_hash,expires_at) "
        "SELECT '" + escape_sql(conn->raw(), id) + "',u.id,u.auth_version,'" + escape_sql(conn->raw(), hash)
        + "',TIMESTAMPADD(SECOND," + std::to_string(ttl) + ",UTC_TIMESTAMP()) FROM users u JOIN tenants t ON t.id=u.tenant_id "
        "WHERE u.id=" + std::to_string(user_id) + " AND u.auth_version=" + std::to_string(version)
        + " AND u.status='active' AND t.status='active' AND u.role IN ('admin','user')")
        && mysql_affected_rows(conn->raw()) == 1;
    mysql_.release(conn);
    return ok;
}

UserLookupStatus UserRepository::rotate_auth_session(const std::string& id, const std::string& old_hash,
                                                     const std::string& new_hash, LoginUser& user, int& ttl) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return UserLookupStatus::Unavailable;
    auto result = UserLookupStatus::NotFound;
    {
        MysqlTransaction tx(*conn);
        if (!tx.active()) result = UserLookupStatus::Unavailable;
        else {
            const auto key = escape_sql(conn->raw(), id);
            auto* res = conn->query("SELECT user_id FROM auth_sessions WHERE id='" + key + "'");
            if (!res) result = UserLookupStatus::Unavailable;
            auto* row = res ? mysql_fetch_row(res) : nullptr;
            const int user_id = row ? std::stoi(row[0]) : 0;
            if (res) mysql_free_result(res);
            const auto identity = user_id > 0 ? load_identity(*conn, user_id, user, true) : UserLookupStatus::NotFound;
            if (identity == UserLookupStatus::Unavailable) result = identity;
            if (identity == UserLookupStatus::Found
                && user.active && (user.role == "admin" || user.role == "user")) {
                const bool updated = conn->execute("UPDATE auth_sessions SET refresh_hash='" + escape_sql(conn->raw(), new_hash)
                    + "' WHERE id='" + key + "' AND user_id=" + std::to_string(user_id)
                    + " AND auth_version=" + std::to_string(user.auth_version) + " AND refresh_hash='"
                    + escape_sql(conn->raw(), old_hash) + "' AND expires_at>UTC_TIMESTAMP()");
                if (!updated) result = UserLookupStatus::Unavailable;
                else if (mysql_affected_rows(conn->raw()) == 1) {
                    res = conn->query("SELECT TIMESTAMPDIFF(SECOND,UTC_TIMESTAMP(),expires_at) FROM auth_sessions WHERE id='" + key + "'");
                    row = res ? mysql_fetch_row(res) : nullptr;
                    ttl = row ? std::stoi(row[0]) : 0;
                    if (res) mysql_free_result(res);
                    if (ttl > 0 && tx.commit()) result = UserLookupStatus::Found;
                    else result = UserLookupStatus::Unavailable;
                }
            }
        }
    }
    mysql_.release(conn);
    return result;
}

bool UserRepository::valid_auth_session(const std::string& id, int user_id, int version) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    auto* res = conn->query("SELECT 1 FROM auth_sessions WHERE id='" + escape_sql(conn->raw(), id)
        + "' AND user_id=" + std::to_string(user_id) + " AND auth_version=" + std::to_string(version)
        + " AND expires_at>UTC_TIMESTAMP()");
    const bool valid = res && mysql_fetch_row(res);
    if (res) mysql_free_result(res);
    mysql_.release(conn);
    return valid;
}

bool UserRepository::revoke_access_session(const std::string& id, int user_id, int version) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    const bool ok = conn->execute("DELETE FROM auth_sessions WHERE id='" + escape_sql(conn->raw(), id)
        + "' AND user_id=" + std::to_string(user_id) + " AND auth_version=" + std::to_string(version));
    mysql_.release(conn);
    return ok;
}

bool UserRepository::revoke_auth_session(const std::string& id, const std::string& hash) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;
    const bool ok = conn->execute("DELETE FROM auth_sessions WHERE id='" + escape_sql(conn->raw(), id)
        + "' AND refresh_hash='" + escape_sql(conn->raw(), hash) + "'");
    mysql_.release(conn);
    return ok;
}

}
