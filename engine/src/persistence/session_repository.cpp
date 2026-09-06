#include "persistence/session_repository.h"

#include <algorithm>

#include <mysql/mysql.h>

namespace fastllm {

SessionRepository::SessionRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::string SessionRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

std::vector<SessionSummary> SessionRepository::list_user_sessions(int tenant_id, int user_id, int limit) {
    std::vector<SessionSummary> result;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return result;

    std::string sql = "SELECT id, title, model, created_at, updated_at FROM sessions WHERE tenant_id="
        + std::to_string(tenant_id) + " AND user_id="
        + std::to_string(user_id) + " ORDER BY updated_at DESC LIMIT "
        + std::to_string(limit);
    auto* res = conn->query(sql);
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            SessionSummary s;
            s.id = row[0] ? row[0] : "";
            s.title = row[1] ? row[1] : "New Chat";
            s.model = row[2] ? row[2] : "";
            s.created_at = row[3] ? row[3] : "";
            s.updated_at = row[4] ? row[4] : "";
            result.push_back(s);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return result;
}

bool SessionRepository::create_session(const std::string& session_id, int tenant_id, int user_id,
                                       const std::string& title, const std::string& model) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "INSERT INTO sessions (id, tenant_id, user_id, title, model) VALUES ('"
        + escape_sql(conn->raw(), session_id) + "',"
        + std::to_string(tenant_id) + ","
        + std::to_string(user_id) + ",'"
        + escape_sql(conn->raw(), title) + "','"
        + escape_sql(conn->raw(), model) + "')";

    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

bool SessionRepository::get_session(const std::string& session_id, int tenant_id, int user_id,
                                    SessionSummary& session) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "SELECT id, title, model, created_at, updated_at FROM sessions WHERE id='"
        + escape_sql(conn->raw(), session_id) + "' AND tenant_id="
        + std::to_string(tenant_id) + " AND user_id="
        + std::to_string(user_id);
    auto* res = conn->query(sql);
    bool found = false;
    if (res) {
        auto* row = mysql_fetch_row(res);
        if (row) {
            session.id = row[0] ? row[0] : "";
            session.title = row[1] ? row[1] : "New Chat";
            session.model = row[2] ? row[2] : "";
            session.created_at = row[3] ? row[3] : "";
            session.updated_at = row[4] ? row[4] : "";
            found = true;
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return found;
}

bool SessionRepository::delete_session(const std::string& session_id, int tenant_id, int user_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "DELETE FROM sessions WHERE id='"
        + escape_sql(conn->raw(), session_id) + "' AND tenant_id="
        + std::to_string(tenant_id) + " AND user_id="
        + std::to_string(user_id);
    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

std::vector<MessageSummary> SessionRepository::list_messages(const std::string& session_id, int limit) {
    std::vector<MessageSummary> msgs;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return msgs;

    limit = std::max(1, limit);
    std::string sql =
        "SELECT id, role, content, reasoning, tokens_in, tokens_out, created_at "
        "FROM (SELECT id, role, content, reasoning, tokens_in, tokens_out, created_at "
        "FROM messages WHERE session_id='"
        + escape_sql(conn->raw(), session_id) + "' ORDER BY id DESC LIMIT "
        + std::to_string(limit) + ") AS recent_messages ORDER BY id ASC";
    auto* res = conn->query(sql);
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            MessageSummary m;
            m.id = row[0] ? std::stoi(row[0]) : 0;
            m.role = row[1] ? row[1] : "";
            m.content = row[2] ? row[2] : "";
            m.reasoning = row[3] ? row[3] : "";
            m.tokens_in = row[4] ? std::stoi(row[4]) : 0;
            m.tokens_out = row[5] ? std::stoi(row[5]) : 0;
            m.created_at = row[6] ? row[6] : "";
            msgs.push_back(m);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return msgs;
}

}
