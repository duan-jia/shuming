#include "persistence/message_repository.h"

#include "persistence/mysql_transaction.h"

#include <mysql/mysql.h>

namespace fastllm {

MessageRepository::MessageRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::string MessageRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

bool MessageRepository::insert_message(MysqlConnection& conn, const std::string& session_id,
                                       const std::string& role, const std::string& content,
                                       int tokens_in, int tokens_out, const std::string& reasoning, const std::string& source_files) {
    std::string sql = "INSERT INTO messages (session_id, role, content, reasoning, rag_references, tokens_in, tokens_out) VALUES ('"
        + escape_sql(conn.raw(), session_id) + "','"
        + escape_sql(conn.raw(), role) + "','"
        + escape_sql(conn.raw(), content) + "','" + escape_sql(conn.raw(), reasoning) + "','" + escape_sql(conn.raw(), source_files) + "',"
        + std::to_string(tokens_in) + ","
        + std::to_string(tokens_out) + ")";
    return conn.execute(sql);
}

bool MessageRepository::insert_message(const std::string& session_id, const std::string& role,
                                       const std::string& content, int tokens_in, int tokens_out) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    bool ok = insert_message(*conn, session_id, role, content, tokens_in, tokens_out);
    mysql_.release(conn);
    return ok;
}

bool MessageRepository::insert_chat_pair(const std::string& session_id,
                                         const std::string& user_content,
                                         const std::string& assistant_content,
                                         int prompt_tokens, int completion_tokens, const std::string& reasoning, const std::string& source_files, const std::string& attachments) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    MysqlTransaction tx(*conn);
    if (!tx.active()) {
        mysql_.release(conn);
        return false;
    }

    bool ok = insert_message(*conn, session_id, "user", user_content, prompt_tokens, 0, "", attachments)
        && insert_message(*conn, session_id, "assistant", assistant_content, 0, completion_tokens, reasoning, source_files)
        && tx.commit();

    if (!ok) tx.rollback();

    mysql_.release(conn);
    return ok;
}

}
