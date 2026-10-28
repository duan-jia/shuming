#pragma once

#include <string>

#include "persistence/mysql_pool.h"

namespace fastllm {

class MessageRepository {
public:
    explicit MessageRepository(MysqlPool& mysql);

    bool insert_message(const std::string& session_id, const std::string& role,
                        const std::string& content, int tokens_in, int tokens_out);
    bool insert_chat_pair(const std::string& session_id,
                          const std::string& user_content,
                          const std::string& assistant_content,
                          int prompt_tokens, int completion_tokens, const std::string& reasoning = "",
                          const std::string& source_files = "[]", const std::string& attachments = "[]");

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
    static bool insert_message(MysqlConnection& conn, const std::string& session_id,
                               const std::string& role, const std::string& content,
                               int tokens_in, int tokens_out, const std::string& reasoning = "",
                               const std::string& source_files = "[]");
};

}
