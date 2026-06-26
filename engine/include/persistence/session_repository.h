#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "persistence/mysql_pool.h"

namespace fastllm {

class SessionRepository {
public:
    explicit SessionRepository(MysqlPool& mysql);

    nlohmann::json list_user_sessions(int tenant_id, int user_id, int limit = 50);
    bool create_session(const std::string& session_id, int tenant_id, int user_id,
                        const std::string& title, const std::string& model);
    bool get_session(const std::string& session_id, int tenant_id, int user_id,
                     nlohmann::json& session);
    bool delete_session(const std::string& session_id, int tenant_id, int user_id);
    nlohmann::json list_messages(const std::string& session_id, int limit = 200);

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
