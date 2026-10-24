#pragma once

#include <string>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct LoginUser {
    int id = 0;
    int tenant_id = 0;
    std::string username;
    std::string role = "user";
    std::string password_hash;
    bool active = false;
};

struct UserSummary {
    int id = 0;
    int tenant_id = 0;
    std::string username;
    std::string role = "user";
    std::string created_at;
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
                     int& user_id);
    bool create_guest_user(int tenant_id, const std::string& username, int& user_id);
    std::vector<UserSummary> list_users();
    bool delete_user(int user_id);

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
