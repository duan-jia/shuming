#include "agent/conversation_history.h"
#include "persistence/agent_repository.h"
#include "persistence/message_repository.h"
#include "persistence/session_repository.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace fastllm;

std::string setting(const char* name, const char* fallback) {
    const auto* value = std::getenv(name);
    return value ? value : fallback;
}

int main() {
    if (!std::getenv("MYSQL_TEST_HOST")) return 77;
    MysqlPool pool(setting("MYSQL_TEST_HOST", "127.0.0.1"), std::stoi(setting("MYSQL_TEST_PORT", "3306")),
        setting("MYSQL_TEST_USER", "root"), setting("MYSQL_TEST_PASSWORD", ""), "llm_platform", 2);
    assert(pool.initialize());
    SessionRepository sessions(pool);
    MessageRepository display(pool);
    AgentRepository journal(pool);
    assert(journal.schema_ready());
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto session = "journal-" + suffix;
    const auto request = "request-" + suffix;
    assert(sessions.create_session(session, 101, 202, "Journal test", "fixture"));
    assert(display.insert_chat_pair(session, "old question", "old answer", 1, 1));
    const nlohmann::json attachments = nlohmann::json::array({{{"doc_id", 7}, {"source_sha256", "fixture-sha"}}});
    assert(journal.begin(session, request, "fixture", "new question", attachments));
    ChatMessage call{"assistant", "Checking", "reason"};
    call.tool_calls = nlohmann::json::array({{{"id", "call"}, {"type", "function"},
        {"function", {{"name", "search_knowledge"}, {"arguments", "{}"}}}}});
    assert(journal.append(request, {{"type", "message"}, {"data", message_to_record(call)}}));
    ChatMessage result{"tool", "{\"content\":\"Evidence\",\"download_url\":\"secret\"}", ""};
    result.tool_call_id = "call";
    assert(journal.append(request, {{"type", "message"}, {"data", message_to_record(result)}}));
    assert(journal.append(request, {{"type", "message"}, {"data", message_to_record({"assistant", "new answer", ""})}}));
    assert(display.insert_chat_pair(session, "new question", "new answer", 10, 2, "", "[]", "[]", request));
    assert(journal.finish(request, "completed"));
    AgentRepository reloaded(pool);
    const auto history = reloaded.history(session);
    assert(history.size() == 6); // legacy pair + canonical four; display pair is not duplicated
    assert(history[1].content.find("tool use cannot be confirmed") != std::string::npos);
    assert(history[3].tool_calls[0]["id"] == history[4].tool_call_id);
    assert(history[4].content.find("secret") == std::string::npos);
    assert(reloaded.attachments(session) == attachments);
    assert(reloaded.requests(session, 101, 202).size() == 1);
    assert(reloaded.trace(session, request, 101, 203).empty());
    assert(reloaded.trace(session, request, 102, 202).empty());
    assert(reloaded.trace(session, request, 101, 202).size() == 4);
    assert(sessions.delete_session(session, 101, 202));
    assert(reloaded.trace(session, request, 101, 202).empty());
    std::cout << "PASS: mixed legacy/new history, durable metadata, no duplicate replay, redaction, owner/tenant isolation, cascade deletion\n";
}
