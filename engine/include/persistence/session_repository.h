#pragma once

#include <string>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct SessionSummary {
    std::string id;
    std::string title = "New Chat";
    std::string model;
    std::string created_at;
    std::string updated_at;
};

struct MessageSummary {
    int id = 0;
    std::string role;
    std::string content;
    std::string reasoning;
    std::string source_files = "[]";
    int tokens_in = 0;
    int tokens_out = 0;
    std::string created_at;
};

class SessionRepository {
public:
    explicit SessionRepository(MysqlPool& mysql);

    std::vector<SessionSummary> list_user_sessions(int tenant_id, int user_id, int limit = 50);
    bool create_session(const std::string& session_id, int tenant_id, int user_id,
                        const std::string& title, const std::string& model);
    bool get_session(const std::string& session_id, int tenant_id, int user_id,
                     SessionSummary& session);
    bool delete_session(const std::string& session_id, int tenant_id, int user_id);
    std::vector<MessageSummary> list_messages(const std::string& session_id, int limit = 200);

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
