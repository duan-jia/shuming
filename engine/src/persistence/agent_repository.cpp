#include "persistence/agent_repository.h"

#include "agent/conversation_history.h"
#include "persistence/mysql_transaction.h"

#include <algorithm>
#include <stdexcept>

namespace fastllm {

std::string AgentRepository::quote(MYSQL* mysql, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    const auto size = mysql_real_escape_string(mysql, escaped.data(), value.data(), value.size());
    escaped.resize(size);
    return "'" + escaped + "'";
}

bool AgentRepository::schema_ready() {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) return false;
    for (const auto& sql : {"SELECT request_id FROM messages LIMIT 0",
                            "SELECT request_id,status FROM agent_requests LIMIT 0",
                            "SELECT request_id,event_type,payload FROM agent_events LIMIT 0"}) {
        auto* res = conn->query(sql);
        if (!res) return false;
        mysql_free_result(res);
    }
    return true;
}

bool AgentRepository::begin(const std::string& session, const std::string& request,
                            const std::string& model, const std::string& prompt, const nlohmann::json& attachments) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) return false;
    MysqlTransaction tx(*conn);
    if (!tx.active()) return false;
    const auto q = [&](const std::string& value) { return quote(conn->raw(), value); };
    auto message = message_to_record({"user", prompt, ""});
    message["attachments"] = attachments;
    return conn->execute("INSERT INTO agent_requests(request_id,session_id,model) VALUES("
        + q(request) + "," + q(session) + "," + q(model) + ")")
        && conn->execute("INSERT INTO agent_events(request_id,event_type,payload) VALUES("
            + q(request) + ",'message'," + q(trace_payload(message).dump()) + ")") && tx.commit();
}

bool AgentRepository::append(const std::string& request, const nlohmann::json& event) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) return false;
    const auto payload = trace_payload(event.value("data", nlohmann::json::object()));
    const auto serialized = payload.dump();
    // Fail visibly instead of silently retaining a different model transcript.
    if (serialized.size() > 8 * 1024 * 1024) return false;
    return conn->execute("INSERT INTO agent_events(request_id,event_type,payload) VALUES("
        + quote(conn->raw(), request) + "," + quote(conn->raw(), event.at("type").get<std::string>())
        + "," + quote(conn->raw(), serialized) + ")");
}

bool AgentRepository::finish(const std::string& request, const std::string& status) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    return conn && conn->execute("UPDATE agent_requests SET status=" + quote(conn->raw(), status)
        + " WHERE request_id=" + quote(conn->raw(), request));
}

std::vector<ChatMessage> AgentRepository::history(const std::string& session) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) throw std::runtime_error("Conversation journal unavailable");
    const auto qsession = quote(conn->raw(), session);
    // Limit requests, not individual events, so a retained batch is never split.
    // Legacy rows remain readable but explicitly carry no tool execution evidence.
    const auto sql = "SELECT payload,request_id FROM ("
        "SELECT JSON_OBJECT('role',m.role,'content',m.content,'reasoning_content',COALESCE(m.reasoning,'')) payload,"
        "'' request_id,m.created_at started,m.id ordinal FROM "
        "(SELECT * FROM messages WHERE session_id=" + qsession
        + " AND request_id IS NULL ORDER BY id DESC LIMIT 200) m UNION ALL "
        "SELECT e.payload,r.request_id,r.created_at started,e.id ordinal FROM agent_events e JOIN "
        "(SELECT request_id,created_at FROM agent_requests WHERE session_id=" + qsession
        + " ORDER BY created_at DESC,request_id DESC LIMIT 100) r ON e.request_id=r.request_id "
        "WHERE e.event_type='message') h ORDER BY started,ordinal";
    auto* res = conn->query(sql);
    if (!res) throw std::runtime_error("Cannot load conversation journal; apply the Agent migration");
    std::vector<ChatMessage> messages;
    try {
        while (auto row = mysql_fetch_row(res)) {
            auto message = message_from_record(nlohmann::json::parse(row[0]), row[1] ? row[1] : "");
            if (message.request_id.empty() && message.role == "assistant")
                message.content += "\n[Legacy turn: tool execution records were not retained; tool use cannot be confirmed.]";
            messages.push_back(std::move(message));
        }
    } catch (...) { mysql_free_result(res); throw; }
    mysql_free_result(res);
    return messages;
}

nlohmann::json AgentRepository::attachments(const std::string& session) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) throw std::runtime_error("Conversation journal unavailable");
    auto* res = conn->query("SELECT e.payload FROM agent_events e JOIN "
        "(SELECT request_id FROM agent_requests WHERE session_id=" + quote(conn->raw(), session)
        + " ORDER BY created_at DESC,request_id DESC LIMIT 100) r ON r.request_id=e.request_id "
        "WHERE e.event_type='message' AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.role'))='user' ORDER BY e.id");
    if (!res) throw std::runtime_error("Cannot load journal attachments");
    auto refs = nlohmann::json::array();
    try {
        while (auto row = mysql_fetch_row(res)) {
            const auto message = nlohmann::json::parse(row[0]);
            for (const auto& ref : message.value("attachments", nlohmann::json::array())) refs.push_back(ref);
        }
    } catch (...) { mysql_free_result(res); throw; }
    mysql_free_result(res);
    return refs;
}

nlohmann::json AgentRepository::requests(const std::string& session, int tenant, int user) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) throw std::runtime_error("Conversation journal unavailable");
    auto* res = conn->query("SELECT r.request_id,r.model,r.status,r.created_at,r.updated_at FROM agent_requests r "
        "JOIN sessions s ON s.id=r.session_id WHERE s.id=" + quote(conn->raw(), session)
        + " AND s.tenant_id=" + std::to_string(tenant) + " AND s.user_id=" + std::to_string(user)
        + " ORDER BY r.created_at DESC,r.request_id DESC LIMIT 100");
    if (!res) throw std::runtime_error("Cannot load request journal");
    auto result = nlohmann::json::array();
    while (auto row = mysql_fetch_row(res)) result.push_back({{"request_id", row[0]}, {"model", row[1]},
        {"status", row[2]}, {"created_at", row[3]}, {"updated_at", row[4]}});
    mysql_free_result(res);
    return result;
}

nlohmann::json AgentRepository::trace(const std::string& session, const std::string& request, int tenant, int user) {
    auto conn = mysql_.acquire();
    MysqlGuard guard(mysql_, conn);
    if (!conn) throw std::runtime_error("Conversation journal unavailable");
    auto* res = conn->query("SELECT e.id,e.event_type,e.payload,e.created_at FROM agent_events e "
        "JOIN agent_requests r ON r.request_id=e.request_id JOIN sessions s ON s.id=r.session_id "
        "WHERE s.id=" + quote(conn->raw(), session) + " AND r.request_id=" + quote(conn->raw(), request)
        + " AND s.tenant_id=" + std::to_string(tenant) + " AND s.user_id=" + std::to_string(user)
        + " ORDER BY e.id");
    if (!res) throw std::runtime_error("Cannot load execution trace");
    auto result = nlohmann::json::array();
    try {
        while (auto row = mysql_fetch_row(res)) result.push_back({{"id", std::stoll(row[0])}, {"type", row[1]},
            {"data", nlohmann::json::parse(row[2])}, {"created_at", row[3]}});
    } catch (...) { mysql_free_result(res); throw; }
    mysql_free_result(res);
    return result;
}

} // namespace fastllm
