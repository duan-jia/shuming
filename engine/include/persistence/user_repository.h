#pragma once

#include <string>
#include <optional>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

// Largest integer exactly representable by the JSON management UI.
inline constexpr long long kMaxDailyTokenQuota = 9007199254740991LL;

struct LoginUser {
    int id = 0;
    int tenant_id = 0;
    std::string username;
    std::string role = "user";
    std::string password_hash;
    bool active = false;
    int auth_version = 0;
    bool must_change_password = false;
};

struct UserSummary {
    int id = 0;
    int tenant_id = 0;
    std::string username;
    std::string role = "user";
    std::string created_at;
    bool must_change_password = false;
    std::optional<long long> daily_token_quota;
    long long effective_daily_token_quota = 0;
    long long today_token_limit = 0;
    long long today_tokens_used = 0;
};

enum class UserLookupStatus {
    Found,
    NotFound,
    Unavailable
};

class UserRepository {
public:
    explicit UserRepository(MysqlPool& mysql);

    UserLookupStatus find_login_user(const std::string& username, LoginUser& user);
    bool create_user(int tenant_id, const std::string& username,
                     const std::string& password_hash, const std::string& role,
                     int& user_id, std::optional<long long> daily_token_quota = std::nullopt, bool must_change_password = false);
    UserLookupStatus set_daily_token_quota(int user_id, std::optional<long long> quota);
    bool create_guest_user(int tenant_id, const std::string& username, int& user_id);
    std::vector<UserSummary> list_users();
    bool delete_user(int user_id);
    UserLookupStatus find_user_by_id(int user_id, LoginUser& user);
    bool change_password(const LoginUser& expected, const std::string& hash, bool must_change);
    bool create_auth_session(int user_id, int version, const std::string& id,
                             const std::string& hash, int ttl);
    UserLookupStatus rotate_auth_session(const std::string& id, const std::string& old_hash,
                                         const std::string& new_hash, LoginUser& user, int& ttl);
    bool valid_auth_session(const std::string& id, int user_id, int version);
    bool revoke_access_session(const std::string& id, int user_id, int version);
    bool revoke_auth_session(const std::string& id, const std::string& hash);


private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
